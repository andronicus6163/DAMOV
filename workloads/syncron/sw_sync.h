// Software synchronization servers: SynCron's Central and Hier baselines (HPCA'21 Sec 6).
//
//   Central  "one dedicated NDP core in the entire NDP system acts as server and coordinates synchronization among
//            all NDP cores", with clients reaching it "via hardware message-passing".
//   Hier     "one NDP core per NDP unit acts as server and coordinates synchronization", servers talking to each
//            other the same way: local servers aggregate their unit's arrivals and one global server joins the units.
//
// The coordination here is real software on a real NDP core: what it costs is whatever an in-order core takes to run
// this code against its own caches and its unit's memory, which is the point of the baseline -- nothing about it is a
// parameter. The simulator only carries the messages (syncron_msg_send / syncron_msg_recv), charging them the same
// per-hop latencies the Synchronization Engine's own messages pay: a crossbar traversal inside a unit, plus Table 5's
// 40 ns inter-unit link when the server is in another unit.
//
// A receive on an empty inbox parks the core until the end of the simulator phase, so a waiting server costs one
// magic op per phase instead of a spin loop -- but it also means a server notices a message no earlier than the next
// phase boundary. That quantisation is zsim's, it applies to every scheme's application-visible numbers, and the
// Ideal scheme measures it.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include "syncron.h"

#define SW_ARRIVE       1u  // client -> its server
#define SW_ARRIVE_UNIT  2u  // local server -> global server: "my unit's participants are all here"
#define SW_RELEASE      3u  // server -> client
#define SW_RELEASE_UNIT 4u  // global server -> local server
#define SW_QUIT         5u  // client -> its server, after its last round
#define SW_QUIT_UNIT    6u  // local server -> global server

static inline uint64_t sw_tag(uint32_t kind, uint32_t group) { return ((uint64_t)kind << 20) | group; }
static inline uint32_t sw_kind(uint64_t tag) { return (uint32_t)(tag >> 20); }
static inline uint32_t sw_group(uint64_t tag) { return (uint32_t)(tag & 0xFFFFF); }

// One barrier, client side: send the arrival to the server that owns this group, then wait for the release message.
static inline void sw_barrier_wait(uint32_t server_core, uint32_t group) {
    syncron_msg_send(server_core, sw_tag(SW_ARRIVE, group));
    for (;;) {
        uint64_t w = syncron_msg_recv();
        if (w && sw_kind(SYNCRON_MSG_TAG(w)) == SW_RELEASE) return;
    }
}

static inline void sw_client_quit(uint32_t server_core) { syncron_msg_send(server_core, sw_tag(SW_QUIT, 0)); }

// What one server core knows. The mutable arrays live in this server's own unit's memory (the arena), because the
// paper's servers "use their local memory hierarchy" -- every count and waiting list the server touches is a real
// access from its own core.
struct SwServer {
    uint32_t my_core;
    uint32_t my_unit;
    uint32_t cores_per_unit;
    uint32_t units_used;
    uint32_t groups;
    uint32_t clients_here;   // clients that send their messages to this server
    int hier;                // 0 = Central (this is the only server), 1 = Hier
    int is_global;           // coordinates across units (Central's server, or Hier's unit-0 server)
    int across;              // the barriers of this run span units
    uint32_t global_core;
    const uint32_t* server_core_of_unit;       // [units_used]
    const uint32_t* group_participants;        // [groups] participants in total
    const uint32_t* group_local_participants;  // [groups] participants of that group inside MY unit
    const uint32_t* group_units;               // [groups] how many units hold participants of that group
    uint32_t max_wait;                         // capacity of one group's waiting list
    uint32_t* count;                           // [groups] arrivals waiting here
    uint32_t* waiters;                         // [groups * max_wait] their core ids
    uint32_t* units_in;                        // [groups] bitmask of units reported in (global server only)
};

static inline void sw_release_local(struct SwServer* s, uint32_t g) {
    uint32_t n = s->count[g];
    for (uint32_t i = 0; i < n; i++) syncron_msg_send(s->waiters[g * s->max_wait + i], sw_tag(SW_RELEASE, g));
    s->count[g] = 0;
}

// The global server knows exactly which units have cores waiting for this group -- they are the ones that reported
// in -- so it wakes only those.
static inline uint32_t sw_popcount(uint32_t x) {
    uint32_t n = 0;
    while (x) { n += x & 1u; x >>= 1; }
    return n;
}

static inline void sw_release_units(struct SwServer* s, uint32_t g) {
    uint32_t mask = s->units_in[g];
    s->units_in[g] = 0;
    for (uint32_t u = 0; u < s->units_used; u++) {
        if (!((mask >> u) & 1u)) continue;
        if (s->server_core_of_unit[u] == s->my_core) sw_release_local(s, g);
        else syncron_msg_send(s->server_core_of_unit[u], sw_tag(SW_RELEASE_UNIT, g));
    }
}

// The server loop. Returns when every client that talks to this server has quit (and, for Hier's global server,
// when every other unit's server has reported the same).
static inline void sw_server_loop(struct SwServer* s) {
    uint32_t quits = 0, unit_quits = 0;
    for (;;) {
        uint64_t w = syncron_msg_recv();
        if (!w) continue;  // empty inbox: the core was parked to the end of the phase
        uint64_t tag = SYNCRON_MSG_TAG(w);
        uint32_t src = (uint32_t)SYNCRON_MSG_SRC(w);
        uint32_t kind = sw_kind(tag), g = sw_group(tag);
        switch (kind) {
            case SW_ARRIVE: {
                s->waiters[g * s->max_wait + s->count[g]] = src;
                s->count[g]++;
                uint32_t need = s->hier ? s->group_local_participants[g] : s->group_participants[g];
                if (s->count[g] < need) break;
                if (!s->hier || !s->across) {
                    sw_release_local(s, g);  // Central sees every participant; a within-unit barrier ends here
                } else if (s->is_global) {
                    s->units_in[g] |= (1u << s->my_unit);
                    if (sw_popcount(s->units_in[g]) == s->group_units[g]) sw_release_units(s, g);
                } else {
                    syncron_msg_send(s->global_core, sw_tag(SW_ARRIVE_UNIT, g));
                }
                break;
            }
            case SW_ARRIVE_UNIT: {
                s->units_in[g] |= (1u << (src / s->cores_per_unit));
                if (sw_popcount(s->units_in[g]) == s->group_units[g]) sw_release_units(s, g);
                break;
            }
            case SW_RELEASE_UNIT: sw_release_local(s, g); break;
            case SW_QUIT: {
                quits++;
                if (quits < s->clients_here) break;
                if (!s->hier) return;                      // Central: every client in the system has finished
                if (!s->is_global) { syncron_msg_send(s->global_core, sw_tag(SW_QUIT_UNIT, 0)); return; }
                unit_quits++;
                if (unit_quits == s->units_used) return;
                break;
            }
            case SW_QUIT_UNIT: {
                unit_quits++;
                if (unit_quits == s->units_used) return;
                break;
            }
            default: fprintf(stderr, "sw_server: core %u got unknown message kind %u\n", s->my_core, kind); return;
        }
    }
}
