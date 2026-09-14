#ifdef _WITH_RAMULATOR2_

#include "ramulator2_mem_ctrl.h"
#include <algorithm>
#include "event_recorder.h"
#include "tick_event.h"
#include "timing_event.h"
#include "zsim.h"
#include "ramulator/capi/ramulator_capi.h"

class Ramulator2AccEvent : public TimingEvent {
  private:
    Ramulator2* dram;
    bool write;
    Address addr;
    uint32_t coreId;

  public:
    uint64_t sCycle;

    Ramulator2AccEvent(Ramulator2* _dram, bool _write, Address _addr, int32_t domain, uint32_t _coreId)
        : TimingEvent(0, 0, domain), dram(_dram), write(_write), addr(_addr), coreId(_coreId), sCycle(0) {}

    bool isWrite() const { return write; }
    Address getAddr() const { return addr; }
    uint32_t getCoreId() const { return coreId; }

    void simulate(uint64_t startCycle) {
        sCycle = startCycle;
        dram->enqueue(this, startCycle);
    }
};

Ramulator2::Ramulator2(const std::string& configFile, unsigned numCores, unsigned lineSize, uint32_t _minLatency,
                       uint32_t _domain, const g_string& _name, const std::string& _statsPath)
    : name(_name), domain(_domain), minLatency(_minLatency), statsPath(_statsPath), curCycle(0) {
    sim = r2_create_from_file_cores(configFile.c_str(), numCores);
    if (!sim) panic("[RAMULATOR2] Cannot create memory system from %s: %s", configFile.c_str(), r2_last_error());

    sizeBytes = std::min<int>(lineSize, r2_get_tx_bytes(sim));
    info("[RAMULATOR2] %s: tCK %.3f ns, %u cores, request size %d bytes", configFile.c_str(), r2_get_tck_ns(sim),
         numCores, sizeBytes);

    TickEvent<Ramulator2>* tickEv = new TickEvent<Ramulator2>(this, domain);
    tickEv->queue(0);
}

Ramulator2::~Ramulator2() {
    r2_destroy(sim);
}

void Ramulator2::initStats(AggregateStat* parentStat) {
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Memory controller stats");
    profReads.init("rd", "Read requests"); memStats->append(&profReads);
    profWrites.init("wr", "Write requests"); memStats->append(&profWrites);
    profTotalRdLat.init("rdlat", "Total latency experienced by read requests"); memStats->append(&profTotalRdLat);
    profTotalWrLat.init("wrlat", "Total latency experienced by write requests"); memStats->append(&profTotalWrLat);
    reissuedAccesses.init("reissuedAccesses", "Number of accesses that were reissued due to full queue"); memStats->append(&reissuedAccesses);
    parentStat->append(memStats);
}

uint64_t Ramulator2::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL) ? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }

    if (req.type == PUTS) return req.cycle;

    bool isWrite = (req.type == PUTX);
    uint64_t respCycle = req.cycle + minLatency;

    if (zinfo->eventRecorders[req.srcId]) {
        Address addr = req.lineAddr << lineBits;
        Ramulator2AccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) Ramulator2AccEvent(this, isWrite, addr, domain, req.srcId);
        memEv->setMinStartCycle(req.cycle);
        TimingRecord tr = {addr, req.cycle, respCycle, req.type, memEv, memEv};
        zinfo->eventRecorders[req.srcId]->pushRecord(tr);
    }
    return respCycle;
}

bool Ramulator2::trySend(Ramulator2AccEvent* ev) {
    ev->hold();
    int ok = r2_send(sim, ev->isWrite() ? R2_WRITE : R2_READ, ev->getAddr(), ev->getCoreId(), sizeBytes,
                     reinterpret_cast<uint64_t>(ev), &Ramulator2::onComplete, this);
    if (ok < 0) panic("[RAMULATOR2] send failed: %s", r2_last_error());
    if (!ok) ev->release();
    return ok;
}

uint32_t Ramulator2::tick(uint64_t cycle) {
    r2_tick(sim);
    if (!overflowQueue.empty() && trySend(overflowQueue.front())) overflowQueue.pop_front();
    curCycle++;
    return 1;
}

void Ramulator2::enqueue(Ramulator2AccEvent* ev, uint64_t cycle) {
    if (!overflowQueue.empty() || !trySend(ev)) {
        overflowQueue.push_back(ev);
        reissuedAccesses.inc();
    }
}

void Ramulator2::onComplete(void* ctx, uint64_t token, uint64_t addr, int type, int sourceId) {
    Ramulator2* self = static_cast<Ramulator2*>(ctx);
    Ramulator2AccEvent* ev = reinterpret_cast<Ramulator2AccEvent*>(token);
    uint32_t lat = self->curCycle + 1 - ev->sCycle;

    if (ev->isWrite()) {
        self->profWrites.inc();
        self->profTotalWrLat.inc(lat);
    } else {
        self->profReads.inc();
        self->profTotalRdLat.inc(lat);
    }

    ev->release();
    ev->done(self->curCycle + 1);
}

void Ramulator2::finish() {
    r2_finalize(sim);
    if (r2_write_stats(sim, statsPath.c_str()) != 0) warn("[RAMULATOR2] Cannot write stats to %s: %s", statsPath.c_str(), r2_last_error());
}

#endif
