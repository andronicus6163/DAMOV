// barrier: the barrier primitive under each of SynCron's four systems (HPCA'21 Sec 6), measured the way the
// cross-platform sync study measures barriers.
//
//   barrier --scheme syncron|ideal|central|hier [--scope unit|units] [--clients-per-unit 15] [--units-used 4]
//           [--groups 1] [--rounds 20] [--warmup 2] [--units 4] [--cores-per-unit 16] [--stack-mb 1024]
//
//   --scheme syncron  the Synchronization Engine: barrier_wait_within_unit / barrier_wait_across_units
//           ideal     zero-overhead synchronization (the paper's performance ceiling); the same code path, with the
//                     engine charging nothing, so what is left is only zsim's phase granularity
//           central   one dedicated NDP core in the whole system runs a software server, reached by hardware messages
//           hier      one dedicated NDP core per unit runs a local server; one of them also joins the units
//   --scope unit   participants of ONE NDP unit, coordinated inside that unit
//          units   participants spread over the units; the variable's Master SE (or the global server) coordinates
//   --groups G     independent barriers running at once: G per unit for --scope unit, G in total for --scope units.
//                  More live variables than the Synchronization Table has entries is what drives it into Sec 4.4's
//                  overflow mode, so this is the knob the overflow experiment turns.
//
// CORE LAYOUT. The harness always starts one thread per core, in core order, and each thread asks the simulator which
// core it is on (getcpu) rather than assuming. Core 0 of every unit is reserved whenever clients-per-unit is below
// cores-per-unit: it runs a software server under central/hier and is left idle otherwise, so all four schemes put
// their clients on exactly the same cores -- the paper's convention of 15 client cores per unit, one core per unit
// spent on a software server. A core with no role parks itself to the end of each simulator phase (it must keep
// running to keep its core, but it costs one magic op per phase rather than a spin loop).
//
// Each participant records, on its own core clock, when it arrived at each barrier and when it came out:
//   period    = (last arrival - first arrival) / barriers, on one core's clock -- the cross-platform metric
//   wait      = release - arrive per participant; min over participants is the last arriver, max the first
// For syncron and ideal the engine additionally reports the *modelled* cost (barrierReleaseCycles / barriers), which
// is exact. Central and Hier have no such number by construction: their coordination is software running on a real
// core, so their cost is only what the application sees. Compare app-visible numbers across schemes, and read Ideal's
// as the floor this simulator's phase granularity imposes.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "ndp_arena.h"
#include "sw_sync.h"
#include "syncron.h"

namespace {

enum Scheme { SCH_SYNCRON, SCH_IDEAL, SCH_CENTRAL, SCH_HIER };
enum Role { R_IDLE, R_CLIENT, R_SERVER };

const char* scheme_names[] = {"syncron", "ideal", "central", "hier"};

struct Sample {
    uint64_t arrive = 0;
    uint64_t release = 0;
};

struct Layout {
    uint32_t units = 4, cores_per_unit = 16, clients_per_unit = 15, units_used = 4, groups = 1;
    bool across = true;
    Scheme scheme = SCH_SYNCRON;
    bool reserved() const { return clients_per_unit < cores_per_unit; }
    uint32_t cores() const { return units * cores_per_unit; }
    uint32_t clients() const { return clients_per_unit * units_used; }
    uint32_t total_groups() const { return across ? groups : groups * units_used; }
    // Core 0 of a unit is the server core; Central uses only unit 0's.
    uint32_t server_core_of_unit(uint32_t u) const { return u * cores_per_unit; }
    uint32_t global_server_core() const { return 0; }
};

Role role_of(const Layout& L, uint32_t core, uint32_t* idx_in_unit) {
    uint32_t u = core / L.cores_per_unit, j = core % L.cores_per_unit;
    if (u >= L.units_used) return R_IDLE;
    if (L.reserved() && j == 0) {
        if (L.scheme == SCH_HIER) return R_SERVER;
        if (L.scheme == SCH_CENTRAL && u == 0) return R_SERVER;
        return R_IDLE;
    }
    if (j < L.cores_per_unit - L.clients_per_unit) return R_IDLE;
    *idx_in_unit = j - (L.cores_per_unit - L.clients_per_unit);
    return R_CLIENT;
}

uint32_t group_of_client(const Layout& L, uint32_t unit, uint32_t idx_in_unit) {
    if (L.across) return (unit * L.clients_per_unit + idx_in_unit) % L.groups;
    return unit * L.groups + (idx_in_unit % L.groups);
}

// Which unit's memory holds a group's variable -- and therefore which SE is its Master (Sec 4.3).
uint32_t master_unit_of_group(const Layout& L, uint32_t g) {
    return L.across ? (g % L.units_used) : (g / L.groups);
}

struct Task {
    pthread_t th;
    uint32_t expect_core = 0;
    uint32_t core = (uint32_t)-1;
    uint32_t core_after_gate = (uint32_t)-1;
    uint32_t unit = 0;
    uint32_t idx_in_unit = 0;
    uint32_t group = 0;
    uint32_t server_core = 0;
    Role role = R_IDLE;
    uint32_t rounds = 0;
    const Layout* L = nullptr;
    const SyncVar* var = nullptr;
    SwServer server;
    // In the client's own unit's memory: it is written twice per round inside the measurement, and anywhere else it
    // would cross an inter-unit link on every new line -- which the link-latency sweep would then report as
    // synchronization cost.
    Sample* samples = nullptr;
};

// --check: a functional test of barrier semantics, run separately from the timing runs (its atomics add coherence
// traffic). Every client bumps its group's counter before arriving; once released it must see every participant's
// arrival for that round. zsim executes the program natively, so a core let through early would read a short count.
bool g_check = false;
volatile uint64_t* g_arrivals = nullptr;  // one per group
const uint32_t* g_group_size = nullptr;
volatile uint64_t g_violations = 0;

volatile uint64_t g_registered = 0;
volatile uint64_t g_clients_done = 0;
uint64_t g_total_clients = 0;
uint64_t g_total_threads = 0;

// Wait without spinning and without blocking. Blocking would hand this thread's core to the next thread that joins
// (zsim gives a core to whoever is runnable), and spinning burns simulated instructions -- the thread-creation gate
// of the earlier harness cost 3.9 M of them on core 0, more than the measurement itself. Parking costs one magic op
// per simulator phase and keeps the core.
void park_until(volatile uint64_t* counter, uint64_t target) {
    volatile uint64_t token = 0;
    while (*counter < target) req_sync(&token, SYNCRON_OP_PARK, 0);
}

void client_work(Task* t) {
    const Layout& L = *t->L;
    for (uint32_t s = 0; s < t->rounds; s++) {
        t->samples[s].arrive = zsim_now();
        if (g_check) __sync_fetch_and_add(&g_arrivals[t->group], 1);
        switch (L.scheme) {
            case SCH_SYNCRON:
            case SCH_IDEAL:
                if (L.across) syncron_barrier_wait_across_units(*t->var);
                else syncron_barrier_wait_within_unit(*t->var);
                break;
            default:
                sw_barrier_wait(t->server_core, t->group);
                break;
        }
        t->samples[s].release = zsim_now();
        if (g_check && g_arrivals[t->group] < (uint64_t)g_group_size[t->group] * (s + 1))
            __sync_fetch_and_add(&g_violations, 1);
    }
    if (L.scheme == SCH_CENTRAL || L.scheme == SCH_HIER) sw_client_quit(t->server_core);
    __sync_fetch_and_add(&g_clients_done, 1);
}

void idle_work() {
    // No role in this run. Stay on this core (so the layout the harness verified holds) without spending
    // instructions, until the clients are done.
    park_until(&g_clients_done, g_total_clients);
}

void* thread_main(void* arg) {
    Task* t = (Task*)arg;
    t->core = syncron_core_id();
    t->unit = t->core / t->L->cores_per_unit;
    __sync_fetch_and_add(&g_registered, 1);
    park_until(&g_registered, g_total_threads);  // everybody has a core before anybody starts
    t->core_after_gate = syncron_core_id();      // ... and still has the same one
    switch (t->role) {
        case R_CLIENT: client_work(t); break;
        case R_SERVER: sw_server_loop(&t->server); break;
        default: idle_work(); break;
    }
    return nullptr;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    Layout L;
    uint32_t rounds = 20, warmup = 2, stack_mb = 1024;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) exit(1); return argv[++i]; };
        if (!strcmp(k, "--scheme")) {
            const char* v = val();
            if (!strcmp(v, "syncron")) L.scheme = SCH_SYNCRON;
            else if (!strcmp(v, "ideal")) L.scheme = SCH_IDEAL;
            else if (!strcmp(v, "central")) L.scheme = SCH_CENTRAL;
            else if (!strcmp(v, "hier")) L.scheme = SCH_HIER;
            else { fprintf(stderr, "barrier: unknown scheme %s\n", v); return 1; }
        }
        else if (!strcmp(k, "--rounds")) rounds = atoi(val());
        else if (!strcmp(k, "--warmup")) warmup = atoi(val());
        else if (!strcmp(k, "--scope")) L.across = !strcmp(val(), "units");
        else if (!strcmp(k, "--units")) L.units = atoi(val());
        else if (!strcmp(k, "--units-used")) L.units_used = atoi(val());
        else if (!strcmp(k, "--cores-per-unit")) L.cores_per_unit = atoi(val());
        else if (!strcmp(k, "--clients-per-unit")) L.clients_per_unit = atoi(val());
        else if (!strcmp(k, "--groups")) L.groups = atoi(val());
        else if (!strcmp(k, "--stack-mb")) stack_mb = atoi(val());
        else if (!strcmp(k, "--check")) g_check = true;
        else {
            fprintf(stderr, "usage: barrier --scheme syncron|ideal|central|hier [--scope unit|units] "
                            "[--clients-per-unit 15] [--units-used 4] [--groups 1] [--rounds 20] [--warmup 2] "
                            "[--units 4] [--cores-per-unit 16] [--stack-mb 1024]\n");
            return 1;
        }
    }
    if (L.units_used > L.units) L.units_used = L.units;
    if (!L.clients_per_unit || L.clients_per_unit > L.cores_per_unit) {
        fprintf(stderr, "barrier: --clients-per-unit must be 1..%u\n", L.cores_per_unit);
        return 1;
    }
    if ((L.scheme == SCH_CENTRAL || L.scheme == SCH_HIER) && !L.reserved()) {
        fprintf(stderr, "barrier: %s needs a core per unit for its server, so --clients-per-unit must be below "
                        "--cores-per-unit\n", scheme_names[L.scheme]);
        return 1;
    }
    if (!L.across && L.groups > L.clients_per_unit) {
        fprintf(stderr, "barrier: --scope unit with %u groups needs at least that many clients per unit\n", L.groups);
        return 1;
    }
    uint32_t total_rounds = warmup + rounds;
    uint32_t ngroups = L.total_groups();
    g_total_clients = L.clients();

    NdpArena arena(L.units, (uint64_t)stack_mb * 1024 * 1024);
    if (!arena.ok()) { fprintf(stderr, "barrier: arena unavailable\n"); return 1; }

    // Who takes part in which barrier, worked out the same way by every role.
    std::vector<uint32_t> participants(ngroups, 0), group_units(ngroups, 0);
    std::vector<std::vector<uint32_t> > local_participants(L.units, std::vector<uint32_t>(ngroups, 0));
    for (uint32_t core = 0; core < L.cores(); core++) {
        uint32_t idx = 0;
        if (role_of(L, core, &idx) != R_CLIENT) continue;
        uint32_t u = core / L.cores_per_unit, g = group_of_client(L, u, idx);
        participants[g]++;
        local_participants[u][g]++;
    }
    for (uint32_t g = 0; g < ngroups; g++)
        for (uint32_t u = 0; u < L.units_used; u++)
            if (local_participants[u][g]) group_units[g]++;

    printf("barrier: scheme=%s scope=%s %u units x %u cores, %u clients/unit on %u units = %u clients, "
           "%u barrier groups, %u rounds (+%u warmup)\n", scheme_names[L.scheme], L.across ? "units" : "unit",
           L.units, L.cores_per_unit, L.clients_per_unit, L.units_used, L.clients(), ngroups, rounds, warmup);

    std::vector<uint64_t> check_counts(ngroups, 0);
    g_arrivals = check_counts.data();
    g_group_size = participants.data();

    zsim_roi_begin();

    // One synchronization variable per barrier group, in the memory of the unit that coordinates it (that is what
    // makes that unit's SE the variable's Master SE). Central and Hier coordinate in software and need none.
    std::vector<SyncVar> vars(ngroups);
    if (L.scheme == SCH_SYNCRON || L.scheme == SCH_IDEAL) {
        for (uint32_t g = 0; g < ngroups; g++) {
            if (!syncron_create_syncvar(arena, master_unit_of_group(L, g), participants[g], &vars[g])) return 1;
        }
    }

    // Server scratch: counts and waiting lists, in the server's own unit's memory.
    std::vector<uint32_t*> counts(L.units, nullptr), waitlists(L.units, nullptr), unit_masks(L.units, nullptr);
    std::vector<uint32_t> max_wait(L.units, 0);
    std::vector<uint32_t> server_core_of_unit(L.units_used, 0);
    for (uint32_t u = 0; u < L.units_used; u++) server_core_of_unit[u] = L.server_core_of_unit(u);
    if (L.scheme == SCH_CENTRAL || L.scheme == SCH_HIER) {
        for (uint32_t u = 0; u < L.units_used; u++) {
            if (L.scheme == SCH_CENTRAL && u) break;
            uint32_t mw = 0;
            for (uint32_t g = 0; g < ngroups; g++) {
                uint32_t n = (L.scheme == SCH_CENTRAL) ? participants[g] : local_participants[u][g];
                if (n > mw) mw = n;
            }
            max_wait[u] = mw;
            counts[u] = (uint32_t*)arena.alloc(u, ngroups * sizeof(uint32_t));
            waitlists[u] = (uint32_t*)arena.alloc(u, (uint64_t)ngroups * mw * sizeof(uint32_t));
            unit_masks[u] = (uint32_t*)arena.alloc(u, ngroups * sizeof(uint32_t));
            if (!counts[u] || !waitlists[u] || !unit_masks[u]) {
                fprintf(stderr, "barrier: no arena space for unit %u's server state\n", u);
                return 1;
            }
            memset(counts[u], 0, ngroups * sizeof(uint32_t));
            memset(unit_masks[u], 0, ngroups * sizeof(uint32_t));
            memset(waitlists[u], 0, (uint64_t)ngroups * mw * sizeof(uint32_t));
        }
    }

    std::vector<Task> tasks(L.cores());
    g_total_threads = L.cores();
    for (uint32_t core = 0; core < L.cores(); core++) {
        Task& t = tasks[core];
        uint32_t idx = 0;
        t.role = role_of(L, core, &idx);
        t.expect_core = core;
        t.idx_in_unit = idx;
        t.rounds = total_rounds;
        t.L = &L;
        uint32_t u = core / L.cores_per_unit;
        if (t.role == R_CLIENT) {
            t.samples = (Sample*)arena.alloc(u, (uint64_t)total_rounds * sizeof(Sample));
            if (!t.samples) { fprintf(stderr, "barrier: no arena space for core %u's samples\n", core); return 1; }
            t.group = group_of_client(L, u, idx);
            t.var = &vars[t.group];
            t.server_core = (L.scheme == SCH_CENTRAL) ? L.global_server_core() : L.server_core_of_unit(u);
        } else if (t.role == R_SERVER) {
            SwServer& s = t.server;
            s.my_core = core;
            s.my_unit = u;
            s.cores_per_unit = L.cores_per_unit;
            s.units_used = L.units_used;
            s.groups = ngroups;
            s.hier = (L.scheme == SCH_HIER);
            s.is_global = (core == L.global_server_core());
            s.across = L.across;
            s.global_core = L.global_server_core();
            s.server_core_of_unit = server_core_of_unit.data();
            s.group_participants = participants.data();
            s.group_local_participants = local_participants[u].data();
            s.group_units = group_units.data();
            s.max_wait = max_wait[u];
            s.count = counts[u];
            s.waiters = waitlists[u];
            s.units_in = unit_masks[u];
            s.clients_here = 0;
            for (uint32_t g = 0; g < ngroups; g++)
                s.clients_here += (L.scheme == SCH_CENTRAL) ? participants[g] : local_participants[u][g];
        }
    }

    // Thread 0 is this thread and lands on core 0; the rest are created one at a time so core ids follow creation
    // order. Every thread checks the core it actually got, and the run fails loudly if the layout is not what the
    // roles above assumed.
    for (uint32_t core = 1; core < L.cores(); core++) {
        if (pthread_create(&tasks[core].th, nullptr, thread_main, &tasks[core]) != 0) {
            perror("pthread_create");
            return 1;
        }
        park_until(&g_registered, core);  // thread `core` has a core id before the next one is created
    }
    thread_main(&tasks[0]);
    for (uint32_t core = 1; core < L.cores(); core++) pthread_join(tasks[core].th, nullptr);
    zsim_roi_end();

    // Every thread must have got the core its role was computed for, and must still have been on it after the
    // start gate -- a thread that changed cores would have been talking to the wrong SE or server.
    bool layout_ok = true;
    for (uint32_t core = 0; core < L.cores(); core++) {
        if (tasks[core].core != tasks[core].expect_core) layout_ok = false;
        if (tasks[core].core_after_gate != tasks[core].expect_core) layout_ok = false;
    }
    printf("BARRIER layout cores=");
    for (uint32_t core = 0; core < L.cores() && core < 8; core++) {
        printf("%s%u:u%u:%c", core ? "," : "", tasks[core].core, tasks[core].unit,
               tasks[core].role == R_CLIENT ? 'c' : (tasks[core].role == R_SERVER ? 's' : '-'));
    }
    printf("%s expected=0..%u %s\n", L.cores() > 8 ? ",..." : "", L.cores() - 1, layout_ok ? "OK" : "MISMATCH");

    std::vector<double> waits, periods;
    for (uint32_t core = 0; core < L.cores(); core++) {
        Task& t = tasks[core];
        if (t.role != R_CLIENT) continue;
        std::vector<double> w;
        for (uint32_t s = warmup; s < total_rounds; s++)
            w.push_back((double)(t.samples[s].release - t.samples[s].arrive));
        std::sort(w.begin(), w.end());
        waits.push_back(w.empty() ? 0.0 : w[w.size() / 2]);
        if (rounds > 1)
            periods.push_back((double)(t.samples[total_rounds - 1].arrive - t.samples[warmup].arrive) / (rounds - 1));
    }
    if (waits.empty() || periods.empty()) { fprintf(stderr, "barrier: no client samples\n"); return 1; }
    printf("BARRIER kind=%s-%s scheme=%s scope=%s participants=%u clients_per_unit=%u units_used=%u groups=%u "
           "per_group=%u rounds=%u\n", scheme_names[L.scheme], L.across ? "across" : "within",
           scheme_names[L.scheme], L.across ? "units" : "unit", L.clients(), L.clients_per_unit, L.units_used,
           ngroups, participants[0], rounds);
    printf("BARRIER period_cycles=%.1f period_min=%.1f period_max=%.1f\n", median(periods),
           *std::min_element(periods.begin(), periods.end()), *std::max_element(periods.begin(), periods.end()));
    printf("BARRIER wait_last_cycles=%.1f wait_first_cycles=%.1f wait_med_cycles=%.1f\n",
           *std::min_element(waits.begin(), waits.end()), *std::max_element(waits.begin(), waits.end()),
           median(waits));
    if (g_check) {
        printf("CHECK arrivals-before-release violations=%lu over %u rounds x %u clients\n", (uint64_t)g_violations,
               total_rounds, L.clients());
        if (g_violations) layout_ok = false;
    }
    printf("VERIFY %s\n", layout_ok ? "PASS" : "FAIL core layout, or a client released before every arrival");
    printf("BARRIER done\n");
    return layout_ok ? 0 : 2;
}
