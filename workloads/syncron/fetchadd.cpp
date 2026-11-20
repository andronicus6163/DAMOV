// fetchadd: fetch-and-add throughput -- the primitive SynCron leaves to future work, so on NDP it is a critical
// section: acquire the variable's lock, read-modify-write it in memory, release.
//
//   fetchadd --clients N --ops K [--counters G] [--units 4 --cores-per-unit 16 --stack-mb 1024] [--check]
//   fetchadd --clients N --ops K [--counters G] --cpus 0,1,2,...        (native build)
//
// G counters (default 1 = every client on one variable, the paper's high-contention case). Client c uses counter
// c % G; each counter has its own lock and its own cache line.
//
// TWO BACKENDS, one source:
//   default      SynCron: one SE lock per counter (syncron.h), the counter itself in the Master unit's memory.
//                SynCron has no atomic read-modify-write (Sec 8 leaves it to future work), so the add is a plain
//                load/add/store inside the lock -- which is why this is the lock's throughput, not an atomic's.
//   -DFA_NATIVE  real hardware: __sync_fetch_and_add, i.e. one `lock xadd` per operation, threads pinned with --cpus.
//   -DFA_SPIN    the same `lock xadd`, but under zsim on the simulated 64-core processor: the simulator assigns the
//                cores and times it, so the baseline does not depend on a loaded machine.
//
// Metric: completed adds divided by the span from the first client starting to the last finishing (ops per 1000
// cycles and ops/us), plus mean cycles per operation. --check verifies every counter ended at exactly the number of
// adds it received, which a lost update would break.
#ifdef FA_NATIVE
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

#if !defined(FA_NATIVE) && !defined(FA_SPIN)
#include "ndp_arena.h"
#include "syncron.h"
#endif
#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;

uint32_t g_clients = 2, g_ops = 100, g_counters = 1;
volatile uint64_t* g_ctr = nullptr;  // g_counters counters, one cache line apart
uint32_t g_stride = kLine / sizeof(uint64_t);
bool g_check = false;

#ifdef FA_SPIN
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }
// The hardware primitive, on the simulated processor: one locked read-modify-write.
inline uint64_t fetch_add(uint32_t c) { return __sync_fetch_and_add(&g_ctr[(size_t)c * g_stride], 1); }

#elif defined(FA_NATIVE)
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

// The hardware primitive: one locked read-modify-write.
inline uint64_t fetch_add(uint32_t c) { return __sync_fetch_and_add(&g_ctr[(size_t)c * g_stride], 1); }
#else
const bool g_native = false;
std::vector<SyncVar> g_lk;

inline uint64_t now_ticks() { return zsim_now(); }

// No atomic RMW on the NDP cores: the counter is read, incremented and written back inside the SE's lock.
inline uint64_t fetch_add(uint32_t c) {
    syncron_lock_acquire(g_lk[c]);
    uint64_t v = g_ctr[(size_t)c * g_stride];
    g_ctr[(size_t)c * g_stride] = v + 1;
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
#ifdef FA_NATIVE
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(FA_SPIN)
        zsim_park();
#else
        volatile uint64_t token = 0;
        req_sync(&token, SYNCRON_OP_PARK, 0);
#endif
    }
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
#ifdef FA_NATIVE
    pin_to(g_cpus.empty() ? (int)t->idx : g_cpus[t->idx]);
#endif
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);

    uint32_t c = t->idx % g_counters;
    t->start = now_ticks();
    for (uint32_t i = 0; i < g_ops; i++) {
        fetch_add(c);
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
        else if (!strcmp(k, "--counters")) g_counters = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
#ifdef FA_NATIVE
        else if (!strcmp(k, "--cpus")) {
            for (char* tok = strtok((char*)val(), ","); tok; tok = strtok(nullptr, ",")) g_cpus.push_back(atoi(tok));
        }
#endif
        else {
            fprintf(stderr, "usage: fetchadd --clients N --ops K [--counters 1] [--units 4] [--cores-per-unit 16] "
                            "[--stack-mb 1024] [--check]\n");
            return 1;
        }
    }
    if (!g_clients || !g_ops || !g_counters) { fprintf(stderr, "fetchadd: bad arguments\n"); return 1; }

    double ghz = 2.0;
    size_t bytes = (size_t)g_counters * kLine;
#ifdef FA_SPIN
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes) != 0) return 1;
    g_ctr = (volatile uint64_t*)mem;
    memset((void*)g_ctr, 0, bytes);
#elif defined(FA_NATIVE)
    ghz = tsc_ghz();
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes) != 0) return 1;
    g_ctr = (volatile uint64_t*)mem;
    memset((void*)g_ctr, 0, bytes);
#else
    // Counters and their locks live in unit 0's memory, so unit 0's SE is their Master.
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "fetchadd: arena unavailable\n"); return 1; }
    void* buf = arena.alloc(0, bytes);
    if (!buf) { fprintf(stderr, "fetchadd: no arena space\n"); return 1; }
    g_ctr = (volatile uint64_t*)buf;
    memset((void*)g_ctr, 0, bytes);
#endif

#ifdef FA_SPIN
    const char* backend = "lock xadd on the simulated processor";
#elif defined(FA_NATIVE)
    const char* backend = "NATIVE (lock xadd)";
#else
    const char* backend = "SynCron (SE lock + load/add/store)";
#endif
    printf("fetchadd: %s, %u clients, %u ops each, %u counter(s)\n", backend, g_clients, g_ops, g_counters);

    zsim_roi_begin();
#if !defined(FA_NATIVE) && !defined(FA_SPIN)
    g_lk.resize(g_counters);
    for (uint32_t c = 0; c < g_counters; c++) {
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
#ifdef FA_NATIVE
        uint32_t want = g_cpus.empty() ? c : (uint32_t)g_cpus[c];
#else
        uint32_t want = c;
#endif
        if (t.core != want) ok = false;
    }
    double span = (double)(last - first);
    printf("FETCHADD clients=%u ops=%u counters=%u total_ops=%lu span_cycles=%.0f ghz=%.3f\n", g_clients, g_ops,
           g_counters, ops, span, ghz);
    printf("FETCHADD throughput_ops_per_kcycle=%.2f throughput_ops_per_us=%.2f cycles_per_op_med=%.1f\n",
           span > 0 ? ops * 1000.0 / span : 0.0, span > 0 ? ops * ghz * 1000.0 / span : 0.0, median(per_op));
    if (g_check) {
        // Every add must be in the counter: a lost update shows up here.
        uint64_t total = 0;
        for (uint32_t c = 0; c < g_counters; c++) total += g_ctr[(size_t)c * g_stride];
        printf("CHECK counter_total=%lu expected=%lu %s\n", total, (uint64_t)g_clients * g_ops,
               total == (uint64_t)g_clients * g_ops ? "OK" : "LOST UPDATES");
        if (total != (uint64_t)g_clients * g_ops) ok = false;
    }
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or updates were lost");
    printf("FETCHADD done\n");
    return ok ? 0 : 2;
}
