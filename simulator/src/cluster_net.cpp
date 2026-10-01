#include "cluster_net.h"
#include <math.h>
#include "bithacks.h"
#include "log.h"

ClusterNet::ClusterNet(const ClusterNetParams& params, uint32_t core_freq_mhz, uint32_t cores)
    : p(params), num_cores(cores) {
    if (!p.nodes || !p.cores_per_node) panic("[cluster] nodes and coresPerNode must be positive");
    if ((uint64_t)p.nodes * p.cores_per_node < cores)
        panic("[cluster] %u nodes x %u cores do not cover the %u simulated cores", p.nodes, p.cores_per_node, cores);
    latency_cycles = (uint64_t)llround(p.latency_ns * core_freq_mhz / 1000.0);
    // bytes / (GB/s) = ns; at least a cycle, so back-to-back messages on one port are ordered.
    port_cycles = p.bw_gbps > 0 ? (uint64_t)ceil(p.msg_bytes / p.bw_gbps * core_freq_mhz / 1000.0) : 0;
    egress_free.resize(p.nodes, 0);
    ingress_free.resize(p.nodes, 0);
    inbox.resize(cores);
    futex_init(&lock);
    info("[cluster] %u nodes x %u cores, %.1f ns (%lu cycles) end to end, %.2f GB/s NIC ports, %u B messages "
         "(%lu cycles/port)", p.nodes, p.cores_per_node, p.latency_ns, latency_cycles, p.bw_gbps, p.msg_bytes,
         port_cycles);
}

void ClusterNet::initStats(AggregateStat* parent) {
    AggregateStat* st = new AggregateStat();
    st->init("cluster", "Cluster network between processors");
    s_msgs.init("msgs", "messages sent"); st->append(&s_msgs);
    s_local_msgs.init("localMsgs", "of those, to a core of the same node (charged nothing)"); st->append(&s_local_msgs);
    s_bytes.init("bytes", "bytes carried between nodes"); st->append(&s_bytes);
    s_port_queue_cycles.init("portQueueCycles", "cycles messages waited for a busy NIC port");
    st->append(&s_port_queue_cycles);
    s_recv.init("recv", "messages received"); st->append(&s_recv);
    s_recv_empty.init("recvEmpty", "receives that found the inbox empty (core parked to the phase end)");
    st->append(&s_recv_empty);
    s_wait_cycles.init("waitCycles", "cycles receivers stalled for a message still in flight"); st->append(&s_wait_cycles);
    parent->append(st);
}

void ClusterNet::send(uint32_t src_core, uint32_t dst_core, uint64_t tag, uint64_t now) {
    if (dst_core >= num_cores) panic("[cluster] message to core %u, which does not exist", dst_core);
    if (tag > MSG_TAG_MASK) panic("[cluster] message tag 0x%lx does not fit in 48 bits", tag);
    futex_lock(&lock);
    uint32_t sn = nodeOf(src_core), dn = nodeOf(dst_core);
    Msg m = {src_core, tag, now};
    s_msgs.inc();
    if (sn == dn) {
        s_local_msgs.inc();  // not how this baseline talks inside a node; kept free so it cannot skew anything
    } else {
        uint64_t out = MAX(now, egress_free[sn]);
        egress_free[sn] = out + port_cycles;
        uint64_t at = out + port_cycles + latency_cycles;
        uint64_t in = MAX(at, ingress_free[dn]);
        ingress_free[dn] = in + port_cycles;
        s_port_queue_cycles.inc((out - now) + (in - at));
        s_bytes.inc(p.msg_bytes);
        m.arrive = in + port_cycles;
    }
    inbox[dst_core].push_back(m);
    futex_unlock(&lock);
}

bool ClusterNet::recv(uint32_t core, uint64_t now, uint64_t* word, uint64_t* resume) {
    futex_lock(&lock);
    g_vector<Msg>& q = inbox[core];
    size_t best = q.size();
    for (size_t i = 0; i < q.size(); i++) {
        if (best == q.size() || q[i].arrive < q[best].arrive) best = i;
    }
    bool found = best < q.size();
    if (found) {
        Msg m = q[best];
        q.erase(q.begin() + best);
        *resume = MAX(now, m.arrive);
        s_wait_cycles.inc(*resume - now);
        *word = MSG_VALID | ((uint64_t)m.src_core << MSG_SRC_SHIFT) | (m.tag & MSG_TAG_MASK);
        s_recv.inc();
    } else {
        s_recv_empty.inc();
    }
    futex_unlock(&lock);
    return found;
}
