#include "syncron/sync_engine.h"

#include "bithacks.h"
#include "log.h"
#include "zsim.h"

namespace syncron {

SyncronSystem::SyncronSystem(const Params& params, uint32_t core_freq_mhz, uint32_t cores)
    : p(params), num_cores(cores) {
    // Table 5's 12 SPU cycles at 1 GHz, in the core clock the rest of the simulator uses.
    service_core_cycles = (uint32_t)((uint64_t)p.service_cycles * core_freq_mhz / p.spu_mhz);
    if (!service_core_cycles) service_core_cycles = 1;
    overflow_mem_core_cycles = p.overflow_mem_cycles;
    if (p.cores_per_unit > 64) panic("[SYNCRON] a local waiting list is one bit per core: %u > 64", p.cores_per_unit);
    if (p.units > 64) panic("[SYNCRON] a global waiting list is one bit per SE: %u > 64", p.units);
    if (p.scheme >= SCHEME_COUNT) panic("[SYNCRON] unknown scheme %u", p.scheme);
    engines.resize(p.units);
    for (uint32_t u = 0; u < p.units; u++) {
        engines[u].st.resize(p.st_entries);
        engines[u].counters.resize(p.indexing_counters, 0);
    }
    release_of_core.resize(num_cores, 0);
    inbox.resize(num_cores);
    futex_init(&lock);
    info("[SYNCRON] scheme %s, %u units x %u cores, SPU %u MHz x %u cycles = %u core cycles/message, "
         "core<->SE %u cyc, SE<->SE %u cyc, ST %u entries, %u counters, overflow access %u cyc, %lu MB per unit",
         scheme_name(p.scheme), p.units, p.cores_per_unit, p.spu_mhz, p.service_cycles, service_core_cycles,
         p.local_msg_cycles, p.global_msg_cycles, p.st_entries, p.indexing_counters, overflow_mem_core_cycles,
         p.bytes_per_unit >> 20);
}

uint64_t SyncronSystem::serve(uint32_t unit, uint64_t arrival, uint32_t extra_cycles) {
    Engine& e = engines[unit];
    uint64_t start = MAX(arrival, e.spu_free);
    s_queue_cycles.inc(start - arrival);
    s_service_cycles.inc(service_core_cycles + extra_cycles);
    e.spu_free = start + service_core_cycles + extra_cycles;
    s_msgs_per_unit.inc(unit);
    return e.spu_free;
}

/* Reserve an ST entry. False means the table is full, which is Sec 4.4's overflow condition -- not an error. */
bool SyncronSystem::stAlloc(uint32_t unit, uint64_t addr) {
    Engine& e = engines[unit];
    for (uint32_t i = 0; i < p.st_entries; i++) {
        if (e.st[i].occupied && e.st[i].addr == addr) return true;  // already tracked
    }
    for (uint32_t i = 0; i < p.st_entries; i++) {
        if (!e.st[i].occupied) {
            e.st[i].occupied = true;
            e.st[i].addr = addr;
            e.st_used++;
            if (e.st_used > s_st_peak.get()) s_st_peak.set(e.st_used);
            return true;
        }
    }
    return false;
}

void SyncronSystem::stFree(uint32_t unit, uint64_t addr) {
    Engine& e = engines[unit];
    for (uint32_t i = 0; i < p.st_entries; i++) {
        if (e.st[i].occupied && e.st[i].addr == addr) {
            e.st[i].occupied = false;
            e.st_used--;
            return;
        }
    }
}

/* One participant arrives. Returns its release cycle if this arrival completed the barrier and the caller is the
 * last participant, otherwise 0 (the caller polls). */
uint64_t SyncronSystem::barrierArrive(uint32_t core_id, uint64_t addr, bool across_units, uint64_t now) {
    uint32_t unit = unitOfCore(core_id);
    uint32_t coord = across_units ? masterUnitOf(addr) : unit;
    s_arrivals.inc();

    Engine& ce = engines[coord];
    BarrierState& b = ce.barriers[addr];
    if (!b.participants) {
        auto it = registered.find(addr);
        if (it == registered.end()) {
            panic("[SYNCRON] core %u waited on a barrier at 0x%lx that was never created (create_syncvar)", core_id,
                  addr);
        }
        b.participants = it->second;
        // Sec 4.4. A counter above zero says some variable with these address LSBs is already being serviced through
        // main memory: this one joins it (the paper's counters cannot tell the two apart -- that is the design, and
        // `overflowAliased` counts how often it costs a variable that the ST had room for). Otherwise try the ST, and
        // fall back to memory when it is full.
        uint32_t idx = counterIdx(addr);
        if (ce.counters[idx] > 0) {
            b.overflowed = true;
            s_overflow_aliased.inc();
        } else if (!stAlloc(coord, addr)) {
            b.overflowed = true;
            s_st_full_events.inc();
        }
        if (b.overflowed) {
            ce.counters[idx]++;
            if (ce.counters[idx] > s_counter_peak.get()) s_counter_peak.set(ce.counters[idx]);
            s_overflow_barriers.inc();
        }
    }

    // In memory mode the Master SE's SPU also reads and writes syncronVar for this message (Sec 4.4).
    uint32_t extra = 0;
    if (b.overflowed) {
        extra = p.overflow_arrive_accesses * overflow_mem_core_cycles;
        s_overflow_msgs.inc();
        s_overflow_mem_accesses.inc(p.overflow_arrive_accesses);
        s_overflow_mem_cycles.inc(extra);
    }

    uint64_t served;
    if (coord == unit) {
        // Core -> its local SE, which is also the coordinator.
        served = serve(unit, now + p.local_msg_cycles, extra);
        s_local_msgs.inc();
    } else {
        // Core -> its local SE, which redirects to the Master SE (Sec 4.3); both SPUs serve the message.
        served = serve(unit, now + p.local_msg_cycles);
        s_local_msgs.inc();
        served = serve(coord, served + p.global_msg_cycles, extra);
        s_global_msgs.inc();
    }

    if (!b.arrived) b.first_arrival = served;
    b.arrived++;
    b.last_arrival = MAX(b.last_arrival, served);
    if (unit == coord) b.local_list |= (1ull << (core_id % p.cores_per_unit));
    b.global_list |= (1ull << unit);
    b.waiter_core.push_back(core_id);
    b.waiter_served.push_back(served);

    if (b.arrived < b.participants) return 0;  // not everybody is here yet
    releaseBarrier(coord, addr, across_units);
    uint64_t mine = release_of_core[core_id];
    release_of_core[core_id] = 0;
    return mine;  // the last arriver learns its release cycle immediately
}

/* Ideal (Sec 6): zero performance overhead for synchronization. No message, no SPU, no ST -- every participant is
 * released at the cycle the last one arrives. Unit 0's engine holds the bookkeeping; it costs nothing. */
uint64_t SyncronSystem::barrierArriveIdeal(uint32_t core_id, uint64_t addr, uint64_t now) {
    s_arrivals.inc();
    Engine& e = engines[0];
    BarrierState& b = e.barriers[addr];
    if (!b.participants) {
        auto it = registered.find(addr);
        if (it == registered.end()) {
            panic("[SYNCRON] core %u waited on a barrier at 0x%lx that was never created (create_syncvar)", core_id,
                  addr);
        }
        b.participants = it->second;
        b.first_arrival = now;
    }
    b.arrived++;
    b.last_arrival = MAX(b.last_arrival, now);
    b.waiter_core.push_back(core_id);
    if (b.arrived < b.participants) return 0;

    uint64_t release = b.last_arrival;
    for (size_t i = 0; i < b.waiter_core.size(); i++) release_of_core[b.waiter_core[i]] = release;
    s_barriers.inc();
    s_barriers_per_unit.inc(0);
    s_barrier_participants.inc(b.participants);
    s_barrier_release_cycles.inc(0);
    s_barrier_span_cycles.inc(release - b.first_arrival);
    s_barrier_skew_cycles.inc(b.last_arrival - b.first_arrival);
    e.barriers.erase(addr);
    uint64_t mine = release_of_core[core_id];
    release_of_core[core_id] = 0;
    return mine;
}

/* Every participant has arrived: the coordinating SE sends the departure messages. Each departure occupies an SPU,
 * which is what makes the release cost grow with the number of participants. */
void SyncronSystem::releaseBarrier(uint32_t coord_unit, uint64_t addr, bool across_units) {
    Engine& e = engines[coord_unit];
    BarrierState& b = e.barriers[addr];
    uint64_t last_release = 0;
    if (b.overflowed) {
        // Sec 4.4: the lists live in memory, so the Master SE's SPU reads them (and writes them back cleared) before
        // it can send the departures. Not a message, so it is not counted as one -- only as SPU occupancy.
        uint32_t cycles = p.overflow_release_accesses * overflow_mem_core_cycles;
        uint64_t start = MAX(b.last_arrival, e.spu_free);
        e.spu_free = start + cycles;
        s_service_cycles.inc(cycles);
        s_overflow_mem_accesses.inc(p.overflow_release_accesses);
        s_overflow_mem_cycles.inc(cycles);
    }
    for (size_t i = 0; i < b.waiter_core.size(); i++) {
        uint32_t core = b.waiter_core[i];
        uint32_t unit = unitOfCore(core);
        // barrier_depart_*: the coordinating SE serves one departure message per waiting participant.
        uint64_t sent = serve(coord_unit, MAX(b.last_arrival, e.spu_free));
        s_local_msgs.inc();
        uint64_t release;
        if (across_units && unit != coord_unit) {
            // ... to the participant's local SE over the inter-unit link, which then serves its own departure.
            uint64_t at_local = serve(unit, sent + p.global_msg_cycles);
            s_global_msgs.inc();
            release = at_local + p.local_msg_cycles;
        } else {
            release = sent + p.local_msg_cycles;
        }
        release_of_core[core] = release;
        last_release = MAX(last_release, release);
    }
    s_barriers.inc();
    s_barriers_per_unit.inc(coord_unit);
    s_barrier_participants.inc(b.participants);
    s_barrier_release_cycles.inc(last_release - b.last_arrival);  // cost once everybody has arrived
    s_barrier_span_cycles.inc(last_release - b.first_arrival);    // cost including arrival skew
    s_barrier_skew_cycles.inc(b.last_arrival - b.first_arrival);
    if (b.overflowed) {
        // The variable's lists are empty again: it leaves memory mode (Sec 4.4's release-type decrement). For a
        // barrier only the coordinating SE ever held the variable -- Sec 4.3 keeps local SEs out of it -- so there is
        // no other counter to decrease and no decrease_indexing_counter message to send.
        uint32_t idx = counterIdx(addr);
        if (e.counters[idx]) e.counters[idx]--;
    } else {
        stFree(coord_unit, addr);
    }
    e.barriers.erase(addr);
}

void SyncronSystem::msgSend(uint32_t src_core, uint32_t dst_core, uint64_t tag, uint64_t now) {
    if (dst_core >= num_cores) panic("[SYNCRON] message to core %u, which does not exist", dst_core);
    if (tag > MSG_TAG_MASK) panic("[SYNCRON] message tag 0x%lx does not fit in 48 bits", tag);
    futex_lock(&lock);
    bool remote = unitOfCore(src_core) != unitOfCore(dst_core);
    Msg m;
    m.src_core = src_core;
    m.tag = tag;
    // The same per-hop costs the SE's own messages pay: the unit's crossbar, plus Table 5's inter-unit link when the
    // server is in another unit.
    m.arrive = now + p.local_msg_cycles + (remote ? p.global_msg_cycles : 0);
    inbox[dst_core].push_back(m);
    s_sw_msgs.inc();
    if (remote) s_sw_global_msgs.inc();
    futex_unlock(&lock);
}

bool SyncronSystem::msgRecv(uint32_t core_id, uint64_t now, uint64_t* word, uint64_t* resume) {
    futex_lock(&lock);
    g_vector<Msg>& q = inbox[core_id];
    size_t best = q.size();
    for (size_t i = 0; i < q.size(); i++) {
        if (best == q.size() || q[i].arrive < q[best].arrive) best = i;  // a queue delivers in arrival order
    }
    bool found = best < q.size();
    if (found) {
        Msg m = q[best];
        q.erase(q.begin() + best);
        *resume = MAX(now, m.arrive);
        s_sw_msg_wait_cycles.inc(*resume - now);
        *word = MSG_VALID | ((uint64_t)m.src_core << MSG_SRC_SHIFT) | (m.tag & MSG_TAG_MASK);
        s_sw_recv.inc();
    } else {
        s_sw_recv_empty.inc();
    }
    futex_unlock(&lock);
    return found;
}

uint64_t SyncronSystem::reqSync(uint32_t core_id, uint64_t addr, uint32_t opcode, uint64_t info, uint64_t now) {
    futex_lock(&lock);
    if (opcode < OP_COUNT) s_msgs_per_opcode.inc(opcode);
    uint64_t ack = 0;
    switch (opcode) {
        case OP_PARK: {
            // Not in the paper and not a message: a core with no role in this run asks to be parked to the end of the
            // phase. It stays scheduled on its core (so the core layout the harness verified does not shift) without
            // spending simulated instructions.
            break;
        }
        case OP_BARRIER_POLL: {
            // Not a message: this only asks whether the release cycle has been decided (see the header).
            s_polls.inc();
            ack = release_of_core[core_id];
            if (ack) release_of_core[core_id] = 0;
            break;
        }
        case OP_PING: {
            s_sync_reqs.inc();
            ack = serve(unitOfCore(core_id), now + p.local_msg_cycles) + p.local_msg_cycles;
            s_local_msgs.inc();
            break;
        }
        case OP_REGISTER_VAR: {
            s_sync_reqs.inc();
            if (!info) panic("[SYNCRON] create_syncvar at 0x%lx needs a participant count", addr);
            registered[addr] = (uint32_t)info;
            if (p.scheme == SCHEME_IDEAL) break;  // no engine takes part
            ack = serve(unitOfCore(core_id), now + p.local_msg_cycles) + p.local_msg_cycles;
            s_local_msgs.inc();
            break;
        }
        case OP_UNREGISTER_VAR: {
            s_sync_reqs.inc();
            registered.erase(addr);
            if (p.scheme == SCHEME_IDEAL) break;
            ack = serve(unitOfCore(core_id), now + p.local_msg_cycles) + p.local_msg_cycles;
            s_local_msgs.inc();
            break;
        }
        case OP_BARRIER_WITHIN_UNIT:
        case OP_BARRIER_ACROSS_UNITS: {
            s_sync_reqs.inc();
            if (p.scheme == SCHEME_CENTRAL || p.scheme == SCHEME_HIER) {
                futex_unlock(&lock);
                panic("[SYNCRON] scheme %s coordinates barriers in software on a server core; core %u used the "
                      "hardware barrier opcode instead", scheme_name(p.scheme), core_id);
            }
            ack = (p.scheme == SCHEME_IDEAL)
                      ? barrierArriveIdeal(core_id, addr, now)
                      : barrierArrive(core_id, addr, opcode == OP_BARRIER_ACROSS_UNITS, now);
            break;
        }
        default:
            s_rejected_opcode.inc();
            futex_unlock(&lock);
            panic("[SYNCRON] core %u issued opcode %u (%s), which is not implemented yet", core_id, opcode,
                  opcode_name(opcode));
    }
    futex_unlock(&lock);
    return ack;
}

void SyncronSystem::reqAsync(uint32_t core_id, uint64_t addr, uint32_t opcode, uint64_t info, uint64_t now) {
    futex_lock(&lock);
    s_async_reqs.inc();
    if (opcode < OP_COUNT) s_msgs_per_opcode.inc(opcode);
    serve(unitOfCore(core_id), now + p.local_msg_cycles);  // the SPU is occupied even though nobody waits
    s_local_msgs.inc();
    futex_unlock(&lock);
}

bool SyncronSystem::takeRelease(uint32_t core_id, uint64_t* release_cycle) {
    futex_lock(&lock);
    uint64_t r = release_of_core[core_id];
    if (r) release_of_core[core_id] = 0;
    futex_unlock(&lock);
    *release_cycle = r;
    return r != 0;
}

void SyncronSystem::initStats(AggregateStat* parent) {
    AggregateStat* st = new AggregateStat();
    st->init("syncron", "SynCron synchronization engine stats");
    s_sync_reqs.init("reqSync", "req_sync instructions that carried a message"); st->append(&s_sync_reqs);
    s_async_reqs.init("reqAsync", "req_async instructions"); st->append(&s_async_reqs);
    s_polls.init("barrierPolls", "release-cycle checks (no modelled cost; a simulator artefact)");
    st->append(&s_polls);
    s_local_msgs.init("localMsgs", "messages between a core and its local SE"); st->append(&s_local_msgs);
    s_global_msgs.init("globalMsgs", "messages between a local SE and a Master SE"); st->append(&s_global_msgs);
    s_queue_cycles.init("queueCycles", "cycles messages waited for a busy SPU"); st->append(&s_queue_cycles);
    s_service_cycles.init("serviceCycles", "cycles SPUs spent serving messages"); st->append(&s_service_cycles);
    s_rejected_opcode.init("rejectedOpcodes", "messages with an unimplemented opcode"); st->append(&s_rejected_opcode);
    s_barriers.init("barriers", "barriers completed"); st->append(&s_barriers);
    s_barrier_participants.init("barrierParticipants", "participants summed over all barriers");
    st->append(&s_barrier_participants);
    s_barrier_release_cycles.init("barrierReleaseCycles",
                                  "summed last release - last arrival (modelled barrier cost)");
    st->append(&s_barrier_release_cycles);
    s_barrier_span_cycles.init("barrierSpanCycles", "summed last release - first arrival");
    st->append(&s_barrier_span_cycles);
    s_barrier_skew_cycles.init("barrierSkewCycles", "summed last arrival - first arrival");
    st->append(&s_barrier_skew_cycles);
    s_st_peak.init("stPeakEntries", "most Synchronization Table entries in use at once, any unit");
    st->append(&s_st_peak);
    s_arrivals.init("arrivals", "barrier arrivals (acquire-type requests)"); st->append(&s_arrivals);
    s_overflow_msgs.init("overflowMsgs", "arrivals serviced through main memory (Sec 4.4); / arrivals = the paper's "
                                         "overflow rate");
    st->append(&s_overflow_msgs);
    s_overflow_barriers.init("overflowBarriers", "barrier instances that ran in memory mode");
    st->append(&s_overflow_barriers);
    s_overflow_aliased.init("overflowAliased", "of those, ones the ST had room for: another variable with the same "
                                               "address LSBs held the indexing counter");
    st->append(&s_overflow_aliased);
    s_overflow_mem_accesses.init("overflowMemAccesses", "syncronVar reads/writes by Master SEs (modelled as SPU "
                                                        "occupancy, not injected into the memory model)");
    st->append(&s_overflow_mem_accesses);
    s_overflow_mem_cycles.init("overflowMemCycles", "SPU cycles spent on those accesses");
    st->append(&s_overflow_mem_cycles);
    s_st_full_events.init("stFullEvents", "variables that found the Synchronization Table full");
    st->append(&s_st_full_events);
    s_counter_peak.init("indexingCounterPeak", "highest value any indexing counter reached");
    st->append(&s_counter_peak);
    s_sw_msgs.init("swMsgs", "hardware messages sent between cores (Central/Hier)"); st->append(&s_sw_msgs);
    s_sw_global_msgs.init("swGlobalMsgs", "of those, ones that crossed NDP units"); st->append(&s_sw_global_msgs);
    s_sw_recv.init("swMsgsReceived", "messages taken out of an inbox"); st->append(&s_sw_recv);
    s_sw_recv_empty.init("swRecvEmpty", "receives that found an empty inbox (the core waits a phase)");
    st->append(&s_sw_recv_empty);
    s_sw_msg_wait_cycles.init("swMsgWaitCycles", "cycles a server waited for a message that was still in flight");
    st->append(&s_sw_msg_wait_cycles);
    s_msgs_per_unit.init("msgsPerUnit", "messages served, per NDP unit", p.units); st->append(&s_msgs_per_unit);
    s_msgs_per_opcode.init("msgsPerOpcode", "messages issued, per opcode", OP_COUNT); st->append(&s_msgs_per_opcode);
    s_barriers_per_unit.init("barriersPerUnit", "barriers coordinated, per NDP unit", p.units);
    st->append(&s_barriers_per_unit);
    parent->append(st);
}

}  // namespace syncron
