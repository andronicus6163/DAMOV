// collective: all-reduce throughput -- every client contributes a value, one result comes back.
//
//   collective --clients N --ops K [--units 4 --cores-per-unit 16 --stack-mb 1024] [--check]
//   collective --clients N --ops K --cpus 0,1,2,...        (native build)
//
// One round: every client writes its value to its own slot, all clients meet at a barrier, then client 0
// sums the slots and publishes the result, and a second barrier releases everyone to read it. K rounds.
//
// WHAT THIS IS MEASURING. SynCron's engine has one collective, the barrier -- there is no value-carrying
// reduction or broadcast opcode. A reduction therefore has to be built out of the barrier plus ordinary
// memory, which is the oldest trick there is and also the only one available: contribute, barrier, one
// core folds, barrier, read. Two barriers and a serial fold per round.
//
// On UpDown the comparison is the Broadcast tree that libraries/sync-primitives wraps: a push tree whose
// acknowledgement path sums as it returns, so the fold happens on the way back up rather than on one core
// afterwards. That tree is limited to sum and to four reduced values, which is a real limitation and is
// reported as one -- but it does not serialise the fold.
//
// The serial fold is deliberately left serial. A software tree reduction would be a fairer algorithm and a
// worse measurement of what the engine provides, because the engine provides none of it either way; the
// point here is the cost of having no reduction primitive, and the fold is only N adds.
//
// THREE BACKENDS, one source:
//   default       SynCron: the SE barrier, across units when the clients span more than one.
//   -DCL_SPIN     the simulated processor: a sense-reversing centralised barrier in shared memory.
//   -DCL_NATIVE   real hardware, the same barrier, threads pinned with --cpus.
//
// Metric: reductions divided by the span from the first client starting to the last finishing (per 1000
// cycles and per us), plus the median per-client cycles per reduction. --check verifies every round's
// result equals the expected sum, which a barrier that released early would break.
#ifdef CL_NATIVE
#define _GNU_SOURCE
#include <sched.h>
#include <time.h>
#include <x86intrin.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#if !defined(CL_NATIVE) && !defined(CL_SPIN)
#include "ndp_arena.h"
#include "syncron.h"
#endif
#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;

uint32_t g_clients = 2, g_ops = 100;
bool g_check = false;

volatile uint64_t* g_slot = nullptr;    // one cache line per client
volatile uint64_t* g_result = nullptr;  // the published sum, its own line
uint32_t g_stride = kLine / sizeof(uint64_t);

#ifdef CL_SPIN
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }
inline void pause_once() { zsim_park(); }
#elif defined(CL_NATIVE)
std::vector<int> g_cpus;
const bool g_native = true;
inline uint64_t now_ticks() { return __rdtsc(); }
inline void pause_once() { __asm__ __volatile__("pause" ::: "memory"); }

double tsc_ghz() {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t t0 = __rdtsc();
    struct timespec nap = {0, 200000000};
    nanosleep(&nap, nullptr);
    uint64_t t1 = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    return (t1 - t0) / ns;
}

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) perror("sched_setaffinity");
}
#else
const bool g_native = false;
SyncVar g_bar;
inline uint64_t now_ticks() { return zsim_now(); }
#endif

// ------------------------------------------------------------------- the barrier ---
#if defined(CL_SPIN) || defined(CL_NATIVE)
// Sense-reversing centralised barrier: one counter, one sense flag, no engine.
struct SwBarrier {
    volatile uint64_t count = 0;
    volatile uint64_t sense = 0;
    char pad[kLine];
};
SwBarrier g_sw;

void barrier_all(uint64_t* my_sense) {
    *my_sense = !*my_sense;
    if (__sync_add_and_fetch(&g_sw.count, 1) == g_clients) {
        g_sw.count = 0;
        __sync_synchronize();
        g_sw.sense = *my_sense;
    } else {
        while (g_sw.sense != *my_sense) pause_once();
    }
}
#else
void barrier_all(uint64_t*) {
    // Across units when the clients span more than one; the SE picks one- or two-level itself.
    syncron_barrier_wait_across_units(g_bar);
}
#endif

uint32_t core_id() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

struct Task {
    pthread_t th;
    uint32_t idx = 0, core = (uint32_t)-1;
    uint64_t start = 0, end = 0, ops_done = 0, bad = 0;
};

volatile uint64_t g_registered = 0;
uint64_t g_total_threads = 0;

void park_until(volatile uint64_t* counter, uint64_t target) {
    while (*counter < target) {
#ifdef CL_NATIVE
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(CL_SPIN)
        zsim_park();
#else
        volatile uint64_t token = 0;
        req_sync(&token, SYNCRON_OP_PARK, 0);
#endif
    }
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
#ifdef CL_NATIVE
    pin_to(g_cpus.empty() ? (int)t->idx : g_cpus[t->idx]);
#endif
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);

    uint64_t sense = 0;
    t->start = now_ticks();
    for (uint32_t r = 1; r <= g_ops; r++) {
        g_slot[(size_t)t->idx * g_stride] = (uint64_t)t->idx + r;   // this round's contribution
        barrier_all(&sense);                                        // everyone has contributed
        if (t->idx == 0) {
            uint64_t sum = 0;
            for (uint32_t c = 0; c < g_clients; c++) sum += g_slot[(size_t)c * g_stride];
            *g_result = sum;
        }
        barrier_all(&sense);                                        // the result is published
        if (g_check) {
            uint64_t want = (uint64_t)g_clients * r + (uint64_t)g_clients * (g_clients - 1) / 2;
            if (*g_result != want) t->bad++;
        }
        t->ops_done++;
    }
    t->end = now_ticks();
    return nullptr;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t units = 4, cores_per_unit = 16, stack_mb = 1024;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) exit(1); return argv[++i]; };
        if (!strcmp(k, "--clients")) g_clients = atoi(val());
        else if (!strcmp(k, "--ops")) g_ops = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
#ifdef CL_NATIVE
        else if (!strcmp(k, "--cpus")) {
            for (char* tok = strtok((char*)val(), ","); tok; tok = strtok(nullptr, ",")) g_cpus.push_back(atoi(tok));
        }
#endif
        else {
            fprintf(stderr, "usage: collective --clients N --ops K [--units 4] [--cores-per-unit 16] "
                            "[--stack-mb 1024] [--check]\n");
            return 1;
        }
    }
    if (g_clients < 2 || !g_ops) { fprintf(stderr, "collective: need clients >= 2 and ops >= 1\n"); return 1; }

    double ghz = 2.0;
    size_t bytes = ((size_t)g_clients + 1) * kLine;   // one line per client, plus the result's line
#if defined(CL_SPIN) || defined(CL_NATIVE)
#ifdef CL_NATIVE
    ghz = tsc_ghz();
#endif
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes) != 0) return 1;
    g_slot = (volatile uint64_t*)mem;
    memset((void*)g_slot, 0, bytes);
    g_result = g_slot + (size_t)g_clients * g_stride;
#else
    // Slots, the result and the barrier variable live in unit 0's memory, so unit 0's SE is the Master.
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "collective: arena unavailable\n"); return 1; }
    void* buf = arena.alloc(0, bytes);
    if (!buf) { fprintf(stderr, "collective: no arena space\n"); return 1; }
    g_slot = (volatile uint64_t*)buf;
    memset((void*)g_slot, 0, bytes);
    g_result = g_slot + (size_t)g_clients * g_stride;
#endif

#ifdef CL_SPIN
    const char* backend = "sense-reversing software barrier on the simulated processor";
#elif defined(CL_NATIVE)
    const char* backend = "NATIVE (sense-reversing software barrier)";
#else
    const char* backend = "SynCron (SE barrier + serial fold)";
#endif
    printf("collective: %s, %u clients, %u reductions\n", backend, g_clients, g_ops);

    zsim_roi_begin();
#if !defined(CL_NATIVE) && !defined(CL_SPIN)
    if (!syncron_create_syncvar(arena, 0, g_clients, &g_bar)) return 1;
#endif

    std::vector<Task> tasks(g_clients);
    g_total_threads = g_clients;
    for (uint32_t c = 0; c < g_clients; c++) tasks[c].idx = c;
    for (uint32_t c = 1; c < g_clients; c++) {
        if (pthread_create(&tasks[c].th, nullptr, thread_main, &tasks[c]) != 0) { perror("pthread_create"); return 1; }
        park_until(&g_registered, c);
    }
    thread_main(&tasks[0]);
    for (uint32_t c = 1; c < g_clients; c++) pthread_join(tasks[c].th, nullptr);
    zsim_roi_end();

    uint64_t first = (uint64_t)-1, last = 0, bad = 0;
    std::vector<double> per_op;
    bool ok = true;
    for (uint32_t c = 0; c < g_clients; c++) {
        Task& t = tasks[c];
        first = std::min(first, t.start);
        last = std::max(last, t.end);
        bad += t.bad;
        if (t.ops_done) per_op.push_back((double)(t.end - t.start) / t.ops_done);
#ifdef CL_NATIVE
        uint32_t want = g_cpus.empty() ? c : (uint32_t)g_cpus[c];
#else
        uint32_t want = c;
#endif
        if (t.core != want) ok = false;
    }
    double span = (double)(last - first);
    printf("COLLECTIVE clients=%u reductions=%u span_cycles=%.0f ghz=%.3f\n", g_clients, g_ops, span, ghz);
    printf("COLLECTIVE throughput_ops_per_kcycle=%.2f throughput_ops_per_us=%.2f cycles_per_op_med=%.1f\n",
           span > 0 ? g_ops * 1000.0 / span : 0.0, span > 0 ? g_ops * ghz * 1000.0 / span : 0.0,
           median(per_op));
    if (g_check) {
        // A barrier that released before every contribution landed would give a short sum here.
        printf("CHECK rounds=%u wrong_sums=%lu %s\n", g_ops, bad, bad == 0 ? "OK" : "EARLY RELEASE");
        if (bad) ok = false;
    }
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or a sum was wrong");
    printf("COLLECTIVE done\n");
    return ok ? 0 : 2;
}
