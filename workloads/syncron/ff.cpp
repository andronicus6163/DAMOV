// ff: fetch-and-function throughput -- an arbitrary read-modify-write, not just an add.
//
//   ff --clients N --ops K [--cells G] [--units 4 --cores-per-unit 16 --stack-mb 1024] [--check]
//   ff --clients N --ops K [--cells G] --cpus 0,1,2,...        (native build)
//
// Derived from fetchadd.cpp and deliberately identical to it except for the operation inside the critical
// section, so the two measurements differ only in what Phi is. Phi here is an atomic MAX, matching the
// MaxCell instantiation in UpDown's libraries/sync-primitives SyncFF test.
//
// WHY THIS IS THE INTERESTING ONE. SynCron's Synchronization Engine executes a fixed opcode set over fixed
// variable types (lock, barrier, semaphore, condition variable) -- there is no opcode that runs an
// application-supplied operation at the data. But the NDP cores are general in-order cores, so an
// arbitrary Phi IS expressible: take the lock, run it on the core, release. It is simply not accelerated.
// This workload measures that: the cost of emulating fetch-and-function out of a lock.
//
// On UpDown the same operation is inlined into the library's event and is atomic by run-to-completion --
// 11 to 13 instructions with no lock instruction executed at all.
//
// TWO BACKENDS, one source:
//   default      SynCron: one SE lock per cell, the cell itself in the Master unit's memory.
//   -DFF_NATIVE  real hardware: a pthread spin lock around the same Phi, threads pinned with --cpus.
//   -DFF_SPIN    the same spin lock, but under zsim on the simulated 64-core processor.
//
// Metric: completed operations divided by the span from the first client starting to the last finishing
// (ops per 1000 cycles and ops/us), plus mean cycles per operation. --check verifies every cell ended at
// the maximum of everything applied to it, which a lost update would break.
#ifdef FF_NATIVE
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

#if !defined(FF_NATIVE) && !defined(FF_SPIN)
#include "ndp_arena.h"
#include "syncron.h"
#endif
#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;

uint32_t g_clients = 2, g_ops = 100, g_cells = 1;
// Phi: the application-supplied operation. Atomic max, the same body as UpDown MaxCell FF_COMBINE.
inline uint64_t phi(uint64_t cur, uint64_t arg) { return arg > cur ? arg : cur; }
volatile uint64_t* g_ctr = nullptr;  // g_cells cells, one cache line apart
uint32_t g_stride = kLine / sizeof(uint64_t);
bool g_check = false;

#ifdef FF_SPIN
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }
// Even on the simulated processor an arbitrary Phi is not a single instruction: it needs a lock.
volatile uint64_t* g_spin = nullptr;
inline void spin_lock(uint32_t c) { while (__sync_lock_test_and_set(&g_spin[(size_t)c * g_stride], 1)) zsim_park(); }
inline void spin_unlock(uint32_t c) { __sync_lock_release(&g_spin[(size_t)c * g_stride]); }
inline uint64_t apply_phi(uint32_t c, uint64_t arg) {
    spin_lock(c);
    uint64_t v = g_ctr[(size_t)c * g_stride];
    g_ctr[(size_t)c * g_stride] = phi(v, arg);
    spin_unlock(c);
    return v;
}

#elif defined(FF_NATIVE)
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

// No single instruction computes an arbitrary Phi: it needs a lock here too.
volatile uint64_t* g_spin = nullptr;
inline void spin_lock(uint32_t c) { while (__sync_lock_test_and_set(&g_spin[(size_t)c * g_stride], 1)) __asm__ __volatile__("pause" ::: "memory"); }
inline void spin_unlock(uint32_t c) { __sync_lock_release(&g_spin[(size_t)c * g_stride]); }
inline uint64_t apply_phi(uint32_t c, uint64_t arg) {
    spin_lock(c);
    uint64_t v = g_ctr[(size_t)c * g_stride];
    g_ctr[(size_t)c * g_stride] = phi(v, arg);
    spin_unlock(c);
    return v;
}
#else
const bool g_native = false;
std::vector<SyncVar> g_lk;

inline uint64_t now_ticks() { return zsim_now(); }

// The SE has no opcode for an application operation, so Phi runs on the NDP core inside the SE's lock.
inline uint64_t apply_phi(uint32_t c, uint64_t arg) {
    syncron_lock_acquire(g_lk[c]);
    uint64_t v = g_ctr[(size_t)c * g_stride];
    g_ctr[(size_t)c * g_stride] = phi(v, arg);
    syncron_lock_release(g_lk[c]);
    return v;
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
    uint64_t start = 0, end = 0, ops_done = 0;
};

volatile uint64_t g_registered = 0;
uint64_t g_total_threads = 0;

void park_until(volatile uint64_t* counter, uint64_t target) {
    while (*counter < target) {
#ifdef FF_NATIVE
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(FF_SPIN)
        zsim_park();
#else
        volatile uint64_t token = 0;
        req_sync(&token, SYNCRON_OP_PARK, 0);
#endif
    }
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
#ifdef FF_NATIVE
    pin_to(g_cpus.empty() ? (int)t->idx : g_cpus[t->idx]);
#endif
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);

    uint32_t c = t->idx % g_cells;
    t->start = now_ticks();
    for (uint32_t i = 0; i < g_ops; i++) {
        apply_phi(c, (uint64_t)t->idx * g_ops + i);   // distinct per client and iteration
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
        else if (!strcmp(k, "--cells")) g_cells = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
#ifdef FF_NATIVE
        else if (!strcmp(k, "--cpus")) {
            for (char* tok = strtok((char*)val(), ","); tok; tok = strtok(nullptr, ",")) g_cpus.push_back(atoi(tok));
        }
#endif
        else {
            fprintf(stderr, "usage: fetchadd --clients N --ops K [--cells 1] [--units 4] [--cores-per-unit 16] "
                            "[--stack-mb 1024] [--check]\n");
            return 1;
        }
    }
    if (!g_clients || !g_ops || !g_cells) { fprintf(stderr, "ff: bad arguments\n"); return 1; }

    double ghz = 2.0;
    size_t bytes = (size_t)g_cells * kLine;
#ifdef FF_SPIN
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes * 2) != 0) return 1;   // cells, then their spin words
    g_ctr = (volatile uint64_t*)mem;
    memset((void*)g_ctr, 0, bytes * 2);
    g_spin = g_ctr + (bytes / sizeof(uint64_t));
#elif defined(FF_NATIVE)
    ghz = tsc_ghz();
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes * 2) != 0) return 1;   // cells, then their spin words
    g_ctr = (volatile uint64_t*)mem;
    memset((void*)g_ctr, 0, bytes * 2);
    g_spin = g_ctr + (bytes / sizeof(uint64_t));
#else
    // Counters and their locks live in unit 0's memory, so unit 0's SE is their Master.
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "ff: arena unavailable\n"); return 1; }
    void* buf = arena.alloc(0, bytes);
    if (!buf) { fprintf(stderr, "ff: no arena space\n"); return 1; }
    g_ctr = (volatile uint64_t*)buf;
    memset((void*)g_ctr, 0, bytes);
#endif

#ifdef FF_SPIN
    const char* backend = "spin lock + Phi on the simulated processor";
#elif defined(FF_NATIVE)
    const char* backend = "NATIVE (spin lock + Phi)";
#else
    const char* backend = "SynCron (SE lock + Phi on the NDP core)";
#endif
    printf("ff: %s, %u clients, %u ops each, %u counter(s)\n", backend, g_clients, g_ops, g_cells);

    zsim_roi_begin();
#if !defined(FF_NATIVE) && !defined(FF_SPIN)
    g_lk.resize(g_cells);
    for (uint32_t c = 0; c < g_cells; c++) {
        if (!syncron_create_syncvar(arena, 0, g_clients, &g_lk[c])) return 1;
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

    uint64_t first = (uint64_t)-1, last = 0, ops = 0;
    std::vector<double> per_op;
    bool ok = true;
    for (uint32_t c = 0; c < g_clients; c++) {
        Task& t = tasks[c];
        first = std::min(first, t.start);
        last = std::max(last, t.end);
        ops += t.ops_done;
        if (t.ops_done) per_op.push_back((double)(t.end - t.start) / t.ops_done);
#ifdef FF_NATIVE
        uint32_t want = g_cpus.empty() ? c : (uint32_t)g_cpus[c];
#else
        uint32_t want = c;
#endif
        if (t.core != want) ok = false;
    }
    double span = (double)(last - first);
    printf("FF clients=%u ops=%u cells=%u total_ops=%lu span_cycles=%.0f ghz=%.3f\n", g_clients, g_ops,
           g_cells, ops, span, ghz);
    printf("FF throughput_ops_per_kcycle=%.2f throughput_ops_per_us=%.2f cycles_per_op_med=%.1f\n",
           span > 0 ? ops * 1000.0 / span : 0.0, span > 0 ? ops * ghz * 1000.0 / span : 0.0, median(per_op));
    if (g_check) {
        // Phi is max, so each cell must hold the largest value any of its clients applied. A lost update
        // leaves it below that. Client i applies i*ops .. i*ops+ops-1, and client i uses cell i % cells.
        bool cells_ok = true;
        for (uint32_t c = 0; c < g_cells; c++) {
            uint64_t want = 0;
            bool any = false;
            for (uint32_t i = c; i < g_clients; i += g_cells) {
                uint64_t hi = (uint64_t)i * g_ops + g_ops - 1;
                if (!any || hi > want) { want = hi; any = true; }
            }
            if (!any) continue;
            uint64_t got = g_ctr[(size_t)c * g_stride];
            if (got != want) {
                printf("CHECK cell=%u got=%lu expected=%lu LOST UPDATES\n", c, got, want);
                cells_ok = false;
            }
        }
        printf("CHECK cells=%u %s\n", g_cells, cells_ok ? "OK" : "LOST UPDATES");
        if (!cells_ok) ok = false;
    }
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or updates were lost");
    printf("FF done\n");
    return ok ? 0 : 2;
}
