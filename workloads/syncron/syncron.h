// SynCron's programming interface (HPCA'21 Sec 4.1), as far as the barrier-first plan needs it.
//
// The paper exposes eleven primitives over the two instructions in syncron_isa.h. Implemented here:
//   syncron_create_syncvar / destroy_syncvar   allocate a synchronization variable and register it with the SE
//   syncron_barrier_wait_within_unit           all participating cores of ONE NDP unit
//   syncron_barrier_wait_across_units          cores spanning units; the variable's Master SE coordinates
// The lock, semaphore and condition-variable primitives are deliberately absent rather than stubbed: the simulator
// panics on their opcodes, so calling one would fail loudly instead of quietly measuring nothing.
//
// A variable must live in the memory of the NDP unit that is to be its Master SE, so it is allocated from that
// unit's slice of the arena (ndp_arena.h) -- the same unit-major placement rule the memory model uses.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "ndp_arena.h"
#include "syncron_isa.h"

// Which NDP unit the calling thread is on. The simulator derives a core's unit from its core id
// (core_id / cores_per_unit), and zsim hands out core ids in thread-creation order with the main thread on core 0 --
// so a thread cannot assume it is on core `thread_index`. Ask the simulator instead: zsim virtualises getcpu() to
// return the core id the thread is running on.
inline uint32_t syncron_core_id() {
    unsigned cpu = 0, node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) != 0) return (uint32_t)-1;
    return cpu;
}

inline uint32_t syncron_unit_of_this_core(uint32_t cores_per_unit) { return syncron_core_id() / cores_per_unit; }

struct SyncVar {
    volatile uint64_t* addr = nullptr;  // the variable itself; the SE keys its Synchronization Table on this address
    uint32_t master_unit = 0;           // the unit whose memory holds it
    uint32_t participants = 0;          // how many cores take part (the SE needs this for barriers)
};

// Allocate a synchronization variable in `unit`'s memory and tell the SEs about it.
inline bool syncron_create_syncvar(NdpArena& arena, uint32_t unit, uint32_t participants, SyncVar* out) {
    void* p = arena.alloc(unit, 64);  // one line per variable, so two variables never share an ST entry by accident
    if (!p) {
        fprintf(stderr, "syncron: no arena space left in unit %u\n", unit);
        return false;
    }
    out->addr = (volatile uint64_t*)p;
    out->master_unit = unit;
    out->participants = participants;
    *out->addr = 0;
    req_sync(out->addr, SYNCRON_OP_REGISTER_VAR, participants);
    return true;
}

inline void syncron_destroy_syncvar(SyncVar* v) {
    if (v->addr) req_sync(v->addr, SYNCRON_OP_UNREGISTER_VAR, 0);
    v->addr = nullptr;
}

// Barrier among the participating cores of one NDP unit (the local SE counts and releases them), and among cores
// spanning units (local SEs forward to the variable's Master SE, Sec 4.3).
//
// The arrival is one req_sync. An early arriver cannot be told its release cycle yet -- that is decided when the last
// participant arrives -- so the arrival returns 0 and we ask again until the engine answers. Each ask parks the core
// until the next simulator phase boundary and costs the model nothing: the barrier's modelled cost is the engine's
// (message path + SPU occupancy for every departure message), and only the moment the core *notices* is quantised.
inline void syncron_barrier_wait(const SyncVar& v, uint64_t opcode) {
    if (req_sync(v.addr, opcode, 0)) return;                        // this core completed the barrier
    while (!req_sync(v.addr, SYNCRON_OP_BARRIER_POLL, 0)) { }       // wait for the release cycle
}

inline void syncron_barrier_wait_within_unit(const SyncVar& v) {
    syncron_barrier_wait(v, SYNCRON_OP_BARRIER_WITHIN_UNIT);
}

inline void syncron_barrier_wait_across_units(const SyncVar& v) {
    syncron_barrier_wait(v, SYNCRON_OP_BARRIER_ACROSS_UNITS);
}

// Not in the paper: a bare round trip to the local SE, for checking the ISA path and its latency.
inline uint64_t syncron_ping(const volatile void* addr) { return req_sync(addr, SYNCRON_OP_PING, 0); }
