/* A cluster network between the processors of a multi-processor run: the interconnect the MPI ranks of a hybrid job
 * talk over. zsim has no network between processors, so this is the one place message timing between them comes
 * from; memory and caches are still zsim's own (one core group, per-processor L3s).
 *
 *   sys.cluster = { nodes; coresPerNode; latencyNs; bwGBps; msgBytes; }
 *
 * MODELLED: a single-switch cluster. A message leaves its node through that node's NIC port, crosses the network in
 * `latencyNs`, and enters the destination node through its NIC port. Each port is busy for msgBytes / bwGBps per
 * message, so messages from (or to) one node serialise on its port. The latency is end to end: it is meant to include
 * the MPI library's per-message send and receive overhead, which is why nothing is charged on the cores for sending.
 *
 * NOT MODELLED: switch-internal contention, packetisation, adaptive routing, the MPI progress engine as instructions.
 * A barrier's traffic is a handful of zero-byte messages per round, none of which contend in a non-blocking switch.
 *
 * The application reaches it with three magic ops (misc/hooks/zsim_hooks.h): net_send (non-blocking), net_recv
 * (takes the earliest message and stalls the core until it has arrived; an empty inbox parks the core to the end of
 * the phase and returns 0) and park (idle to the end of the phase, for waits outside the measurement).
 */
#ifndef CLUSTER_NET_H_
#define CLUSTER_NET_H_

#include "g_std/g_vector.h"
#include "galloc.h"
#include "locks.h"
#include "stats.h"

struct ClusterNetParams {
    uint32_t nodes = 1;
    uint32_t cores_per_node = 64;
    double latency_ns = 1000.0;
    double bw_gbps = 12.5;
    uint32_t msg_bytes = 64;
};

class ClusterNet : public GlobAlloc {
   public:
    static const uint64_t MSG_VALID = 1ull << 63;
    static const uint32_t MSG_SRC_SHIFT = 48;
    static const uint64_t MSG_TAG_MASK = (1ull << 48) - 1;

    ClusterNet(const ClusterNetParams& p, uint32_t core_freq_mhz, uint32_t num_cores);
    void initStats(AggregateStat* parent);

    void send(uint32_t src_core, uint32_t dst_core, uint64_t tag, uint64_t now);
    /* Takes the earliest-arriving message for this core. False if the inbox is empty; otherwise *word is
     * MSG_VALID | src core << 48 | tag and *resume the cycle the message is in. */
    bool recv(uint32_t core, uint64_t now, uint64_t* word, uint64_t* resume);

    uint32_t nodeOf(uint32_t core) const { return core / p.cores_per_node; }

   private:
    struct Msg {
        uint32_t src_core;
        uint64_t tag;
        uint64_t arrive;
    };
    ClusterNetParams p;
    uint32_t num_cores;
    uint64_t latency_cycles, port_cycles;
    g_vector<uint64_t> egress_free, ingress_free;  // per node: when its NIC port is free next
    g_vector<g_vector<Msg> > inbox;                 // per core
    lock_t lock;

    Counter s_msgs, s_local_msgs, s_bytes, s_port_queue_cycles, s_recv, s_recv_empty, s_wait_cycles;
};

#endif  // CLUSTER_NET_H_
