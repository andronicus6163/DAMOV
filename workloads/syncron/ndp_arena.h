// ndp_arena: hands each NDP unit memory that lives in its own stack.
//
// The memory model maps addresses "unit-major": the high bits of an address pick the stack (see HBMStack's
// `addressing = unit_major`), so unit u owns the address range [u * stack_bytes, (u+1) * stack_bytes). zsim uses
// virtual addresses as physical ones (procMask | vLineAddr, and procMask is 0 for a single process), so an
// allocation's *virtual* address decides which stack it lands on, and the app can place its own data by allocating
// one arena aligned to the whole modelled capacity and slicing it per unit.
//
//   NdpArena arena(4, 1ull << 30);           // 4 units, 1 GB of modelled memory per stack
//   void* p = arena.alloc(unit_of(core), bytes);
//
// The arena is mapped with MAP_NORESERVE and never touched outside the slices the app uses, so the untouched parts
// cost nothing on the host. A stack's slice is exhausted at stack_bytes; alloc() returns nullptr rather than
// silently placing data in the wrong unit, because "silently in the wrong stack" is exactly the error this exists
// to prevent.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#include <vector>

class NdpArena {
   public:
    NdpArena(uint32_t units, uint64_t stack_bytes) : units_(units), stack_bytes_(stack_bytes) {
        uint64_t span = (uint64_t)units * stack_bytes;
        // Reserve twice the span so the base can be rounded up to a multiple of it: unit u is then exactly the
        // block whose address divided by stack_bytes is congruent to u.
        raw_ = (uint8_t*)mmap(nullptr, 2 * span, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (raw_ == MAP_FAILED) {
            perror("NdpArena mmap");
            raw_ = nullptr;
            return;
        }
        uint64_t base = (uint64_t)raw_;
        base_ = (uint8_t*)(((base + span - 1) / span) * span);
        used_.assign(units, 0);
    }

    bool ok() const { return base_ != nullptr; }

    // `bytes` of memory inside unit `unit`'s stack, 64-byte aligned.
    void* alloc(uint32_t unit, uint64_t bytes) {
        if (!base_ || unit >= units_) return nullptr;
        bytes = (bytes + 63) & ~63ull;
        if (used_[unit] + bytes > stack_bytes_) return nullptr;  // would spill into the next unit's stack
        void* p = base_ + (uint64_t)unit * stack_bytes_ + used_[unit];
        used_[unit] += bytes;
        return p;
    }

    uint32_t unit_of(const void* p) const {
        return (uint32_t)((((uint64_t)p - (uint64_t)base_) / stack_bytes_) % units_);
    }

    void report() const {
        printf("arena: base %p, %u units x %.2f MB", (void*)base_, units_, stack_bytes_ / 1048576.0);
        for (uint32_t u = 0; u < units_; u++) printf(", u%u used %.2f MB", u, used_[u] / 1048576.0);
        printf("\n");
    }

   private:
    uint32_t units_;
    uint64_t stack_bytes_;
    uint8_t* raw_ = nullptr;
    uint8_t* base_ = nullptr;
    std::vector<uint64_t> used_;
};
