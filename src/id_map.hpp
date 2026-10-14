#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace ob {

// Order id -> pool slot, as an open-addressing hash table with linear probing.
//
// Why not std::unordered_map: it allocates a node per entry and chases a
// pointer per lookup. Here every entry lives in one flat array, sized once,
// so a lookup is usually a single cache line and nothing ever allocates.
//
// Deletion uses backward-shift instead of tombstones: after removing an entry,
// later entries in the same probe run are moved back to fill the hole. This
// keeps probe sequences short forever, where tombstones would slowly degrade
// a table that sees millions of insert/erase cycles (which an order book does).
class IdMap {
public:
    static constexpr OrderId kEmpty = ~OrderId{0};   // reserved: not a valid id
    static constexpr std::uint32_t kMissing = ~std::uint32_t{0};

    explicit IdMap(std::size_t max_entries) {
        std::size_t cap = 16;
        while (cap < max_entries * 2) cap <<= 1;     // load factor <= 0.5
        slots_.assign(cap, Slot{kEmpty, 0});
        mask_ = cap - 1;
    }

    std::uint32_t find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.id == id) return s.value;
            if (s.id == kEmpty) return kMissing;
        }
    }

    // Caller guarantees `id` is absent and the table is below capacity.
    void insert(OrderId id, std::uint32_t value) {
        std::size_t i = home(id);
        while (slots_[i].id != kEmpty) i = (i + 1) & mask_;
        slots_[i] = Slot{id, value};
    }

    void erase(OrderId id) {
        std::size_t i = home(id);
        while (slots_[i].id != id) {
            if (slots_[i].id == kEmpty) return;
            i = (i + 1) & mask_;
        }
        // Backward-shift: walk the rest of the run and pull back any entry
        // whose home position means it may legally sit in the hole at `i`.
        for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (slots_[j].id == kEmpty) break;
            std::size_t h = home(slots_[j].id);
            // Entry j may move to i iff i lies cyclically in [h, j).
            bool movable = (i <= j) ? (h <= i || h > j) : (h <= i && h > j);
            if (movable) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].id = kEmpty;
    }

private:
    struct Slot {
        OrderId id;
        std::uint32_t value;
    };

    std::size_t home(OrderId id) const {
        // Fibonacci hashing: one multiply spreads sequential ids (which real
        // feeds produce) evenly across the table.
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) & mask_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
};

}  // namespace ob
