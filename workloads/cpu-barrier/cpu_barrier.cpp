// cpu_barrier: the CPU baseline for the barrier study -- a cluster of 64-core processors, hybrid MPI + threads.
//
//   cpu_barrier --threads N [--cores-per-proc 64] [--rounds 20] [--warmup 50] [--check]
//
// Inside one processor the threads share memory, and the barrier is the textbook centralized sense-reversing barrier
// (Mellor-Crummey & Scott, TOCS'91, Fig. 8), spinning on coherent caches: every thread flips its local sense and
// atomically increments a shared count; the last to arrive resets the count and publishes the new sense; everybody
// else spins, with pause, until the published sense equals its own. The count and the sense each have a cache line
// to themselves.
//
// Across processors the program is a hybrid MPI job with one rank per processor, written the way hybrid codes write a
// global barrier (MPI_THREAD_FUNNELED: only the rank's main thread calls MPI):
//
//     thread barrier;  if (main thread) MPI_Barrier(COMM_WORLD);  thread barrier;
//
// With a single processor there is no MPI and a barrier is one thread barrier.
//
// --barrier tree replaces both with Mellor-Crummey & Scott's tree barrier (TOCS'91, the "tree-based barrier"): each
// thread has its own record on its own cache line; arrival climbs a 4-ary tree (a child clears its byte in its
// parent's childnotready word, the parent spins on that word), wake-up descends a binary tree (a parent writes its two
// children's parentsense flags). No atomics, and no line is written by more than four threads. The hybrid version runs
// the local arrival tree, then only the processor's root thread (its thread 0) calls MPI_Barrier with the other
// processors' roots, then the local wake-up tree -- one local pass per barrier, not two:
//
//     local arrival tree;  if (root) MPI_Barrier(COMM_WORLD);  local wake-up tree;
//
// MPI_Barrier is OpenMPI 2.1's (the version in the simulation container) own algorithm choice, coll/tuned's fixed
// decision: two ranks exchange once, a power-of-two communicator uses recursive doubling, anything else Bruck's
// dissemination. Our rank counts are powers of two, so it is recursive doubling: log2(P) rounds, in round k a zero-byte
// send-receive with rank ^ 2^k. The messages ride zsim's cluster network model (src/cluster_net.h: one NIC port per
// processor, an end-to-end latency that includes the MPI library's send and receive overhead, a port bandwidth), set
// in the config's sys.cluster. Everything else -- the thread barriers, the leader's bookkeeping -- is real code on
// real simulated cores and caches.
//
// Each thread records when it arrived at each barrier and when it came out, on its own core clock (zsim_now); the
// main thread of processor 0 also records when its two thread barriers and its MPI_Barrier ended, which splits a
// barrier into its three parts. The reported period is (last arrival - first arrival) / barriers, the metric the
// rest of the study uses.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;

struct Sample {
    uint64_t arrive = 0;
    uint64_t release = 0;
#ifdef BREAKDOWN
    uint64_t xadd_done = 0;  // this thread's lock xadd completed
    uint64_t flip_done = 0;  // last arriver only: its sense store completed
    uint32_t order = 0;      // the value xadd returned: this thread's place in the arrival order
#endif
};

// Main thread of processor 0 only: when each part of a barrier ended.
struct Split {
    uint64_t intra1 = 0;  // first thread barrier
    uint64_t mpi = 0;     // MPI_Barrier
};

// One processor's shared barrier state. The count and the sense each get their own line, so the spinners' copies of
// the sense are invalidated once per barrier (by the release) and not once per arrival.
struct alignas(kLine) ProcBarrier {
    volatile uint32_t count;
    char pad0[kLine - sizeof(uint32_t)];
    volatile uint32_t sense;
    char pad1[kLine - sizeof(uint32_t)];
};

uint32_t g_threads = 1, g_cores_per_proc = 64, g_procs = 1;
ProcBarrier* g_proc = nullptr;  // [g_procs]
bool g_tree = false;            // --barrier tree
bool g_check = false;
volatile uint64_t* g_arrivals = nullptr;  // --check: one global counter
volatile uint64_t g_violations = 0;

volatile uint64_t g_registered = 0;
volatile uint64_t g_done = 0;

uint32_t core_id() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

// Wait without spinning and without blocking, for the thread-creation gate and nothing else: blocking would hand the
// core to another thread, spinning would put millions of gate instructions on the cores before the measurement.
void park_until(volatile uint64_t* counter, uint64_t target) {
    while (*counter < target) zsim_park();
}

#ifdef BREAKDOWN
// A breakdown timestamp: fenced, and behind a call so the basic block before it (the xadd, the stores) has been
// simulated when the clock is read; on the global timeline so threads can be compared.
__attribute__((noinline)) uint64_t stamp() { return zsim_now_abs(); }
#define STAMP() (__sync_synchronize(), stamp())
#else
#define STAMP() zsim_now()
#endif

inline void cpu_relax() { __asm__ __volatile__("pause" ::: "memory"); }

// The thread barrier: centralized, sense-reversing, spinning.
//
// BREAKDOWN builds (cpu_barrier_bd) timestamp the xadd and the release store; the timing binary has none of it.
void thread_barrier(ProcBarrier* b, uint32_t n, uint32_t* local_sense, Sample* rec = nullptr) {
#ifdef ABL_EMPTY
    return;  // ablation: the loop around the barrier only
#endif
    *local_sense ^= 1u;
#ifdef ABL_NOLOCK
    uint32_t order = b->count;  // ablation: the same arithmetic without the lock (correct for one thread only)
    b->count = order + 1;
#else
    uint32_t order = __sync_fetch_and_add(&b->count, 1u);
#endif
#ifdef BREAKDOWN
    if (rec) { rec->xadd_done = STAMP(); rec->order = order; }
#else
    (void)rec;
#endif
    if (order == n - 1) {
        b->count = 0;
        __sync_synchronize();
        b->sense = *local_sense;
#ifdef BREAKDOWN
        if (rec) rec->flip_done = STAMP();
#endif
    } else {
        while (b->sense != *local_sense) cpu_relax();
    }
}

// ------------------------------------------------------------------------------------ MCS tree barrier ---
// One record per thread, padded to a line. Fields exactly as in the paper: the 4-ary arrival tree's childnotready
// bytes (packed in one word, so the parent spins on one location), the binary wake-up tree's parentsense, and
// pointers to the parent's byte and the two wake-up children's flags.
struct alignas(kLine) MCSNode {
    union {
        volatile uint8_t childnotready[4];
        volatile uint32_t childnotready_all;
    };
    uint32_t havechild_all;             // havechild[0..3] as bytes: 1 if that arrival child exists
    volatile uint32_t parentsense;
    volatile uint8_t* parentpointer;    // my byte in my parent's childnotready (the dummy for the root)
    volatile uint32_t* childpointers[2];  // my wake-up children's parentsense (the dummy if absent)
    volatile uint32_t dummy;
};

MCSNode* g_nodes = nullptr;  // [g_procs * g_cores_per_proc]

// Wire processor `proc`'s n threads, as the paper initialises them.
void mcs_init(MCSNode* nodes, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        MCSNode& v = nodes[i];
        v.havechild_all = 0;
        for (uint32_t j = 0; j < 4; j++)
            if (4 * i + j + 1 < n) v.havechild_all |= 1u << (8 * j);
        v.childnotready_all = v.havechild_all;
        v.parentsense = 0;
        v.dummy = 0;
        v.parentpointer = i ? &nodes[(i - 1) / 4].childnotready[(i - 1) % 4] : (volatile uint8_t*)&v.dummy;
        v.childpointers[0] = (2 * i + 1 < n) ? &nodes[2 * i + 1].parentsense : &v.dummy;
        v.childpointers[1] = (2 * i + 2 < n) ? &nodes[2 * i + 2].parentsense : &v.dummy;
    }
}

// The paper's tree_barrier, with the hybrid hook: the root runs `at_root` (the MPI_Barrier) after its whole
// processor has arrived and before it starts the wake-up. `sense` is private, initially 1.
template <typename F>
void mcs_tree_barrier(MCSNode* v, bool root, uint32_t* sense, F at_root) {
    while (v->childnotready_all != 0) cpu_relax();  // wait for my arrival children
    v->childnotready_all = v->havechild_all;        // re-arm for the next barrier
    *v->parentpointer = 0;                          // tell my parent I am here
    if (root) at_root();
    else while (v->parentsense != *sense) cpu_relax();  // wait for the wake-up
    *v->childpointers[0] = *sense;                  // wake my two children
    *v->childpointers[1] = *sense;
    *sense ^= 1u;
}

// ------------------------------------------------------------------------------------------ MPI_Barrier ---
// A message is (barrier number, round). A partner can be at most one barrier ahead of us -- it cannot finish barrier
// e+1 without our round-0 message of e+1 -- so received messages are kept per barrier parity and round.
constexpr uint32_t kMaxRounds = 32;

struct Rank {
    uint32_t rank = 0, size = 1;
    uint32_t epoch = 0;
    uint8_t got[2][kMaxRounds] = {};
};

inline uint32_t leader_core(uint32_t rank) { return rank * g_cores_per_proc; }
inline uint64_t mpi_tag(uint32_t epoch, uint32_t round) { return ((uint64_t)(epoch & 0xFFFFFF) << 8) | round; }

// Blocking receive of (epoch, round), from whoever the recursive-doubling partner is: messages that are not the one
// asked for are kept for later.
void mpi_recv(Rank* r, uint32_t epoch, uint32_t round) {
    while (!r->got[epoch & 1][round]) {
        uint64_t w = zsim_net_recv();
        if (!w) continue;  // inbox empty: the core was parked to the end of the phase
        uint64_t tag = ZSIM_NET_TAG(w);
        uint32_t e = (uint32_t)(tag >> 8), k = (uint32_t)(tag & 0xFF);
        if (e != (epoch & 0xFFFFFF) && e != ((epoch + 1) & 0xFFFFFF)) {
            fprintf(stderr, "cpu_barrier: rank %u got a message of barrier %u round %u during barrier %u\n", r->rank, e,
                    k, epoch);
            exit(3);
        }
        r->got[e & 1][k] = 1;
    }
    r->got[epoch & 1][round] = 0;
}

// ompi_coll_base_barrier_intra_recursivedoubling, for a power-of-two communicator: in round k, sendrecv with
// rank ^ 2^k. (OpenMPI's two_procs algorithm for two ranks is the same single exchange.)
void mpi_barrier(Rank* r) {
    uint32_t e = r->epoch++;
    uint32_t round = 0;
    for (uint32_t mask = 1; mask < r->size; mask <<= 1, round++) {
        uint32_t peer = r->rank ^ mask;
        zsim_net_send(leader_core(peer), mpi_tag(e, round));
        mpi_recv(r, e, round);
    }
}

// ------------------------------------------------------------------------------------------------ threads ---
struct Task {
    pthread_t th;
    uint32_t expect_core = 0, core = (uint32_t)-1, core_after_gate = (uint32_t)-1;
    uint32_t rounds = 0;
    Sample* samples = nullptr;
    Split* split = nullptr;  // main thread of processor 0 only
};

void worker(Task* t) {
    uint32_t proc = t->expect_core / g_cores_per_proc, local = t->expect_core % g_cores_per_proc;
    uint32_t n = std::min(g_cores_per_proc, g_threads - proc * g_cores_per_proc);
    ProcBarrier* b = &g_proc[proc];
    uint32_t sense = 0;
    bool leader = (local == 0);
    Rank rank;
    rank.rank = proc;
    rank.size = g_procs;
    MCSNode* node = g_nodes ? &g_nodes[proc * g_cores_per_proc + local] : nullptr;
    uint32_t tree_sense = 1;
    for (uint32_t s = 0; s < t->rounds; s++) {
        t->samples[s].arrive = STAMP();
        if (g_check) __sync_fetch_and_add(g_arrivals, 1);
        if (g_tree) {
            Split* sp = t->split ? &t->split[s] : nullptr;
            mcs_tree_barrier(node, leader, &tree_sense, [&]() {
                if (sp) sp->intra1 = zsim_now();       // the processor has arrived
                if (g_procs > 1) mpi_barrier(&rank);
                if (sp) sp->mpi = zsim_now();
            });
            t->samples[s].release = STAMP();
            if (g_check && *g_arrivals < (uint64_t)g_threads * (s + 1)) __sync_fetch_and_add(&g_violations, 1);
            continue;
        }
        thread_barrier(b, n, &sense, &t->samples[s]);
        if (g_procs > 1) {
            if (t->split) t->split[s].intra1 = zsim_now();
            if (leader) mpi_barrier(&rank);
            if (t->split) t->split[s].mpi = zsim_now();
            thread_barrier(b, n, &sense);
        }
        t->samples[s].release = STAMP();
        if (g_check && *g_arrivals < (uint64_t)g_threads * (s + 1)) __sync_fetch_and_add(&g_violations, 1);
    }
    __sync_fetch_and_add(&g_done, 1);
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_threads);  // everybody has a core before anybody starts
    t->core_after_gate = core_id();
    worker(t);
    return nullptr;
}

double mean(const std::vector<double>& v) {
    double t = 0.0;
    for (double x : v) t += x;
    return v.empty() ? 0.0 : t / v.size();
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t rounds = 20, warmup = 50;
    bool trace = false;  // --trace: processor 0's main-thread timestamps, every barrier
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) exit(1); return argv[++i]; };
        if (!strcmp(k, "--threads")) g_threads = atoi(val());
        else if (!strcmp(k, "--cores-per-proc")) g_cores_per_proc = atoi(val());
        else if (!strcmp(k, "--rounds")) rounds = atoi(val());
        else if (!strcmp(k, "--warmup")) warmup = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
        else if (!strcmp(k, "--trace")) trace = true;
        else if (!strcmp(k, "--barrier")) {
            const char* v = val();
            if (!strcmp(v, "tree")) g_tree = true;
            else if (strcmp(v, "central")) { fprintf(stderr, "cpu_barrier: --barrier central|tree\n"); return 1; }
        }
        else {
            fprintf(stderr, "usage: cpu_barrier --threads N [--cores-per-proc 64] [--rounds 20] [--warmup 50] "
                            "[--check] [--barrier central|tree]\n");
            return 1;
        }
    }
    if (!g_threads || !g_cores_per_proc || rounds < 2) { fprintf(stderr, "cpu_barrier: bad arguments\n"); return 1; }
    g_procs = (g_threads + g_cores_per_proc - 1) / g_cores_per_proc;
    if (g_procs > 1 && (g_threads % g_cores_per_proc || (g_procs & (g_procs - 1)))) {
        fprintf(stderr, "cpu_barrier: beyond one processor, --threads must fill whole processors and give a power-of-two "
                        "rank count (recursive doubling)\n");
        return 1;
    }
    uint32_t total = warmup + rounds;
    printf("cpu_barrier: %u threads on %u processor(s) of %u cores, %u rounds (+%u warmup), %s\n", g_threads, g_procs,
           g_cores_per_proc, rounds, warmup,
           g_tree ? (g_procs > 1 ? "MCS tree arrival + root-only MPI_Barrier (recursive doubling) + MCS tree wake-up"
                                 : "MCS tree barrier")
                  : (g_procs > 1 ? "thread barrier + MPI_Barrier (recursive doubling) + thread barrier"
                                 : "thread barrier"));

    void* mem = nullptr;
    if (posix_memalign(&mem, 4096, (size_t)g_procs * sizeof(ProcBarrier)) != 0) return 1;
    g_proc = (ProcBarrier*)mem;
    memset(g_proc, 0, (size_t)g_procs * sizeof(ProcBarrier));
    if (g_tree) {
        size_t bytes = (size_t)g_procs * g_cores_per_proc * sizeof(MCSNode);
        if (posix_memalign(&mem, 4096, bytes) != 0) return 1;
        memset(mem, 0, bytes);
        g_nodes = (MCSNode*)mem;
        for (uint32_t pr = 0; pr < g_procs; pr++)
            mcs_init(&g_nodes[pr * g_cores_per_proc], std::min(g_cores_per_proc, g_threads - pr * g_cores_per_proc));
    }
    uint64_t check_count = 0;
    g_arrivals = &check_count;

    std::vector<Task> tasks(g_threads);
    std::vector<Split> split(total);
    for (uint32_t c = 0; c < g_threads; c++) {
        Task& t = tasks[c];
        t.expect_core = c;
        t.rounds = total;
        // Each thread's samples on lines of its own, so recording them is private to the thread's caches.
        if (posix_memalign(&mem, kLine, (size_t)total * sizeof(Sample)) != 0) return 1;
        t.samples = (Sample*)mem;
        memset(t.samples, 0, (size_t)total * sizeof(Sample));
    }
    tasks[0].split = split.data();

    zsim_roi_begin();
    // Thread 0 is this thread, on core 0; the rest are created one at a time so core ids follow creation order, and
    // each checks the core it got.
    for (uint32_t c = 1; c < g_threads; c++) {
        if (pthread_create(&tasks[c].th, nullptr, thread_main, &tasks[c]) != 0) { perror("pthread_create"); return 1; }
        park_until(&g_registered, c);
    }
    thread_main(&tasks[0]);
    for (uint32_t c = 1; c < g_threads; c++) pthread_join(tasks[c].th, nullptr);
    zsim_roi_end();

    bool ok = true;
    for (uint32_t c = 0; c < g_threads; c++) {
        if (tasks[c].core != c || tasks[c].core_after_gate != c) ok = false;
    }
    printf("CPUBAR layout %s\n", ok ? "OK" : "MISMATCH");

    std::vector<double> waits, periods;
    for (uint32_t c = 0; c < g_threads; c++) {
        Task& t = tasks[c];
        std::vector<double> w;
        for (uint32_t s = warmup; s < total; s++) w.push_back((double)(t.samples[s].release - t.samples[s].arrive));
        waits.push_back(mean(w));
        periods.push_back((double)(t.samples[total - 1].arrive - t.samples[warmup].arrive) / (rounds - 1));
    }
    printf("CPUBAR barrier=%s threads=%u procs=%u cores_per_proc=%u rounds=%u warmup=%u\n", g_tree ? "tree" : "central",
           g_threads, g_procs, g_cores_per_proc, rounds, warmup);
    printf("CPUBAR period_cycles=%.1f period_min=%.1f period_max=%.1f\n", median(periods),
           *std::min_element(periods.begin(), periods.end()), *std::max_element(periods.begin(), periods.end()));
    // Means, not medians: two ranks can settle into alternating long and short waits, and only means add up to the
    // period (which is itself a mean).
    printf("CPUBAR wait_last_cycles=%.1f wait_first_cycles=%.1f wait_mean_cycles=%.1f\n",
           *std::min_element(waits.begin(), waits.end()), *std::max_element(waits.begin(), waits.end()), mean(waits));
#ifdef BREAKDOWN
    // One processor only: split every period into five consecutive parts, from all threads' timestamps. The parts
    // telescope, so their means add up to the mean period exactly.
    //   enter    first arrival -> first xadd done       (the first thread's xadd: an L3 transfer)
    //   chain    first xadd done -> last xadd done      (the other n-1 xadds, one ownership transfer after another)
    //   release  last xadd done -> sense store done     (last arriver: count = 0, fence, sense = ~sense, fence)
    //   wake     sense store done -> last thread out    (spinners re-read sense: downgrade of the writer, then reads)
    //   outside  last thread out -> next first arrival  (the loop around the barrier: timestamps, sample stores)
    if (g_procs == 1) {
        std::vector<double> enter, chain, rel, wake, outside, per;
        for (uint32_t s = warmup; s + 1 < total; s++) {
            uint64_t a0 = UINT64_MAX, a1 = UINT64_MAX, x0 = UINT64_MAX, x1 = 0, flip = 0, out = 0;
            for (uint32_t c = 0; c < g_threads; c++) {
                const Sample& m = tasks[c].samples[s];
                a0 = std::min(a0, m.arrive);
                a1 = std::min(a1, tasks[c].samples[s + 1].arrive);
                x0 = std::min(x0, m.xadd_done);
                x1 = std::max(x1, m.xadd_done);
                if (m.order == g_threads - 1) flip = m.flip_done;
                out = std::max(out, m.release);
            }
            enter.push_back((double)(x0 - a0));
            chain.push_back((double)(x1 - x0));
            rel.push_back((double)(flip - x1));
            wake.push_back((double)(out - flip));
            outside.push_back((double)(a1 - out));
            per.push_back((double)(a1 - a0));
        }
        if (g_threads <= 4) {
            for (uint32_t s = warmup; s < warmup + 6; s++)
                for (uint32_t c = 0; c < g_threads; c++) {
                    const Sample& m = tasks[c].samples[s];
                    printf("RAW s=%u c=%u arrive=%lu xadd=%lu order=%u flip=%lu release=%lu\n", s, c, m.arrive,
                           m.xadd_done, m.order, m.flip_done, m.release);
                }
        }
        printf("BREAKDOWN threads=%u period=%.1f enter=%.1f chain=%.1f chain_per_xadd=%.1f release=%.1f wake=%.1f "
               "outside=%.1f\n", g_threads, mean(per), mean(enter), mean(chain),
               g_threads > 1 ? mean(chain) / (g_threads - 1) : 0.0, mean(rel), mean(wake), mean(outside));
    }
#endif
    if (g_procs > 1) {
        // Processor 0's main thread: arrival -> end of first thread barrier -> end of MPI_Barrier -> release.
        std::vector<double> a, m, b;
        for (uint32_t s = warmup; s < total; s++) {
            a.push_back((double)(split[s].intra1 - tasks[0].samples[s].arrive));
            m.push_back((double)(split[s].mpi - split[s].intra1));
            b.push_back((double)(tasks[0].samples[s].release - split[s].mpi));
        }
        if (trace) {
            for (uint32_t s = 0; s < total; s++)
                printf("TRACE %u arrive=%lu intra1=%lu mpi=%lu release=%lu\n", s, tasks[0].samples[s].arrive,
                       split[s].intra1, split[s].mpi, tasks[0].samples[s].release);
        }
        printf("CPUBAR split_intra1_cycles=%.1f split_mpi_cycles=%.1f split_intra2_cycles=%.1f\n", mean(a), mean(m),
               mean(b));
    }
    if (g_check) {
        printf("CHECK arrivals-before-release violations=%lu over %u rounds x %u threads\n", (uint64_t)g_violations,
               total, g_threads);
        if (g_violations) ok = false;
    }
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or a thread released before every arrival");
    printf("CPUBAR done\n");
    return ok ? 0 : 2;
}
