/* SynCron's Synchronization Engine (HPCA'21 Sec 4.2-4.4), one per NDP unit, plus the hardware message transport the
 * paper's software baselines (Central, Hier) use.
 *
 * MODELLED
 *   - one engine per NDP unit. Each has a Synchronization Processing Unit that serves ONE message at a time, taking
 *     `service_cycles` SPU cycles at `spu_mhz` (Table 5: 12 cycles at 1 GHz), so messages queue -- including the
 *     departure messages a barrier release consists of, which is why a barrier costs more as participants grow.
 *   - a Synchronization Table of `st_entries` entries. An entry holds the variable's address, a local waiting list
 *     (one bit per core in the unit), a global waiting list (one bit per SE), and TableInfo (for a barrier: how many
 *     participants it takes). Entries are allocated on the first arrival and freed when the barrier completes.
 *   - `indexing_counters` counters indexed by the address's least significant bits (Table 5: 256). A counter above
 *     zero means "this variable is currently serviced through main memory" -- Sec 4.4's overflow mode, below.
 *   - core <-> local SE: `local_msg_cycles` each way (the compute die's crossbar).
 *     local SE <-> Master SE: `global_msg_cycles` each way (Table 5's inter-unit link, 40 ns).
 *   - a variable's Master SE is the unit whose memory holds it: (address / bytes_per_unit) % units, the same rule the
 *     memory model uses for unit-major addressing.
 *   - barrier_wait_within_unit: the local SE counts its own cores and releases them (no global traffic).
 *     barrier_wait_across_units: the local SE forwards every arrival to the Master SE, which counts ALL participants
 *     and sends the departures back down. Sec 4.3 says this is deliberately one-level ("local SEs re-direct all
 *     messages received from their local NDP cores to the Master SE"), so this model does not aggregate per unit.
 *
 * OVERFLOW (Sec 4.4, phase S5)
 *   "When an SE receives a message with acquire-type semantics for a synchronization variable and there is no
 *   corresponding entry in the fully-occupied ST, the indexing counter for that synchronization variable increases",
 *   and "a synchronization variable is currently serviced via main memory, when the corresponding indexing counter is
 *   larger than zero". In that mode "the SPU of the Master SE issues read or write requests to its local memory to
 *   globally coordinate synchronization via the syncronVar variable".
 *   Here: the coordinating SE falls back to memory mode, and every message for that variable additionally occupies
 *   its SPU for `overflow_*_accesses` x `overflow_mem_cycles`. The counter is decremented when the barrier completes
 *   (the release-type event for a barrier). Two consequences of the paper's own design are modelled rather than
 *   avoided: a counter is shared by every address with the same LSBs, so an *aliasing* variable is pushed into memory
 *   mode even though the ST has room (counted as `overflowAliased`); and because Sec 4.3's barrier is one-level, only
 *   the coordinating SE ever holds an entry for a barrier variable, so no decrease_indexing_counter messages are sent
 *   -- those appear when locks arrive in S10.
 *   NOT modelled: the syncronVar accesses are charged as SPU occupancy but are not injected into the memory model as
 *   real DRAM traffic. zsim's bound phase cannot return a real DRAM latency to a magic op (Ramulator2::access returns
 *   req.cycle + minLatency and resolves the real latency in the weave phase), and issuing the access from the calling
 *   core would attribute it to the wrong stack for a remote Master SE. `overflow_mem_cycles` is therefore a measured
 *   parameter, not a simulated access; see the progress doc.
 *
 * SCHEMES (Sec 6, phase S6). `scheme` selects which of the paper's four systems this run models:
 *   SYNCRON  the engine above.
 *   IDEAL    "an ideal scheme with zero performance overhead for synchronization": no messages, no SPU, no ST; every
 *            participant is released at the cycle the last one arrives. Its application-visible cost is therefore
 *            purely zsim's phase granularity, which makes it the calibration point for that artefact.
 *   CENTRAL  "one dedicated NDP core in the entire NDP system acts as server", HIER "one NDP core per NDP unit acts
 *            as server", both communicating "via hardware message-passing". The coordination itself is software
 *            running on a real NDP core (so its cost emerges from the simulation instead of being a parameter); this
 *            class only provides the message transport, msgSend/msgRecv, charged the same per-hop latencies as the
 *            SE's own messages. The SE takes no part: barrier opcodes are rejected under these schemes.
 *
 * NOT MODELLED YET
 *   - locks, semaphores, condition variables (S10): their opcodes are rejected.
 *
 * HOW A BLOCKING BARRIER WORKS HERE. req_sync arrives, the SE records it, and the calling core is stalled. The
 * release cycle of an early arriver is not known until the last participant arrives, so the arrival returns 0 and
 * the application polls with OP_BARRIER_POLL; a poll costs the model nothing (no SPU time, not counted as a message)
 * and only asks whether the release cycle has been decided, idling the core to the end of the phase if not. The
 * *modelled* barrier cost is therefore exact (the SE computes each participant's release cycle from the message path
 * and SPU occupancy, and reports it in the stats below), while the *application-visible* cost additionally carries
 * zsim's phase granularity, because a core can only notice the release at a phase boundary. Report both.
 */
#ifndef SYNCRON_SYNC_ENGINE_H_
#define SYNCRON_SYNC_ENGINE_H_

#include "g_std/g_string.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"
#include "galloc.h"
#include "locks.h"
#include "stats.h"
#include "syncron/sync_isa.h"

namespace syncron {

enum Scheme : uint32_t {
    SCHEME_SYNCRON = 0,
    SCHEME_IDEAL = 1,
    SCHEME_CENTRAL = 2,
    SCHEME_HIER = 3,
    SCHEME_COUNT = 4,
};

inline const char* scheme_name(uint32_t s) {
    switch (s) {
        case SCHEME_SYNCRON: return "syncron";
        case SCHEME_IDEAL: return "ideal";
        case SCHEME_CENTRAL: return "central";
        case SCHEME_HIER: return "hier";
        default: return "?";
    }
}

struct Params {
    uint32_t scheme = SCHEME_SYNCRON;
    uint32_t units = 4;
    uint32_t cores_per_unit = 16;
    uint32_t st_entries = 64;          // Table 5
    uint32_t indexing_counters = 256;  // Table 5
    uint32_t spu_mhz = 1000;           // Table 5
    uint32_t service_cycles = 12;      // Table 5: SPU cycles per message
    uint32_t local_msg_cycles = 2;     // core <-> local SE, one way, in core cycles
    uint32_t global_msg_cycles = 80;   // local SE <-> Master SE, one way (40 ns at 2 GHz)
    uint64_t bytes_per_unit = 1ull << 30;
    // Sec 4.4 overflow: one syncronVar access costs this much SPU time. Measured on this machine model, not invented:
    // the S2 pointer-chase run reports pim_latency/pim_requests = 29.4 memory ticks = 58.8 ns = 118 core cycles at
    // 2 GHz. The paper does not say how many accesses a message needs, so the two counts below are this model's
    // assumption: an arrival reads the waiting lists / VarInfo and writes them back, and a release reads them and
    // writes them back cleared.
    uint32_t overflow_mem_cycles = 118;
    uint32_t overflow_arrive_accesses = 2;
    uint32_t overflow_release_accesses = 2;
};

class SyncronSystem : public GlobAlloc {
   public:
    SyncronSystem(const Params& params, uint32_t core_freq_mhz, uint32_t num_cores);

    /* req_sync. Returns the cycle the calling core may resume at, or 0 if it must poll (a barrier that is not
     * complete yet). */
    uint64_t reqSync(uint32_t core_id, uint64_t addr, uint32_t opcode, uint64_t info, uint64_t now);
    /* req_async: issued and forgotten. */
    void reqAsync(uint32_t core_id, uint64_t addr, uint32_t opcode, uint64_t info, uint64_t now);
    /* True if this core has a decided release cycle waiting; consumes it. */
    bool takeRelease(uint32_t core_id, uint64_t* release_cycle);

    /* Hardware message passing between NDP cores (Sec 6's Central and Hier baselines). The tag is opaque to the
     * simulator: it is whatever the software server protocol puts there. */
    void msgSend(uint32_t src_core, uint32_t dst_core, uint64_t tag, uint64_t now);
    /* Takes the earliest-arriving message for this core. Returns false if the inbox is empty; otherwise *word is the
     * packed (valid, source core, tag) the application decodes and *resume is the cycle the core resumes at. */
    bool msgRecv(uint32_t core_id, uint64_t now, uint64_t* word, uint64_t* resume);

    void initStats(AggregateStat* parent);

    uint32_t unitOfCore(uint32_t core_id) const { return (core_id / p.cores_per_unit) % p.units; }
    uint32_t masterUnitOf(uint64_t addr) const { return (uint32_t)((addr / p.bytes_per_unit) % p.units); }

    static const uint64_t MSG_VALID = 1ull << 63;
    static const uint32_t MSG_SRC_SHIFT = 48;
    static const uint64_t MSG_TAG_MASK = (1ull << 48) - 1;

   private:
    /* One barrier in flight, held by the SE that coordinates it. */
    struct BarrierState {
        uint32_t participants = 0;  // TableInfo
        uint32_t arrived = 0;
        uint64_t local_list = 0;   // bit per core of the coordinating unit that is waiting
        uint64_t global_list = 0;  // bit per SE that has cores waiting
        g_vector<uint32_t> waiter_core;
        g_vector<uint64_t> waiter_served;  // when the coordinating SPU finished that arrival
        uint64_t first_arrival = 0;
        uint64_t last_arrival = 0;
        bool overflowed = false;  // serviced through main memory (Sec 4.4)
    };

    struct STEntry {
        uint64_t addr = 0;
        bool occupied = false;
    };

    struct Msg {
        uint32_t src_core = 0;
        uint64_t tag = 0;
        uint64_t arrive = 0;
    };

    struct Engine {
        uint64_t spu_free = 0;
        g_vector<STEntry> st;
        g_unordered_map<uint64_t, BarrierState> barriers;  // keyed by variable address
        g_vector<uint32_t> counters;                       // indexing counters (Sec 4.4)
        uint32_t st_used = 0;
    };

    uint64_t serve(uint32_t unit, uint64_t arrival, uint32_t extra_cycles = 0);  // occupy a unit's SPU
    uint32_t counterIdx(uint64_t addr) const { return (uint32_t)((addr >> 6) % p.indexing_counters); }
    bool stAlloc(uint32_t unit, uint64_t addr);  // false = no entry: the variable goes to memory mode (Sec 4.4)
    void stFree(uint32_t unit, uint64_t addr);
    uint64_t barrierArrive(uint32_t core_id, uint64_t addr, bool across_units, uint64_t now);
    uint64_t barrierArriveIdeal(uint32_t core_id, uint64_t addr, uint64_t now);
    void releaseBarrier(uint32_t coord_unit, uint64_t addr, bool across_units);

    Params p;
    uint32_t service_core_cycles;
    uint32_t overflow_mem_core_cycles;
    uint32_t num_cores;
    g_vector<Engine> engines;
    g_vector<uint64_t> release_of_core;  // 0 = nothing decided yet
    g_vector<g_vector<Msg> > inbox;      // one per core, for the software schemes
    g_unordered_map<uint64_t, uint32_t> registered;  // variable address -> participant count from create_syncvar
    lock_t lock;

    Counter s_sync_reqs, s_async_reqs, s_polls, s_local_msgs, s_global_msgs, s_queue_cycles, s_service_cycles,
        s_rejected_opcode, s_barriers, s_barrier_participants, s_barrier_release_cycles, s_barrier_span_cycles,
        s_barrier_skew_cycles, s_st_peak, s_arrivals, s_overflow_msgs, s_overflow_barriers, s_overflow_aliased,
        s_overflow_mem_accesses, s_overflow_mem_cycles, s_st_full_events, s_counter_peak, s_sw_msgs,
        s_sw_global_msgs, s_sw_recv, s_sw_recv_empty, s_sw_msg_wait_cycles;
    VectorCounter s_msgs_per_unit, s_msgs_per_opcode, s_barriers_per_unit;
};

}  // namespace syncron

#endif  // SYNCRON_SYNC_ENGINE_H_
