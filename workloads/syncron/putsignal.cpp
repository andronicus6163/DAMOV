// putsignal: put-with-signal throughput -- move a payload, then tell the target it is there.
//
//   putsignal --clients N --ops K [--words W] [--units 4 --cores-per-unit 16 --stack-mb 1024] [--check]
//   putsignal --clients N --ops K [--words W] --cpus 0,1,2,...        (native build)
//
// N clients form N/2 producer-consumer pairs. A producer writes a W-word payload into its pair's slot and
// then signals; the consumer wakes, reads the payload and checks it. One handoff is counted per payload.
//
// WHAT THIS IS MEASURING. SynCron has no data-movement primitive -- it coordinates cores, it does not move
// payloads -- so put-with-signal has to be built: store the payload, make sure it is visible, then signal a
// condition variable the consumer is waiting on. That is three steps and at least one SE round trip.
//
// On UpDown the same thing needs no library at all between lanes, because a send_event carries data AND
// wakes the target. For a payload in DRAM, SyncPut points the write's acknowledgement at the consumer, so
// the memory system delivers the completion there and the acknowledgement IS the signal: one message, and
// ps_put_local is 2 instructions.
//
// THREE BACKENDS, one source:
//   default       SynCron: one SE lock + one SE condition variable per pair; payload in the Master unit's
//                 memory. The consumer blocks in the SE rather than spinning.
//   -DPS_SPIN     the simulated processor: payload in shared memory, a sequence flag the consumer spins on.
//                 No engine and no messages, just coherence.
//   -DPS_NATIVE   real hardware, the same flag-and-spin, threads pinned with --cpus.
//
// The spin backends are the FAVOURABLE baseline: a spinning consumer sees the flag as soon as the line
// arrives, with no syscall and no scheduler involvement. A blocking implementation would be slower.
//
// Metric: handoffs divided by the span from the first client starting to the last finishing (per 1000
// cycles and per us), plus the median per-pair cycles per handoff. --check verifies every payload arrived
// intact and in order, which a lost or early signal would break.
#ifdef PS_NATIVE
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

#if !defined(PS_NATIVE) && !defined(PS_SPIN)
#include "ndp_arena.h"
#include "syncron.h"
#endif
#include "zsim_hooks.h"

namespace {

constexpr int kLine = 64;
constexpr uint32_t kMaxWords = 8;

uint32_t g_clients = 2, g_ops = 100, g_words = 1;
uint32_t g_pairs = 1;
bool g_check = false;

// Per pair: [0] published seq, [1] consumed seq, [2..] payload.
//
// The consumed counter is flow control and is NOT optional. A single slot with only a published flag lets
// the producer run ahead and overwrite the payload before the consumer has read it -- measured at 2,946
// corrupt payloads out of 4,000 before this was added. A handoff means the target saw THIS payload, so the
// pair is a capacity-1 bounded buffer: the producer waits for space, the consumer waits for data.
volatile uint64_t* g_slot = nullptr;
uint32_t g_slot_stride = (kMaxWords + 2 + 7) & ~7u;   // words per pair, cache-line rounded

inline volatile uint64_t* slot_of(uint32_t p) { return g_slot + (size_t)p * g_slot_stride; }

#ifdef PS_SPIN
const bool g_native = false;
inline uint64_t now_ticks() { return zsim_now(); }
inline void pause_once() { zsim_park(); }

#elif defined(PS_NATIVE)
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
std::vector<SyncVar> g_lk, g_cv_data, g_cv_space;   // one lock, two conditions: a capacity-1 buffer
inline uint64_t now_ticks() { return zsim_now(); }
#endif

uint32_t core_id() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

// ---------------------------------------------------------------- the operation ---
// put: write the payload, then make it visible and signal. get: wait for the signal, read and verify.

void put_signal(uint32_t p, uint64_t seq) {
    volatile uint64_t* s = slot_of(p);
#if !defined(PS_NATIVE) && !defined(PS_SPIN)
    syncron_lock_acquire(g_lk[p]);
    while (s[1] + 1 < seq) syncron_cond_wait(g_cv_space[p], g_lk[p]);   // wait for the slot to be free
    for (uint32_t w = 0; w < g_words; w++) s[2 + w] = seq * 1000 + w;
    s[0] = seq;
    syncron_cond_signal(g_cv_data[p], g_lk[p]);
    syncron_lock_release(g_lk[p]);
#else
    while (s[1] + 1 < seq) pause_once();                                // wait for the slot to be free
    for (uint32_t w = 0; w < g_words; w++) s[2 + w] = seq * 1000 + w;
    __sync_synchronize();        // the payload must be visible before the flag that advertises it
    s[0] = seq;
#endif
}

bool get_signal(uint32_t p, uint64_t seq) {
    volatile uint64_t* s = slot_of(p);
#if !defined(PS_NATIVE) && !defined(PS_SPIN)
    syncron_lock_acquire(g_lk[p]);
    while (s[0] < seq) syncron_cond_wait(g_cv_data[p], g_lk[p]);
    bool ok = true;
    for (uint32_t w = 0; w < g_words; w++) if (s[2 + w] != seq * 1000 + w) ok = false;
    s[1] = seq;                                                         // the slot is free again
    syncron_cond_signal(g_cv_space[p], g_lk[p]);
    syncron_lock_release(g_lk[p]);
    return ok;
#else
    while (s[0] < seq) pause_once();
    __sync_synchronize();
    bool ok = true;
    for (uint32_t w = 0; w < g_words; w++) if (s[2 + w] != seq * 1000 + w) ok = false;
    __sync_synchronize();
    s[1] = seq;                                                         // the slot is free again
    return ok;
#endif
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
#ifdef PS_NATIVE
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(PS_SPIN)
        zsim_park();
#else
        volatile uint64_t token = 0;
        req_sync(&token, SYNCRON_OP_PARK, 0);
#endif
    }
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
#ifdef PS_NATIVE
    pin_to(g_cpus.empty() ? (int)t->idx : g_cpus[t->idx]);
#endif
    t->core = core_id();
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);

    uint32_t pair = t->idx / 2;
    bool producer = (t->idx % 2) == 0;
    t->start = now_ticks();
    for (uint32_t i = 1; i <= g_ops; i++) {
        if (producer) put_signal(pair, i);
        else if (!get_signal(pair, i)) t->bad++;
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
        else if (!strcmp(k, "--words")) g_words = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
#ifdef PS_NATIVE
        else if (!strcmp(k, "--cpus")) {
            for (char* tok = strtok((char*)val(), ","); tok; tok = strtok(nullptr, ",")) g_cpus.push_back(atoi(tok));
        }
#endif
        else {
            fprintf(stderr, "usage: putsignal --clients N --ops K [--words 1] [--units 4] "
                            "[--cores-per-unit 16] [--stack-mb 1024] [--check]\n");
            return 1;
        }
    }
    if (!g_clients || g_clients % 2 || !g_ops || !g_words || g_words > kMaxWords) {
        fprintf(stderr, "putsignal: need an even client count >= 2, ops >= 1, 1 <= words <= %u\n", kMaxWords);
        return 1;
    }
    g_pairs = g_clients / 2;

    double ghz = 2.0;
    size_t bytes = (size_t)g_pairs * g_slot_stride * sizeof(uint64_t);
#if defined(PS_SPIN) || defined(PS_NATIVE)
#ifdef PS_NATIVE
    ghz = tsc_ghz();
#endif
    void* mem = nullptr;
    if (posix_memalign(&mem, kLine, bytes) != 0) return 1;
    g_slot = (volatile uint64_t*)mem;
    memset((void*)g_slot, 0, bytes);
#else
    // Slots and their sync variables live in unit 0's memory, so unit 0's SE is their Master.
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "putsignal: arena unavailable\n"); return 1; }
    void* buf = arena.alloc(0, bytes);
    if (!buf) { fprintf(stderr, "putsignal: no arena space\n"); return 1; }
    g_slot = (volatile uint64_t*)buf;
    memset((void*)g_slot, 0, bytes);
#endif

#ifdef PS_SPIN
    const char* backend = "flag + spin on the simulated processor";
#elif defined(PS_NATIVE)
    const char* backend = "NATIVE (flag + spin)";
#else
    const char* backend = "SynCron (SE lock + condition variable)";
#endif
    printf("putsignal: %s, %u clients = %u pair(s), %u handoffs each, %u word payload\n",
           backend, g_clients, g_pairs, g_ops, g_words);

    zsim_roi_begin();
#if !defined(PS_NATIVE) && !defined(PS_SPIN)
    g_lk.resize(g_pairs);
    g_cv_data.resize(g_pairs);
    g_cv_space.resize(g_pairs);
    for (uint32_t p = 0; p < g_pairs; p++) {
        if (!syncron_create_syncvar(arena, 0, 2, &g_lk[p])) return 1;
        if (!syncron_create_syncvar(arena, 0, 2, &g_cv_data[p])) return 1;
        if (!syncron_create_syncvar(arena, 0, 2, &g_cv_space[p])) return 1;
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

    uint64_t first = (uint64_t)-1, last = 0, handoffs = 0, bad = 0;
    std::vector<double> per_op;
    bool ok = true;
    for (uint32_t c = 0; c < g_clients; c++) {
        Task& t = tasks[c];
        first = std::min(first, t.start);
        last = std::max(last, t.end);
        bad += t.bad;
        if (c % 2) {                       // consumers count the completed handoffs
            handoffs += t.ops_done;
            if (t.ops_done) per_op.push_back((double)(t.end - t.start) / t.ops_done);
        }
#ifdef PS_NATIVE
        uint32_t want = g_cpus.empty() ? c : (uint32_t)g_cpus[c];
#else
        uint32_t want = c;
#endif
        if (t.core != want) ok = false;
    }
    double span = (double)(last - first);
    printf("PUTSIGNAL clients=%u pairs=%u ops=%u words=%u handoffs=%lu span_cycles=%.0f ghz=%.3f\n",
           g_clients, g_pairs, g_ops, g_words, handoffs, span, ghz);
    printf("PUTSIGNAL throughput_ops_per_kcycle=%.2f throughput_ops_per_us=%.2f cycles_per_op_med=%.1f\n",
           span > 0 ? handoffs * 1000.0 / span : 0.0,
           span > 0 ? handoffs * ghz * 1000.0 / span : 0.0, median(per_op));
    if (g_check) {
        // A signal that arrived before its payload, or a payload that was overwritten, shows up here.
        printf("CHECK payloads=%lu corrupt=%lu %s\n", handoffs, bad, bad == 0 ? "OK" : "CORRUPT PAYLOADS");
        if (bad) ok = false;
    }
    printf("VERIFY %s\n", ok ? "PASS" : "FAIL core layout, or a payload was wrong");
    printf("PUTSIGNAL done\n");
    return ok ? 0 : 2;
}
