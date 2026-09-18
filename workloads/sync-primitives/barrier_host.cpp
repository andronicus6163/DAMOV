// barrier_host: a barrier across HBM-PIM units, mediated by the host.
//
// HBM-PIM units have no path to each other: a unit can only reach the DRAM channel it sits on, and there is no
// inter-unit network and no coherence. So the only way to synchronise them is through the host, which is what this
// measures:
//
//   PIM unit u  (on channel c(u))   store arrive[c(u)] = seq        (write into its OWN channel, 0 base-die hops)
//                                   poll  release[c(u)] until seq   (read from its OWN channel)
//   host                            poll  arrive[c(u)] for every u  (one read per unit per pass, over the host PHY)
//                                   store release[c(u)] for every u (one write per unit, over the host PHY)
//
// The mailbox lines are registered as an uncached window (zsim_uncached_region), so every flag access really goes to
// DRAM in both directions: no cache holds them and no directory tracks them. That is how an incoherent host<->PIM
// mailbox behaves; without it the flags would live in an L1 and the barrier would cost nothing.
//
// Per barrier each participant records the cycle it arrived and the cycle it was released, and we report
//   lat_last  = release(last) - arrive(last)   cost once everybody has arrived
//   lat_first = release(last) - arrive(first)  cost including arrival skew
//
// Caveat measured on this simulator: only the barrier PERIOD (many barriers, one core's clock) is reliable here. A
// single arrive->release interval is not: zsim's bound phase advances a core's cycle count optimistically and the
// weave phase then corrects it, so a reading taken inside a phase can overshoot. Per-core wait values come out
// larger than the period that contains them, which is the signature of that overshoot. Use period_ns; treat
// wait_* and xcore_* as diagnostics.
// Timestamps come from zsim_now() (the calling core's own cycle count, weave corrections included), falling back to
// rdtsc natively. Do not use rdtsc under zsim: it is virtualised to globPhaseCycles + progress inside the phase,
// a nominal clock that drifts away from a core's real cycle count whenever memory stalls stretch a phase.
//
// Core layout expected from the config: core 0 is the host (full cache hierarchy), cores 1..N are the PIM units
// (sys.cores.pim.pim = true). Threads are created one at a time INSIDE the ROI, so worker k lands on core k; each
// worker checks that with getcpu() and the run fails loudly if the mapping is not what the model assumes.
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <vector>

#include "zsim_hooks.h"

namespace {

struct Sample {
    uint64_t arrive = 0;
    uint64_t release = 0;
};

uint8_t* g_mailbox = nullptr;
uint64_t g_stride = 1024;    // bytes between consecutive channels in the HBMStack address map
uint32_t g_channels = 16;    // channels per stack = PIM units per stack
uint32_t g_rounds = 100, g_warmup = 2, g_total = 102;
// Which channel a unit uses. "cpu" is the model: the HBMStack puts the PIM of core id `cpu` at channel cpu % channels.
// "index" ignores core ids and gives worker k channel k; only for native protocol checks, where there is no mapping.
bool g_map_by_cpu = true;
volatile uint64_t g_registered = 0;
volatile uint64_t g_go = 0;

inline volatile uint64_t* arrive_slot(uint32_t ch) {
    return (volatile uint64_t*)(g_mailbox + (uint64_t)ch * g_stride);
}
// (g_channels + ch) % g_channels == ch, so a unit's release flag sits in the same channel as its arrival flag.
inline volatile uint64_t* release_slot(uint32_t ch) {
    return (volatile uint64_t*)(g_mailbox + (uint64_t)(g_channels + ch) * g_stride);
}

// The calling core's cycle count under zsim; rdtsc natively (and during fast-forward, where zsim_now returns 0).
inline uint64_t now() {
    uint64_t t = zsim_now();
    return t ? t : __rdtsc();
}

uint32_t this_cpu() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

struct Unit {
    uint32_t idx = 0;
    uint32_t cpu = (uint32_t)-1;
    uint32_t channel = 0;
    uint64_t polls = 0;
    std::vector<Sample> samples;
    pthread_t th;
};

std::vector<Unit> g_units;

void* pim_unit(void* arg) {
    Unit* u = (Unit*)arg;
    u->cpu = this_cpu();
    u->channel = g_map_by_cpu ? (u->cpu % g_channels) : (u->idx % g_channels);
    __sync_synchronize();
    __sync_fetch_and_add(&g_registered, 1);
    while (!g_go) __asm__ __volatile__("pause");

    volatile uint64_t* arrive = arrive_slot(u->channel);
    volatile uint64_t* release = release_slot(u->channel);
    uint64_t polls = 0;
    for (uint32_t s = 1; s <= g_total; s++) {
        uint64_t t0 = now();
        *arrive = s;
        while (*release < s) polls++;
        uint64_t t1 = now();
        u->samples[s - 1].arrive = t0;
        u->samples[s - 1].release = t1;
    }
    u->polls = polls;
    return nullptr;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void usage() {
    fprintf(stderr,
            "usage: barrier_host --participants N [--rounds 100] [--warmup 2] [--channels 16]\n"
            "                    [--chan-stride 1024] [--mhz 2000] [--map cpu|index] [--csv out.csv]\n");
    exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t participants = 0;
    double mhz = 2000.0;
    const char* csv = nullptr;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (!strcmp(k, "--participants")) participants = atoi(val());
        else if (!strcmp(k, "--rounds")) g_rounds = atoi(val());
        else if (!strcmp(k, "--warmup")) g_warmup = atoi(val());
        else if (!strcmp(k, "--channels")) g_channels = atoi(val());
        else if (!strcmp(k, "--chan-stride")) g_stride = strtoull(val(), nullptr, 0);
        else if (!strcmp(k, "--mhz")) mhz = atof(val());
        else if (!strcmp(k, "--csv")) csv = val();
        else if (!strcmp(k, "--map")) g_map_by_cpu = !strcmp(val(), "cpu");
        else usage();
    }
    if (!participants) usage();
    if (participants > g_channels) {
        fprintf(stderr, "barrier_host: %u participants need %u channels (one PIM unit per channel)\n", participants,
                participants);
        return 1;
    }
    g_total = g_warmup + g_rounds;

    // Mailbox: 2 slots per channel (arrival + release), aligned so that slot i lands on channel i.
    uint64_t span = 2ull * g_channels * g_stride;
    uint64_t align = (uint64_t)g_channels * g_stride;
    uint8_t* raw = (uint8_t*)mmap(nullptr, span + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); return 1; }
    g_mailbox = raw + ((align - ((uintptr_t)raw % align)) % align);
    memset(g_mailbox, 0, span);

    g_units.resize(participants);
    for (uint32_t i = 0; i < participants; i++) {
        g_units[i].idx = i;
        g_units[i].samples.assign(g_total, Sample{});
    }

    printf("barrier_host: %u participants, %u channels, stride %lu B, %u rounds (+%u warmup), mailbox %p\n",
           participants, g_channels, (unsigned long)g_stride, g_rounds, g_warmup, (void*)g_mailbox);

    zsim_uncached_region(g_mailbox, span);
    zsim_roi_begin();

    // One thread at a time, so the scheduler hands worker k core k (the config makes cores 1..N the PIM units).
    for (uint32_t i = 0; i < participants; i++) {
        if (pthread_create(&g_units[i].th, nullptr, pim_unit, &g_units[i]) != 0) { perror("pthread_create"); return 1; }
        while (g_registered <= i) __asm__ __volatile__("pause");
    }
    uint32_t host_cpu = this_cpu();
    std::vector<uint32_t> ch(participants);
    for (uint32_t i = 0; i < participants; i++) ch[i] = g_units[i].channel;

    __sync_synchronize();
    g_go = 1;

    // Host: poll every unit's arrival flag until they are all in, then write every release flag.
    uint64_t host_polls = 0;
    std::vector<char> done(participants, 0);
    for (uint32_t s = 1; s <= g_total; s++) {
        std::fill(done.begin(), done.end(), 0);
        uint32_t left = participants;
        while (left) {
            for (uint32_t i = 0; i < participants; i++) {
                if (done[i]) continue;
                host_polls++;
                if (*arrive_slot(ch[i]) >= s) { done[i] = 1; left--; }
            }
        }
        for (uint32_t i = 0; i < participants; i++) *release_slot(ch[i]) = s;
    }
    for (uint32_t i = 0; i < participants; i++) pthread_join(g_units[i].th, nullptr);

    zsim_roi_end();

    // Did the threads land where the model assumes?
    bool layout_ok = true;
    for (uint32_t i = 0; i < participants; i++) {
        if (g_map_by_cpu && g_units[i].cpu != i + 1) layout_ok = false;  // core 0 is the host, cores 1..N the PIM units
        for (uint32_t j = 0; j < i; j++)
            if (g_units[i].channel == g_units[j].channel) layout_ok = false;  // one PIM unit per channel
    }
    printf("BARRIER layout host_cpu=%u worker_cpus=", host_cpu);
    for (uint32_t i = 0; i < participants; i++) printf("%s%u:ch%u", i ? "," : "", g_units[i].cpu, g_units[i].channel);
    printf(" expected=1..%u %s\n", participants, layout_ok ? "OK" : "MISMATCH");

    std::vector<double> lat_last, lat_first, skew;
    FILE* f = nullptr;
    if (csv) {
        f = fopen(csv, "w");
        if (!f) { perror(csv); return 1; }
        fprintf(f, "barrier,first_arrive,last_arrive,first_release,last_release\n");
    }
    // Cross-core comparisons are NOT valid here: each core's cycle counter is its own timeline (zsim only keeps cores
    // in step at phase boundaries, and weave corrections move each core's count independently). The xcore_* numbers
    // below are diagnostics only -- ts_inversions counts how often one core's release precedes another's arrival,
    // which measures that misalignment, not a protocol error: the host writes a release flag only after it has read
    // every arrival flag out of DRAM. Use the same-core period/wait numbers for latency.
    uint32_t inversions = 0;
    for (uint32_t s = g_warmup; s < g_total; s++) {
        uint64_t fa = UINT64_MAX, la = 0, fr = UINT64_MAX, lr = 0;
        for (uint32_t i = 0; i < participants; i++) {
            const Sample& sm = g_units[i].samples[s];
            fa = std::min(fa, sm.arrive);
            la = std::max(la, sm.arrive);
            fr = std::min(fr, sm.release);
            lr = std::max(lr, sm.release);
        }
        if (participants > 1 && fr < la) inversions++;
        lat_last.push_back((double)(lr - la));
        lat_first.push_back((double)(lr - fa));
        skew.push_back((double)(la - fa));
        if (f) fprintf(f, "%u,%lu,%lu,%lu,%lu\n", s - g_warmup, (unsigned long)fa, (unsigned long)la,
                       (unsigned long)fr, (unsigned long)lr);
    }
    if (f) fclose(f);

    double to_ns = 1000.0 / mhz;
    // Same-core metrics. zsim only keeps cores in step at phase boundaries, so subtracting one core's rdtsc from
    // another's is unreliable (see ts_inversions). These use one core's clock only:
    //   wait_i     = release_i - arrive_i, that unit's own time inside the barrier
    //   period_i   = arrive_i[last] - arrive_i[first] over the measured barriers, i.e. barrier time end to end
    // min_i wait_i is the unit that arrived last (it waits only for the release path), max_i wait_i the one that
    // arrived first (it waits for everybody plus the release path).
    std::vector<double> waits, periods;
    for (uint32_t i = 0; i < participants; i++) {
        std::vector<double> w;
        for (uint32_t s = g_warmup; s < g_total; s++)
            w.push_back((double)(g_units[i].samples[s].release - g_units[i].samples[s].arrive));
        waits.push_back(median(w));
        if (g_rounds > 1)
            periods.push_back((double)(g_units[i].samples[g_total - 1].arrive - g_units[i].samples[g_warmup].arrive) /
                              (g_rounds - 1));
    }
    for (uint32_t i = 0; i < participants && i < 4; i++)
        printf("BARRIER percore unit=%u cpu=%u ch=%u wait=%.1f period=%.1f first_arrive=%lu last_arrive=%lu\n", i,
               g_units[i].cpu, g_units[i].channel, waits[i], periods.empty() ? 0.0 : periods[i],
               (unsigned long)g_units[i].samples[g_warmup].arrive,
               (unsigned long)g_units[i].samples[g_total - 1].arrive);
    double wait_min = *std::min_element(waits.begin(), waits.end());
    double wait_max = *std::max_element(waits.begin(), waits.end());
    double period_min = periods.empty() ? 0 : *std::min_element(periods.begin(), periods.end());
    double period_max = periods.empty() ? 0 : *std::max_element(periods.begin(), periods.end());

    uint64_t pim_polls = 0;
    for (uint32_t i = 0; i < participants; i++) pim_polls += g_units[i].polls;
    printf("BARRIER kind=hbm-host-mediated participants=%u channels=%u rounds=%u mhz=%.0f\n", participants, g_channels,
           g_rounds, mhz);
    printf("BARRIER xcore_lat_last_cycles=%.1f xcore_lat_last_ns=%.2f xcore_lat_first_cycles=%.1f "
           "xcore_lat_first_ns=%.2f xcore_skew_cycles=%.1f (diagnostics: cross-core clocks are not aligned)\n",
           median(lat_last), median(lat_last) * to_ns, median(lat_first), median(lat_first) * to_ns, median(skew));
    printf("BARRIER xcore_lat_last_min=%.1f xcore_lat_last_max=%.1f ts_inversions=%u of=%u\n",
           *std::min_element(lat_last.begin(), lat_last.end()), *std::max_element(lat_last.begin(), lat_last.end()),
           inversions, g_rounds);
    printf("BARRIER period_cycles=%.1f period_ns=%.2f period_min=%.1f period_max=%.1f\n",
           median(periods), median(periods) * to_ns, period_min, period_max);
    printf("BARRIER wait_last_cycles=%.1f wait_last_ns=%.2f wait_first_cycles=%.1f wait_first_ns=%.2f "
           "wait_med_cycles=%.1f\n",
           wait_min, wait_min * to_ns, wait_max, wait_max * to_ns, median(waits));
    printf("BARRIER host_polls_per_barrier=%.2f pim_polls_per_barrier=%.2f host_writes_per_barrier=%u\n",
           (double)host_polls / g_total, (double)pim_polls / g_total, participants);
    printf("VERIFY %s\n", layout_ok ? "PASS" : "FAIL core layout is not host=0, PIM=1..N one per channel");
    return layout_ok ? 0 : 2;
}
