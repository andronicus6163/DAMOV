// monitor: a bounded-buffer monitor -- one lock and two condition variables -- measured as throughput.
//
//   monitor --clients N --ops K --cap C [--units 4 --cores-per-unit 16 --stack-mb 1024] [--check]
//   monitor --clients N --ops K --cap C --cpus 0,1,2,...        (native build, see below)
//
// The same monitor as UpDown's apps/sync-primitives/monitor (bounded buffer, Mesa semantics), so the three platforms
// run the same program:
//
//   put(v):  lock;  while (count == cap) wait(NOT_FULL);   items[tail++] = v; count++;  signal(NOT_EMPTY);  unlock
//   get():   lock;  while (count == 0)   wait(NOT_EMPTY);  v = items[head++]; count--;  signal(NOT_FULL);   unlock
//
// Clients alternate producer and consumer. Every producer puts distinct values and every consumer sums what it takes,
// so the sums must add up to what was put (--check prints the verdict either way).
//
// TWO BACKENDS, one source, so the baseline is the same program and not merely the same idea:
//   -DMON_SPIN   the CPU baseline, in shared memory: a test-and-set spin lock and condition variables that are
//                generation counters threads spin on -- ordinary cache lines kept coherent by the L3, no engine and
//                no messages. Built for zsim (build.sh OUT=monitor_spin DEFS=-DMON_SPIN) so the baseline can be
//                timed on the simulated processor instead of a loaded host.
//   -DMON_NATIVE real hardware: a pthread mutex and two pthread condition variables, threads pinned with --cpus.
//   default      SynCron: one SE lock and two SE condition variables per monitor (syncron.h). The buffer and the
//                three variables live in the memory of the monitor's Master unit, monitor g in unit g % units.
//
// --monitors G (default 1): G independent monitors. Client idx is a producer when idx is even and a consumer when it
// is odd, and uses monitor (idx / 2) % G, so every monitor gets as many puts as gets. (Using idx % G for the monitor
// made the role and the monitor depend on the same bit and left some monitors with only producers: a deadlock.)
//
// -DMON_PHASES (build.sh OUT=monitor_ph / monitor_spin_ph): also time where each operation goes on the client --
// lock_wait (acquiring), cond_wait (inside wait, including re-acquiring), critical (holding the lock otherwise),
// release (the unlock call) and outside (between operations) -- printed as cycles per operation, summed over all
// clients and divided by all operations. The engine's own service/queue/idle times are in the zsim stats.
//
// Metric: completed operations (puts + gets) divided by the span from the first client starting to the last finishing
// (ops per 1000 cycles and ops/us), plus the median over clients of each client's cycles per operation.
//
// RECONSTRUCTED 2026-11-20 after the 10-01 original was lost: lines 1-18 are the original's, the rest follows
// fetchadd.cpp (same session) and the outputs recorded in dissertation/data/smds (syncron-hbm1-vs-hbm3e.csv,
// monitor-phases.csv). Check new numbers against those before using them.
#ifdef MON_NATIVE
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

#if !defined(MON_NATIVE) && !defined(MON_SPIN)
#include "ndp_arena.h"
#include "syncron.h"
#endif
#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;

uint32_t g_clients = 2, g_ops = 100, g_cap = 16, g_monitors = 1;
bool g_check = false;

// One monitor's shared state: the buffer's indices on their own line, then the items.
struct Buffer {
    volatile uint64_t head, tail, count;
    uint64_t pad[kLine / 8 - 3];
    volatile uint64_t items[1];  // g_cap of them
};
std::vector<Buffer*> g_buf;

inline size_t buffer_bytes() { return ((size_t)kLine + (size_t)g_cap * 8 + kLine - 1) / kLine * kLine; }

#ifdef MON_SPIN
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }

// Lock word and the two generation counters, each on its own line.
struct Sync {
    volatile uint64_t lock;
    uint64_t p0[kLine / 8 - 1];
    volatile uint64_t not_full;
    uint64_t p1[kLine / 8 - 1];
    volatile uint64_t not_empty;
    uint64_t p2[kLine / 8 - 1];
};
std::vector<Sync*> g_sync;
typedef volatile uint64_t* Cond;

inline void mon_lock(uint32_t m) { while (__sync_lock_test_and_set(&g_sync[m]->lock, 1)) zsim_park(); }
inline void mon_unlock(uint32_t m) { __sync_lock_release(&g_sync[m]->lock); }
inline Cond not_full(uint32_t m) { return &g_sync[m]->not_full; }
inline Cond not_empty(uint32_t m) { return &g_sync[m]->not_empty; }
// Mesa wait: note the generation, drop the lock, spin until a signal bumps it, take the lock again.
inline void mon_wait(Cond cv, uint32_t m) {
    uint64_t gen = *cv;
    mon_unlock(m);
    while (*cv == gen) zsim_park();
    mon_lock(m);
}
inline void mon_signal(Cond cv, uint32_t) { __sync_fetch_and_add(cv, 1); }

#elif defined(MON_NATIVE)
std::vector<int> g_cpus;
const bool g_native = true;

inline uint64_t now_ticks() { return __rdtsc(); }

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

struct alignas(kLine) Sync {
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
};
std::vector<Sync*> g_sync;
typedef pthread_cond_t* Cond;

inline void mon_lock(uint32_t m) { pthread_mutex_lock(&g_sync[m]->lock); }
inline void mon_unlock(uint32_t m) { pthread_mutex_unlock(&g_sync[m]->lock); }
inline Cond not_full(uint32_t m) { return &g_sync[m]->not_full; }
inline Cond not_empty(uint32_t m) { return &g_sync[m]->not_empty; }
inline void mon_wait(Cond cv, uint32_t m) { pthread_cond_wait(cv, &g_sync[m]->lock); }
inline void mon_signal(Cond cv, uint32_t) { pthread_cond_signal(cv); }

#else
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }

struct Sync {
    SyncVar lock, not_full, not_empty;
};
std::vector<Sync> g_sync;
typedef const SyncVar* Cond;

inline void mon_lock(uint32_t m) { syncron_lock_acquire(g_sync[m].lock); }
inline void mon_unlock(uint32_t m) { syncron_lock_release(g_sync[m].lock); }
inline Cond not_full(uint32_t m) { return &g_sync[m].not_full; }
inline Cond not_empty(uint32_t m) { return &g_sync[m].not_empty; }
inline void mon_wait(Cond cv, uint32_t m) { syncron_cond_wait(*cv, g_sync[m].lock); }
inline void mon_signal(Cond cv, uint32_t m) { syncron_cond_signal(*cv, g_sync[m].lock); }
#endif

enum Phase { PH_LOCK_WAIT, PH_COND_WAIT, PH_CRITICAL, PH_RELEASE, PH_OUTSIDE, PH_COUNT };
const char* kPhaseName[PH_COUNT] = {"lock_wait", "cond_wait", "critical", "release", "outside"};

uint32_t core_id() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

struct Task {
    pthread_t th;
    uint32_t idx = 0, core = (uint32_t)-1;
    uint64_t start = 0, end = 0, ops_done = 0;
    uint64_t sum = 0;  // producer: what it put; consumer: what it took
    uint64_t phase[PH_COUNT] = {0, 0, 0, 0, 0};
};

volatile uint64_t g_registered = 0;
uint64_t g_total_threads = 0;

void park_until(volatile uint64_t* counter, uint64_t target) {
    while (*counter < target) {
#ifdef MON_NATIVE
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(MON_SPIN)
        zsim_park();
#else
        volatile uint64_t token = 0;
        req_sync(&token, SYNCRON_OP_PARK, 0);
#endif
    }
}

#ifdef MON_PHASES
#define PH_MARK(t, ph, since)            \
    do {                                 \
        uint64_t _n = now_ticks();       \
        (t)->phase[ph] += _n - (since);  \
        (since) = _n;                    \
    } while (0)
#else
#define PH_MARK(t, ph, since) do { } while (0)
#endif

// One operation. `since` is the time the previous phase ended (only used with MON_PHASES).
inline void put(Task* t, uint32_t m, uint64_t v, uint64_t& since) {
    Buffer* b = g_buf[m];
    PH_MARK(t, PH_OUTSIDE, since);
    mon_lock(m);
    PH_MARK(t, PH_LOCK_WAIT, since);
    while (b->count == g_cap) {
        PH_MARK(t, PH_CRITICAL, since);
        mon_wait(not_full(m), m);
        PH_MARK(t, PH_COND_WAIT, since);
    }
    b->items[b->tail] = v;
    b->tail = (b->tail + 1) % g_cap;
    b->count = b->count + 1;
    mon_signal(not_empty(m), m);
    PH_MARK(t, PH_CRITICAL, since);
    mon_unlock(m);
    PH_MARK(t, PH_RELEASE, since);
}

inline uint64_t get(Task* t, uint32_t m, uint64_t& since) {
    Buffer* b = g_buf[m];
    PH_MARK(t, PH_OUTSIDE, since);
    mon_lock(m);
    PH_MARK(t, PH_LOCK_WAIT, since);
    while (b->count == 0) {
        PH_MARK(t, PH_CRITICAL, since);
        mon_wait(not_empty(m), m);
        PH_MARK(t, PH_COND_WAIT, since);
    }
    uint64_t v = b->items[b->head];
    b->head = (b->head + 1) % g_cap;
    b->count = b->count - 1;
    mon_signal(not_full(m), m);
    PH_MARK(t, PH_CRITICAL, since);
    mon_unlock(m);
    PH_MARK(t, PH_RELEASE, since);
    return v;
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
#ifdef MON_NATIVE
    pin_to(g_cpus.empty() ? (int)t->idx : g_cpus[t->idx]);
#endif
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);

    bool producer = (t->idx % 2) == 0;
    uint32_t m = (t->idx / 2) % g_monitors;
    t->start = now_ticks();
    uint64_t since = t->start;
    for (uint32_t i = 0; i < g_ops; i++) {
        if (producer) {
            uint64_t v = (uint64_t)t->idx * g_ops + i + 1;  // distinct across all producers, never 0
            put(t, m, v, since);
            t->sum += v;
        } else {
            t->sum += get(t, m, since);
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
        else if (!strcmp(k, "--cap")) g_cap = atoi(val());
        else if (!strcmp(k, "--monitors")) g_monitors = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
#ifdef MON_NATIVE
        else if (!strcmp(k, "--cpus")) {
            for (char* tok = strtok((char*)val(), ","); tok; tok = strtok(nullptr, ",")) g_cpus.push_back(atoi(tok));
        }
#endif
        else {
            fprintf(stderr, "usage: monitor --clients N --ops K --cap C [--monitors 1] [--units 4] "
                            "[--cores-per-unit 16] [--stack-mb 1024] [--check]\n");
            return 1;
        }
    }
    if (!g_clients || !g_ops || !g_cap || !g_monitors) { fprintf(stderr, "monitor: bad arguments\n"); return 1; }
    if (g_clients % 2) { fprintf(stderr, "monitor: --clients must be even (producer/consumer pairs)\n"); return 1; }
    (void)cores_per_unit;

    double ghz = 2.0;
    g_buf.resize(g_monitors);
#if defined(MON_SPIN) || defined(MON_NATIVE)
#ifdef MON_NATIVE
    ghz = tsc_ghz();
#endif
    g_sync.resize(g_monitors);
    for (uint32_t m = 0; m < g_monitors; m++) {
        void* p = nullptr;
        if (posix_memalign(&p, kLine, buffer_bytes()) != 0) return 1;
        memset(p, 0, buffer_bytes());
        g_buf[m] = (Buffer*)p;
        if (posix_memalign(&p, kLine, sizeof(Sync)) != 0) return 1;
        memset(p, 0, sizeof(Sync));
        g_sync[m] = (Sync*)p;
#ifdef MON_NATIVE
        pthread_mutex_init(&g_sync[m]->lock, nullptr);
        pthread_cond_init(&g_sync[m]->not_full, nullptr);
        pthread_cond_init(&g_sync[m]->not_empty, nullptr);
#endif
    }
#else
    // Monitor m's buffer and variables live in unit m % units, so that unit's SE is their Master.
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "monitor: arena unavailable\n"); return 1; }
    for (uint32_t m = 0; m < g_monitors; m++) {
        void* p = arena.alloc(m % units, buffer_bytes());
        if (!p) { fprintf(stderr, "monitor: no arena space\n"); return 1; }
        memset(p, 0, buffer_bytes());
        g_buf[m] = (Buffer*)p;
    }
#endif

#ifdef MON_SPIN
    const char* backend = "spin lock + generation-counter condvars on the simulated processor";
#elif defined(MON_NATIVE)
    const char* backend = "NATIVE (pthread mutex + condvars)";
#else
    const char* backend = "SynCron (SE lock + SE condition variables)";
#endif
#ifdef MON_PHASES
    const char* inst = ", phase-instrumented";
#else
    const char* inst = "";
#endif
    printf("monitor: %s%s, %u clients, %u ops each, cap %u, %u monitor(s)\n", backend, inst, g_clients, g_ops, g_cap,
           g_monitors);

    zsim_roi_begin();
#if !defined(MON_NATIVE) && !defined(MON_SPIN)
    g_sync.resize(g_monitors);
    for (uint32_t m = 0; m < g_monitors; m++) {
        uint32_t users = 0;
        for (uint32_t c = 0; c < g_clients; c++) users += ((c / 2) % g_monitors) == m;
        if (!users) users = 1;
        if (!syncron_create_syncvar(arena, m % units, users, &g_sync[m].lock)) return 1;
        if (!syncron_create_syncvar(arena, m % units, users, &g_sync[m].not_full)) return 1;
        if (!syncron_create_syncvar(arena, m % units, users, &g_sync[m].not_empty)) return 1;
    }
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

    uint64_t first = (uint64_t)-1, last = 0, ops = 0, put_sum = 0, got_sum = 0;
    uint64_t phase[PH_COUNT] = {0, 0, 0, 0, 0};
    std::vector<double> per_op;
    bool ok = true;
    for (uint32_t c = 0; c < g_clients; c++) {
        Task& t = tasks[c];
        first = std::min(first, t.start);
        last = std::max(last, t.end);
        ops += t.ops_done;
        if (t.ops_done) per_op.push_back((double)(t.end - t.start) / t.ops_done);
        if (c % 2 == 0) put_sum += t.sum; else got_sum += t.sum;
        for (int p = 0; p < PH_COUNT; p++) phase[p] += t.phase[p];
#ifdef MON_NATIVE
        uint32_t want = g_cpus.empty() ? c : (uint32_t)g_cpus[c];
#else
        uint32_t want = c;
#endif
        if (t.core != want) ok = false;
    }
    double span = (double)(last - first);
    printf("MONITOR clients=%u ops=%u cap=%u monitors=%u total_ops=%lu span_cycles=%.0f ghz=%.3f\n", g_clients, g_ops,
           g_cap, g_monitors, ops, span, ghz);
    printf("MONITOR throughput_ops_per_kcycle=%.2f throughput_ops_per_us=%.2f cycles_per_op_med=%.1f\n",
           span > 0 ? ops * 1000.0 / span : 0.0, span > 0 ? ops * ghz * 1000.0 / span : 0.0, median(per_op));
#ifdef MON_PHASES
    for (int p = 0; p < PH_COUNT; p++)
        printf("PHASE %s cycles_per_op=%.1f\n", kPhaseName[p], ops ? (double)phase[p] / ops : 0.0);
#endif
    // Whatever the check flag, the sums are cheap: every value put must have been taken exactly once.
    uint64_t left = 0;
    for (uint32_t m = 0; m < g_monitors; m++) left += g_buf[m]->count;
    bool sums_ok = put_sum == got_sum && left == 0;
    printf("CHECK put_sum=%lu got_sum=%lu left_in_buffers=%lu %s\n", put_sum, got_sum, left,
           sums_ok ? "OK" : "LOST OR DUPLICATED ITEMS");
    if (g_check && !sums_ok) ok = false;
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or items were lost");
    printf("MONITOR done\n");
    return ok ? 0 : 2;
}
