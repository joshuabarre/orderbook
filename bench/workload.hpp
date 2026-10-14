#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "../src/types.hpp"

namespace bench {

// One pre-generated operation, so generating flow is never inside the timer.
struct Op {
    enum Kind : std::uint8_t { Add, Market, Cancel, Reduce } kind;
    ob::Side side;
    ob::Tif tif;
    ob::Qty qty;
    ob::OrderId id;
    ob::Price price;
};

// Synthetic flow around a random-walking mid price:
//   50% limit adds, mostly passive at a geometric distance from the mid,
//       about 15% priced through the touch so they trade on arrival
//   33% cancels of a recently added order (which may already have filled)
//   12% reductions
//    5% market orders
// Cancel-heavy because real order flow is: most orders never trade.
inline std::vector<Op> synthetic(std::size_t n, ob::Price lo, ob::Price hi, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::geometric_distribution<int> depth(0.25);
    std::uniform_int_distribution<ob::Qty> size(1, 200);

    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<ob::OrderId> recent;
    recent.reserve(1 << 16);
    ob::OrderId next_id = 1;
    ob::Price mid = (lo + hi) / 2;

    for (std::size_t i = 0; i < n; ++i) {
        if (pct(rng) < 5) mid += pct(rng) < 50 ? -1 : 1;
        if (mid < lo + 100) mid = lo + 100;
        if (mid > hi - 100) mid = hi - 100;

        Op op{};
        const int k = pct(rng);
        if (k < 50 || recent.empty()) {
            op.kind = Op::Add;
            op.side = pct(rng) < 50 ? ob::Side::Buy : ob::Side::Sell;
            op.tif = ob::Tif::GTC;
            op.qty = size(rng);
            op.id = next_id++;
            const ob::Price off = 1 + depth(rng);
            const bool aggressive = pct(rng) < 15;
            const ob::Price sign = (op.side == ob::Side::Buy) ? -1 : 1;
            op.price = mid + (aggressive ? -sign * off : sign * off);
            recent.push_back(op.id);
        } else if (k < 83) {
            op.kind = Op::Cancel;
            std::uniform_int_distribution<std::size_t> pick(0, recent.size() - 1);
            const std::size_t j = pick(rng);
            op.id = recent[j];
            recent[j] = recent.back();
            recent.pop_back();
        } else if (k < 95) {
            op.kind = Op::Reduce;
            std::uniform_int_distribution<std::size_t> pick(0, recent.size() - 1);
            op.id = recent[pick(rng)];
            op.qty = size(rng) / 4;
        } else {
            op.kind = Op::Market;
            op.side = pct(rng) < 50 ? ob::Side::Buy : ob::Side::Sell;
            op.qty = size(rng);
            op.id = next_id++;
        }
        ops.push_back(op);
        if (recent.size() > 50000) recent.erase(recent.begin(), recent.begin() + 10000);
    }
    return ops;
}

}  // namespace bench
