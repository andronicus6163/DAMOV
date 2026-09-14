#ifndef RAMULATOR2_MEM_CTRL_H_
#define RAMULATOR2_MEM_CTRL_H_

#ifdef _WITH_RAMULATOR2_

#include <deque>
#include <string>
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "stats.h"

struct r2_sim;
class Ramulator2AccEvent;

class Ramulator2 : public MemObject {
  private:
    g_string name;
    uint32_t domain;
    uint32_t minLatency;
    int sizeBytes;
    uint32_t reqFlags;
    std::string statsPath;
    r2_sim* sim;
    uint64_t curCycle;
    bool nsClock;
    double cpuNs;
    double memNs;
    double diffNs;
    std::deque<Ramulator2AccEvent*> overflowQueue;

    PAD();
    Counter profReads;
    Counter profWrites;
    Counter profTotalRdLat;
    Counter profTotalWrLat;
    Counter reissuedAccesses;
    Counter memTicks;
    PAD();

    bool trySend(Ramulator2AccEvent* ev);
    static void onComplete(void* ctx, uint64_t token, uint64_t addr, int type, int sourceId, int hops);

  public:
    Ramulator2(const std::string& configFile, unsigned numCores, unsigned lineSize, uint32_t minLatency,
               uint32_t domain, const g_string& name, const std::string& statsPath, uint32_t cpuFreqMHz,
               const std::string& clockMode, bool pimMode);
    ~Ramulator2();

    const char* getName() { return name.c_str(); }
    void initStats(AggregateStat* parentStat);
    uint64_t access(MemReq& req);
    uint32_t tick(uint64_t cycle);
    void enqueue(Ramulator2AccEvent* ev, uint64_t cycle);
    void finish();
};

#endif
#endif
