#ifndef __ZSIM_HOOKS_H__
#define __ZSIM_HOOKS_H__

#include <stdint.h>
#include <stdio.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

//These need to be in sync with the simulator
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)
#define ZSIM_MAGIC_OP_WORK_BEGIN        (1029) //ubik
#define ZSIM_MAGIC_OP_WORK_END          (1030) //ubik
#define ZSIM_MAGIC_OP_UNCACHED_REGION   (1040)
#define ZSIM_MAGIC_OP_NOW               (1041)
#define ZSIM_MAGIC_OP_REQ_SYNC          (1042)   /* SynCron: see syncron_isa.h */
#define ZSIM_MAGIC_OP_REQ_ASYNC         (1043)
#define ZSIM_MAGIC_OP_MSG_SEND          (1044)   /* SynCron baselines: hardware message passing between NDP cores */
#define ZSIM_MAGIC_OP_MSG_RECV          (1045)

#ifdef __x86_64__
#define HOOKS_STR  "HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
}

static inline void zsim_magic_op_args(uint64_t op, uint64_t a0, uint64_t a1) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "D"(a0), "S"(a1));
    COMPILER_BARRIER();
}

static inline uint64_t zsim_magic_op_ret(uint64_t op) {
    uint64_t ret = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : "=a"(ret) : "c"(op), "a"(ret));
    COMPILER_BARRIER();
    return ret;
}
#else
#define HOOKS_STR  "NOP-HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    //NOP
}
static inline void zsim_magic_op_args(uint64_t op, uint64_t a0, uint64_t a1) {
    //NOP
}
static inline uint64_t zsim_magic_op_ret(uint64_t op) {
    return 0;
}
#endif

static inline void zsim_roi_begin() {
    printf("[" HOOKS_STR "] ROI begin\n");
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_BEGIN);
}

static inline void zsim_roi_end() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_END);
    printf("[" HOOKS_STR  "] ROI end\n");
}

static inline void zsim_heartbeat() {
    zsim_magic_op(ZSIM_MAGIC_OP_HEARTBEAT);
}

/* Mark [addr, addr+bytes) uncached: every access goes to memory, no cache keeps the line and no
   coherence state is tracked for it. One window per run; set it before the ROI. */
static inline void zsim_uncached_region(void* addr, uint64_t bytes) {
    zsim_magic_op_args(ZSIM_MAGIC_OP_UNCACHED_REGION, (uint64_t)addr, bytes);
}

/* This core's cycle count. Outside the simulator (or in fast-forward) it returns 0, so fall back to rdtsc there. */
static inline uint64_t zsim_now() {
    return zsim_magic_op_ret(ZSIM_MAGIC_OP_NOW);
}

static inline void zsim_work_begin() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_BEGIN); }
static inline void zsim_work_end() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_END); }

#endif /*__ZSIM_HOOKS_H__*/
