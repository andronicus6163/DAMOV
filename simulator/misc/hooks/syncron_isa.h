/* SynCron (HPCA'21) ISA and API, application side.
 *
 * KEEP THE OPCODES IN SYNC with simulator/src/syncron/sync_isa.h (zsim already duplicates its magic-op numbers the
 * same way, between zsim.cpp and zsim_hooks.h).
 *
 * The paper's two instructions (Sec 4.1):
 *   req_sync  addr, opcode, info   issue a message, BLOCK until the ACK (acquire semantics)
 *   req_async addr, opcode         issue a message and continue (release semantics)
 * Both act as memory fences, so each is wrapped in a compiler barrier here.
 */
#ifndef __SYNCRON_ISA_H__
#define __SYNCRON_ISA_H__

#include <stdint.h>

#include "zsim_hooks.h"

#define SYNCRON_OP_PING                 0
#define SYNCRON_OP_REGISTER_VAR         1
#define SYNCRON_OP_UNREGISTER_VAR       2
#define SYNCRON_OP_BARRIER_WITHIN_UNIT  3
#define SYNCRON_OP_BARRIER_ACROSS_UNITS 4
#define SYNCRON_OP_BARRIER_POLL         5
#define SYNCRON_OP_PARK                 6

#ifdef __x86_64__
/* Returns the cycle the SE answered at (the core was stalled until then). */
static inline uint64_t req_sync(const volatile void* addr, uint64_t opcode, uint64_t info) {
    uint64_t ret = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;"
                         : "=a"(ret)
                         : "c"((uint64_t)ZSIM_MAGIC_OP_REQ_SYNC), "D"((uint64_t)addr), "S"(opcode), "d"(info),
                           "a"(ret));
    COMPILER_BARRIER();
    return ret;
}

static inline void req_async(const volatile void* addr, uint64_t opcode, uint64_t info) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;"
                         :
                         : "c"((uint64_t)ZSIM_MAGIC_OP_REQ_ASYNC), "D"((uint64_t)addr), "S"(opcode), "d"(info));
    COMPILER_BARRIER();
}
/* Hardware message passing between NDP cores, which is how the paper's software baselines (Central, Hier) reach
 * their server cores: "clients communicate with the server via hardware message-passing" (Sec 6). The tag is opaque
 * to the simulator -- it carries whatever the server protocol needs -- and must fit in 48 bits. */
static inline void syncron_msg_send(uint64_t dst_core, uint64_t tag) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;"
                         :
                         : "c"((uint64_t)ZSIM_MAGIC_OP_MSG_SEND), "D"(dst_core), "S"(tag));
    COMPILER_BARRIER();
}

/* Takes the earliest-arriving message in this core's inbox, blocking the core until it arrives. Returns 0 if the
 * inbox is empty (the core has been parked to the end of the phase), otherwise SYNCRON_MSG_VALID | source core |
 * tag. */
static inline uint64_t syncron_msg_recv(void) {
    uint64_t ret = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : "=a"(ret) : "c"((uint64_t)ZSIM_MAGIC_OP_MSG_RECV), "a"(ret));
    COMPILER_BARRIER();
    return ret;
}
#else
static inline uint64_t req_sync(const volatile void* addr, uint64_t opcode, uint64_t info) { return 0; }
static inline void req_async(const volatile void* addr, uint64_t opcode, uint64_t info) {}
static inline void syncron_msg_send(uint64_t dst_core, uint64_t tag) {}
static inline uint64_t syncron_msg_recv(void) { return 0; }
#endif

#define SYNCRON_MSG_VALID  (1ULL << 63)
#define SYNCRON_MSG_SRC(w) (((w) >> 48) & 0x7FFFULL)
#define SYNCRON_MSG_TAG(w) ((w) & ((1ULL << 48) - 1))

#endif /*__SYNCRON_ISA_H__*/
