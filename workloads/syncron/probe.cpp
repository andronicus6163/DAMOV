// probe: the S1 throughput probe for the SynCron machine model. No synchronization at all -- it exists to measure
// how fast the simulator runs this machine, and to check that 4 NDP units x 16 in-order cores actually execute.
//
//   probe --threads 64 [--mode stream|chase] [--kb 256] [--iters 20] [--units 4] [--stack-mb 1024] [--no-arena]
//
//   stream  each thread walks its own array linearly (read + write): best case for the memory model
//   chase   each thread follows a random permutation of its own array, one dependent load at a time: the access
//           pattern SynCron's workloads actually have, and the one that makes the simulator work hardest
//
// Every thread touches only its own buffer, so there is no sharing and no coherence traffic. By default each
// buffer is allocated from its own NDP unit's slice of the address space (ndp_arena.h), which is what makes the
// accesses local to the unit's stack -- the way a real NDP system has to work, since a unit has no path off its
// stack. `--no-arena` uses plain malloc instead, which shows what happens without placement.
//
// Threads are created one at a time (so core ids follow creation order) and then **block** on a pthread_barrier
// rather than spinning: a spin gate burns simulated instructions while the remaining threads are being created, and
// at 64 threads that startup spin dwarfed the work it was supposed to measure.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ndp_arena.h"
#include "zsim_hooks.h"

namespace {

struct Task {
    pthread_t th;
    uint32_t id = 0;
    uint64_t* buf = nullptr;
    size_t words = 0;
    uint32_t iters = 0;
    bool chase = false;
    uint64_t checksum = 0;
};

volatile uint64_t g_registered = 0;
pthread_barrier_t g_start;

void* work(void* arg) {
    Task* t = (Task*)arg;
    __sync_fetch_and_add(&g_registered, 1);
    pthread_barrier_wait(&g_start);  // blocks; does not spin

    uint64_t sum = 0;
    if (t->chase) {
        // buf[i] holds the index of the next element: a dependent load chain.
        for (uint32_t it = 0; it < t->iters; it++) {
            size_t i = 0;
            for (size_t n = 0; n < t->words; n++) {
                i = t->buf[i];
                sum += i;
            }
        }
    } else {
        for (uint32_t it = 0; it < t->iters; it++) {
            for (size_t i = 0; i < t->words; i++) {
                t->buf[i] = t->buf[i] + it + 1;
                sum += t->buf[i];
            }
        }
    }
    t->checksum = sum;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t threads = 64, iters = 20, kb = 256, units = 4, stack_mb = 1024;
    bool chase = false, arena_alloc = true;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) exit(1); return argv[++i]; };
        if (!strcmp(k, "--threads")) threads = atoi(val());
        else if (!strcmp(k, "--iters")) iters = atoi(val());
        else if (!strcmp(k, "--kb")) kb = atoi(val());
        else if (!strcmp(k, "--mode")) chase = !strcmp(val(), "chase");
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--no-arena")) arena_alloc = false;
        else { fprintf(stderr, "usage: probe --threads N [--mode stream|chase] [--kb 256] [--iters 20]\n"); return 1; }
    }
    size_t words = (size_t)kb * 1024 / sizeof(uint64_t);
    uint32_t cores_per_unit = (threads + units - 1) / units;

    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (arena_alloc && !arena.ok()) { fprintf(stderr, "probe: arena unavailable\n"); return 1; }

    std::vector<Task> tasks(threads);
    for (uint32_t i = 0; i < threads; i++) {
        tasks[i].id = i;
        tasks[i].words = words;
        tasks[i].iters = iters;
        tasks[i].chase = chase;
        if (arena_alloc) {
            tasks[i].buf = (uint64_t*)arena.alloc(i / cores_per_unit, words * sizeof(uint64_t));
            if (!tasks[i].buf) { fprintf(stderr, "probe: unit %u out of arena space\n", i / cores_per_unit); return 1; }
        } else {
            tasks[i].buf = (uint64_t*)aligned_alloc(4096, words * sizeof(uint64_t));
            if (!tasks[i].buf) { perror("aligned_alloc"); return 1; }
        }
        if (chase) {
            // A single random cycle over the whole buffer (Sattolo), so the chain never short-circuits.
            for (size_t j = 0; j < words; j++) tasks[i].buf[j] = j;
            for (size_t j = words - 1; j > 0; j--) {
                size_t k = (size_t)(rand() % (int)j);
                uint64_t tmp = tasks[i].buf[j];
                tasks[i].buf[j] = tasks[i].buf[k];
                tasks[i].buf[k] = tmp;
            }
        } else {
            memset(tasks[i].buf, 0, words * sizeof(uint64_t));
        }
    }
    printf("probe: %u threads, %s, %u KB/thread, %u iters, %u units, %s\n", threads, chase ? "chase" : "stream", kb,
           iters, units, arena_alloc ? "per-unit arena" : "malloc (no placement)");
    if (arena_alloc) arena.report();

    pthread_barrier_init(&g_start, nullptr, threads + 1);
    zsim_roi_begin();
    for (uint32_t i = 0; i < threads; i++) {
        if (pthread_create(&tasks[i].th, nullptr, work, &tasks[i]) != 0) { perror("pthread_create"); return 1; }
        while (g_registered <= i) __asm__ __volatile__("pause");  // one thread at a time, so core ids are in order
    }
    pthread_barrier_wait(&g_start);  // releases every worker at once
    for (uint32_t i = 0; i < threads; i++) pthread_join(tasks[i].th, nullptr);
    zsim_roi_end();

    uint64_t checksum = 0;
    for (uint32_t i = 0; i < threads; i++) checksum += tasks[i].checksum;
    printf("PROBE done threads=%u mode=%s checksum=%lu\n", threads, chase ? "chase" : "stream",
           (unsigned long)checksum);
    return 0;
}
