// Validation of zsim's "atomics through SynCron locks" (sys.syncron.routeAtomics) on the PIM-baseline machine.
//   atomic_ubench <home_unit> <threads> <iters> [shared]
// Each of <threads> pthreads does <iters> __atomic_fetch_add on a counter. Without "shared" every thread has its own
// counter, placed so its line's home unit (HBMStack channel_interleave: (addr / 32 KiB) % 4) is <home_unit>;
// with "shared" all threads use one counter in <home_unit>. Only the loop is inside the zsim ROI. Plain pthreads
// (no OpenMP) so the app's atomic count is exactly threads x iters; pthread_create/join's own atomics are libc's.
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static inline void magic(unsigned long op) {
  __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op) : "memory");
}

static const uint64_t UNIT_BYTES = 32768, UNITS = 4;
static uint64_t* counters[256];
static long iters;
static pthread_barrier_t start;

static void* work(void* a) {
  uint64_t* c = counters[(long)a];
  pthread_barrier_wait(&start);
  for (long i = 0; i < iters; i++) __atomic_fetch_add(c, 1, __ATOMIC_SEQ_CST);
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: atomic_ubench <home_unit> <threads> <iters> [shared]\n"); return 1; }
  int unit = atoi(argv[1]), T = atoi(argv[2]);
  iters = atol(argv[3]);
  bool shared = argc > 4 && !strcmp(argv[4], "shared");
  // One 128 KiB-aligned block per thread: offset unit * 32 KiB has home <unit>.
  char* mem = (char*)aligned_alloc(UNIT_BYTES * UNITS, UNIT_BYTES * UNITS * (T + 1));
  memset(mem, 0, UNIT_BYTES * UNITS * (T + 1));
  for (int t = 0; t < T; t++)
    counters[t] = (uint64_t*)(mem + UNIT_BYTES * UNITS * (shared ? 0 : t) + UNIT_BYTES * unit);
  pthread_barrier_init(&start, nullptr, T);
  pthread_t th[256];
  magic(1025);  // ROI begin (zsim)
  for (long t = 1; t < T; t++) pthread_create(&th[t], nullptr, work, (void*)t);
  work((void*)0);
  for (int t = 1; t < T; t++) pthread_join(th[t], nullptr);
  magic(1026);  // ROI end
  uint64_t sum = 0;
  for (int t = 0; t < (shared ? 1 : T); t++) sum += *counters[t];
  printf("ATOMIC_UBENCH unit=%d threads=%d iters=%ld shared=%d sum=%lu expect=%ld %s\n", unit, T, iters, shared,
         (unsigned long)sum, (long)T * iters, sum == (uint64_t)T * iters ? "PASS" : "FAIL");
  return sum == (uint64_t)T * iters ? 0 : 1;
}
