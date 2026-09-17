/* SynCron (HPCA'21) opcodes and parameters, simulator side.
 *
 * KEEP IN SYNC with misc/hooks/syncron_isa.h, which the application includes. zsim already duplicates its magic-op
 * numbers between zsim.cpp and misc/hooks/zsim_hooks.h; this follows that convention rather than adding an include
 * path from the simulator into the hooks directory.
 *
 * The paper (Sec 4.1) gives the NDP cores two instructions, and builds eleven primitives on top of them:
 *   req_sync  addr, opcode, info   issue a message and BLOCK until the ACK comes back (acquire semantics)
 *   req_async addr, opcode         issue a message and continue (release semantics)
 * Here they are magic ops, and the opcode says which primitive the message belongs to.
 */
#ifndef SYNCRON_SYNC_ISA_H_
#define SYNCRON_SYNC_ISA_H_

#include <stdint.h>

namespace syncron {

enum Opcode : uint32_t {
    OP_PING = 0,                  // not in the paper: a bare round trip to the local SE, to test the ISA path
    OP_REGISTER_VAR = 1,          // create_syncvar(): tell the SE about a variable and its Master unit
    OP_UNREGISTER_VAR = 2,        // destroy_syncvar()
    OP_BARRIER_WITHIN_UNIT = 3,   // barrier_wait_within_unit()  (Sec 4.1)
    OP_BARRIER_ACROSS_UNITS = 4,  // barrier_wait_across_units()
    OP_BARRIER_POLL = 5,          // not in the paper: "has my release cycle been decided?" -- see sync_engine.h
    OP_PARK = 6,                  // not in the paper: park this core until the end of the simulator phase, so a core
                                  // with no role in the run keeps its core id without spending instructions
    // Locks, semaphores and condition variables are S10; the SE rejects their opcodes until then.
    OP_COUNT = 7,
};

inline const char* opcode_name(uint32_t op) {
    switch (op) {
        case OP_PING: return "ping";
        case OP_REGISTER_VAR: return "register_var";
        case OP_UNREGISTER_VAR: return "unregister_var";
        case OP_BARRIER_WITHIN_UNIT: return "barrier_within_unit";
        case OP_BARRIER_ACROSS_UNITS: return "barrier_across_units";
        case OP_BARRIER_POLL: return "barrier_poll";
        case OP_PARK: return "park";
        default: return "?";
    }
}

}  // namespace syncron

#endif  // SYNCRON_SYNC_ISA_H_
