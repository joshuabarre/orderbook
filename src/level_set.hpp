#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace ob {

// A set of integers in [0, n), stored as a hierarchical bitmap, answering
// "next member >= i" and "previous member <= i" in O(log64 n) word operations.
//
// Layer 0 has one bit per integer. Each higher layer has one bit per 64-bit
// word of the layer below, set iff that word is non-zero, until a layer fits
// in a single word. For n = 20 million that is 5 layers, so finding the next
// occupied price across a gap of any size costs at most a handful of word
// reads, where a flat bitmap would scan up to 2.5 MB.
//
// Layers are allocated with calloc, so the zero pages of a huge, mostly empty
// range are never touched and cost no physical memory.
class LevelSet {
public:
    explicit LevelSet(std::size_t n) {
        std::size_t bits = n;
        do {
            const std::size_t words = (bits + 63) / 64;
            auto* p = static_cast<std::uint64_t*>(std::calloc(words, sizeof(std::uint64_t)));
            if (!p) throw std::bad_alloc();
            layers_.push_back(Layer{Words(p), words});
            bits = words;
        } while (bits > 1);
    }

    void insert(std::size_t i) {
        for (Layer& l : layers_) {
            std::uint64_t& w = l.words[i >> 6];
            const bool was_empty = (w == 0);
            w |= std::uint64_t{1} << (i & 63);
            if (!was_empty) return;   // the layers above already know this word is occupied
            i >>= 6;
        }
    }

    void erase(std::size_t i) {
        for (Layer& l : layers_) {
            std::uint64_t& w = l.words[i >> 6];
            w &= ~(std::uint64_t{1} << (i & 63));
            if (w != 0) return;       // the word is still occupied; layers above unchanged
            i >>= 6;
        }
    }

    // Smallest member >= i, or -1.
    std::int64_t next_at_or_above(std::size_t i) const { return up(0, i); }

    // Largest member <= i, or -1. Pass i < 0 as "none below".
    std::int64_t prev_at_or_below(std::int64_t i) const { return i < 0 ? -1 : down(0, static_cast<std::size_t>(i)); }

private:
    struct Free {
        void operator()(std::uint64_t* p) const { std::free(p); }
    };
    using Words = std::unique_ptr<std::uint64_t[], Free>;
    struct Layer {
        Words words;
        std::size_t nwords;
    };

    // Search layer `k` for the first set bit >= i. If i's word has nothing at
    // or above i, ask the layer above for the next non-empty word, then take
    // that word's lowest bit.
    std::int64_t up(std::size_t k, std::size_t i) const {
        const Layer& l = layers_[k];
        std::size_t w = i >> 6;
        if (w >= l.nwords) return -1;
        std::uint64_t bits = l.words[w] & (~std::uint64_t{0} << (i & 63));
        if (bits) return static_cast<std::int64_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(bits)));
        if (k + 1 == layers_.size()) return -1;
        const std::int64_t nw = up(k + 1, w + 1);
        if (nw < 0) return -1;
        bits = l.words[static_cast<std::size_t>(nw)];
        return nw * 64 + std::countr_zero(bits);
    }

    std::int64_t down(std::size_t k, std::size_t i) const {
        const Layer& l = layers_[k];
        const std::size_t w = i >> 6;
        std::uint64_t bits = l.words[w] & (~std::uint64_t{0} >> (63 - (i & 63)));
        if (bits) return static_cast<std::int64_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits)));
        if (k + 1 == layers_.size() || w == 0) return -1;
        const std::int64_t pw = down(k + 1, w - 1);
        if (pw < 0) return -1;
        bits = l.words[static_cast<std::size_t>(pw)];
        return pw * 64 + 63 - std::countl_zero(bits);
    }

    std::vector<Layer> layers_;
};

}  // namespace ob
