/* Minimal STREAM-style memory-bound kernel, instrumented with DAMOV/zsim ROI
   hooks. Stands in for the DAMOV workload suite, whose archive is offline. */
#include <stdio.h>
#include <stdlib.h>
#include "../misc/hooks/zsim_hooks.h"

#define N (4 * 1024 * 1024)

int main(void) {
    double *a = malloc(N * sizeof(double));
    double *b = malloc(N * sizeof(double));
    double *c = malloc(N * sizeof(double));
    for (long i = 0; i < N; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }

    zsim_roi_begin();
    for (long i = 0; i < N; i++) c[i] = a[i] + 3.0 * b[i];
    zsim_roi_end();

    printf("checksum %f\n", c[N - 1]);
    return 0;
}
