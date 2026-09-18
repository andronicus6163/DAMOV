// ping: the S3 check that SynCron's ISA path works and costs what the model says it should.
//
//   ping --threads N [--reps 200] [--units 4] [--cores-per-unit 16] [--stack-mb 1024]
//
// Each thread issues `reps` req_sync(PING) messages to its own NDP unit's SE and measures, on its own core clock,
// how long each one took. There is no synchronization semantics here: PING just goes core -> local SE -> core, so
// the expected cost is 2 x core<->SE plus one SPU service, and anything beyond that is SPU queueing from the other
// cores of the same unit -- which is the property worth checking, because 16 cores share one SPU.
//
// Each thread asks the simulator which core it is on (getcpu) and uses its own unit's variable. It must not assume
// "thread i is on core i": zsim gives the main thread core 0 and the workers cores 1..N, so deriving the unit from
// the thread index puts one thread's traffic on the wrong SE -- which is exactly what the first run of this test
// did, visibly, in the per-unit message counters.
//
// The main thread takes part as participant 0 rather than idling: zsim hands out core 0 to main, so with N workers
// plus an idle main the machine would need N+1 cores and the last worker would double up on core 1 (which is what
// the first version of this test did -- thread 63 reported core 1). Main participating means 64 participants land on
// exactly the 64 cores of the 4 NDP units.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "ndp_arena.h"
#include "syncron.h"

namespace {

struct Task {
    pthread_t th;
    uint32_t id = 0;
    uint32_t core = (uint32_t)-1;
    uint32_t unit = 0;
    uint32_t reps = 0;
    uint32_t cores_per_unit = 16;
    volatile uint64_t* const* vars = nullptr;  // one variable per unit
    std::vector<uint64_t> lat;
};

volatile uint64_t g_registered = 0;
pthread_barrier_t g_start;

void* work(void* arg) {
    Task* t = (Task*)arg;
    t->core = syncron_core_id();
    t->unit = t->core / t->cores_per_unit;
    __sync_fetch_and_add(&g_registered, 1);
    pthread_barrier_wait(&g_start);
    const volatile uint64_t* var = t->vars[t->unit];
    t->lat.resize(t->reps);
    for (uint32_t i = 0; i < t->reps; i++) {
        uint64_t t0 = zsim_now();
        syncron_ping(var);
        uint64_t t1 = zsim_now();
        t->lat[i] = t1 - t0;
    }
    return nullptr;
}

double median(std::vector<uint64_t> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return (double)v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t threads = 16, reps = 200, units = 4, cores_per_unit = 16, stack_mb = 1024;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) exit(1); return argv[++i]; };
        if (!strcmp(k, "--threads")) threads = atoi(val());
        else if (!strcmp(k, "--reps")) reps = atoi(val());
        else if (!strcmp(k, "--units")) units = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) cores_per_unit = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else {
            fprintf(stderr, "usage: ping --threads N [--reps 200] [--units 4] [--cores-per-unit 16] "
                            "[--stack-mb 1024]\n");
            return 1;
        }
    }
    NdpArena arena(units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "ping: arena unavailable\n"); return 1; }
    std::vector<Task> tasks(threads);
    // One variable per unit, in that unit's own memory.
    std::vector<SyncVar> vars(units);
    std::vector<volatile uint64_t*> var_addrs(units, nullptr);

    printf("ping: %u threads, %u units x %u cores, %u reps\n", threads, units, cores_per_unit, reps);
    pthread_barrier_init(&g_start, nullptr, threads);  // main is participant 0, so it counts as a participant
    zsim_roi_begin();
    for (uint32_t u = 0; u < units; u++) {
        if (!syncron_create_syncvar(arena, u, cores_per_unit, &vars[u])) return 1;
        var_addrs[u] = vars[u].addr;
    }
    for (uint32_t i = 0; i < threads; i++) {
        tasks[i].id = i;
        tasks[i].reps = reps;
        tasks[i].cores_per_unit = cores_per_unit;
        tasks[i].vars = var_addrs.data();
    }
    // Participant 0 is this thread (core 0); workers are created one at a time so core ids follow creation order.
    for (uint32_t i = 1; i < threads; i++) {
        if (pthread_create(&tasks[i].th, nullptr, work, &tasks[i]) != 0) { perror("pthread_create"); return 1; }
        while (g_registered < i) __asm__ __volatile__("pause");
    }
    work(&tasks[0]);
    for (uint32_t i = 1; i < threads; i++) pthread_join(tasks[i].th, nullptr);
    zsim_roi_end();

    std::vector<uint64_t> all;
    for (uint32_t i = 0; i < threads; i++) all.insert(all.end(), tasks[i].lat.begin(), tasks[i].lat.end());
    printf("PING threads=%u reps=%u median_cycles=%.1f min_cycles=%lu max_cycles=%lu\n", threads, reps, median(all),
           (unsigned long)*std::min_element(all.begin(), all.end()),
           (unsigned long)*std::max_element(all.begin(), all.end()));
    for (uint32_t i = 0; i < threads; i++)
        printf("PING thread=%u core=%u unit=%u median_cycles=%.1f\n", i, tasks[i].core, tasks[i].unit,
               median(tasks[i].lat));
    printf("PING done\n");
    return 0;
}
