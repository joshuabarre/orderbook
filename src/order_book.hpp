#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "id_map.hpp"
#include "level_set.hpp"
#include "types.hpp"

namespace ob {

// A limit order book for one instrument with price-time priority matching.
//
// Layout, chosen so the hot paths never allocate and rarely miss cache:
//
//   * Price ladder. Each side has one Level per tick in [min_price,
//     max_price], so finding a price's level is an index, not a tree search.
//     Levels live in 4096-level chunks allocated the first time an order
//     rests in them. Real feeds need this: market makers park "stub quotes"
//     at $0.01 and $199,999.99, and a dense array over that range would be
//     hundreds of MB per instrument. Chunked, a far-off stub costs 64 KB.
//
//   * Occupancy set. A hierarchical bitmap (LevelSet) marks which levels
//     hold orders. When the best level empties, the next best is found in a
//     few word operations however large the gap, using count-trailing/
//     leading-zeros, instead of walking empty levels one at a time.
//
//   * Order pool. Every order is a fixed-size node in one preallocated array,
//     recycled through a free list. Orders at a level form an intrusive
//     doubly linked FIFO through pool indices, so cancel unlinks in O(1)
//     and the oldest order (time priority) is always the head.
//
//   * IdMap. Order id -> pool index, for O(1) cancel and reduce.
//
// Complexity: add O(1) + O(fills), cancel O(1), reduce O(1), best price O(1).
// When a best level empties, finding the next one is O(log64 range).
class OrderBook {
public:
    OrderBook(Price min_price, Price max_price, std::size_t max_orders)
        : min_(min_price),
          nlevels_(checked_levels(min_price, max_price)),
          bids_(nlevels_),
          asks_(nlevels_),
          pool_(max_orders),
          ids_(max_orders),
          best_bid_(kNoBid),
          best_ask_(static_cast<std::int64_t>(nlevels_)) {
        if (max_orders == 0 || max_orders >= kNil)
            throw std::invalid_argument("max_orders out of range");
        for (std::uint32_t i = 0; i < max_orders; ++i) pool_[i].next = i + 1;
        pool_.back().next = kNil;
        free_head_ = 0;
    }

    // Submit a limit order. Matches against the opposite side first; any
    // remainder rests (GTC) or is discarded (IOC). `on_fill(const Fill&)` is
    // called once per trade, in execution order.
    template <class OnFill>
    AddResult add(OrderId id, Side side, Price price, Qty qty, Tif tif, OnFill&& on_fill) {
        if (qty == 0 || id == IdMap::kEmpty || price < min_ ||
            price >= min_ + static_cast<Price>(nlevels_) ||
            ids_.find(id) != IdMap::kMissing)
            return {Status::Rejected, 0};

        const std::int64_t px = price - min_;
        Qty left = (side == Side::Buy) ? match_buy(id, px, qty, on_fill)
                                       : match_sell(id, px, qty, on_fill);
        const Qty filled = qty - left;
        if (left == 0) return {Status::Filled, filled};
        if (tif == Tif::IOC) return {Status::Cancelled, filled};
        if (free_head_ == kNil) return {Status::Rejected, filled};   // pool full

        rest(id, side, px, left);
        return {Status::Rested, filled};
    }

    // Market order: an IOC that will trade at any price.
    template <class OnFill>
    AddResult market(OrderId id, Side side, Qty qty, OnFill&& on_fill) {
        if (qty == 0 || id == IdMap::kEmpty || ids_.find(id) != IdMap::kMissing)
            return {Status::Rejected, 0};
        Qty left = (side == Side::Buy)
                       ? match_buy(id, static_cast<std::int64_t>(nlevels_) - 1, qty, on_fill)
                       : match_sell(id, 0, qty, on_fill);
        return {left == 0 ? Status::Filled : Status::Cancelled, qty - left};
    }

    bool cancel(OrderId id) {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kMissing) return false;
        remove(n);
        return true;
    }

    // Reduce a resting order's quantity in place, keeping its time priority.
    // Increasing quantity is refused: exchanges send an increase to the back
    // of the queue, which callers do explicitly with cancel + add.
    bool reduce(OrderId id, Qty new_qty) {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kMissing || new_qty >= pool_[n].qty) return false;
        if (new_qty == 0) {
            remove(n);
            return true;
        }
        Node& o = pool_[n];
        side_ladder(o.side).at(o.level).qty -= o.qty - new_qty;
        o.qty = new_qty;
        return true;
    }

    bool has_bid() const { return best_bid_ != kNoBid; }
    bool has_ask() const { return best_ask_ != static_cast<std::int64_t>(nlevels_); }
    Price best_bid() const { return min_ + best_bid_; }   // valid only if has_bid()
    Price best_ask() const { return min_ + best_ask_; }   // valid only if has_ask()

    // Total resting quantity at one price on one side.
    std::uint64_t depth(Side side, Price price) const {
        if (price < min_ || price >= min_ + static_cast<Price>(nlevels_)) return 0;
        const Ladder& l = side_ladder(side);
        const std::size_t i = static_cast<std::size_t>(price - min_);
        return l.chunks[i >> kChunkBits] ? l.at(i).qty : 0;
    }

    std::size_t order_count() const { return live_; }

private:
    static constexpr std::uint32_t kNil = ~std::uint32_t{0};
    static constexpr std::int64_t kNoBid = -1;

    struct Node {
        OrderId id = 0;
        Qty qty = 0;
        std::uint32_t level = 0;
        std::uint32_t prev = kNil;
        std::uint32_t next = kNil;
        Side side = Side::Buy;
    };

    struct Level {
        std::uint32_t head = kNil;   // oldest order: next to trade
        std::uint32_t tail = kNil;
        std::uint64_t qty = 0;
    };

    static constexpr unsigned kChunkBits = 12;   // 4096 levels = 64 KB per chunk

    struct Ladder {
        explicit Ladder(std::size_t n)
            : chunks((n >> kChunkBits) + 1), occupied(n) {}

        // Only for levels known to exist (they hold, or just held, an order).
        Level& at(std::size_t i) { return chunks[i >> kChunkBits][i & ((1u << kChunkBits) - 1)]; }
        const Level& at(std::size_t i) const { return chunks[i >> kChunkBits][i & ((1u << kChunkBits) - 1)]; }

        Level& at_or_create(std::size_t i) {
            auto& c = chunks[i >> kChunkBits];
            if (!c) c = std::make_unique<Level[]>(std::size_t{1} << kChunkBits);
            return c[i & ((1u << kChunkBits) - 1)];
        }

        std::vector<std::unique_ptr<Level[]>> chunks;
        LevelSet occupied;
    };

    static std::size_t checked_levels(Price lo, Price hi) {
        if (hi < lo || hi - lo >= (Price{1} << 31))
            throw std::invalid_argument("price range out of bounds");
        return static_cast<std::size_t>(hi - lo + 1);
    }

    Ladder& side_ladder(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const Ladder& side_ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    // ---- matching -------------------------------------------------------

    template <class OnFill>
    Qty match_buy(OrderId taker, std::int64_t limit, Qty qty, OnFill& on_fill) {
        while (qty > 0 && best_ask_ <= limit) {
            Level& lv = asks_.at(static_cast<std::size_t>(best_ask_));
            qty = take_level(lv, best_ask_, taker, qty, on_fill);
            if (lv.head == kNil) {
                asks_.occupied.erase(static_cast<std::size_t>(best_ask_));
                best_ask_ = next_ask(best_ask_ + 1);
            }
        }
        return qty;
    }

    template <class OnFill>
    Qty match_sell(OrderId taker, std::int64_t limit, Qty qty, OnFill& on_fill) {
        while (qty > 0 && best_bid_ != kNoBid && best_bid_ >= limit) {
            Level& lv = bids_.at(static_cast<std::size_t>(best_bid_));
            qty = take_level(lv, best_bid_, taker, qty, on_fill);
            if (lv.head == kNil) {
                bids_.occupied.erase(static_cast<std::size_t>(best_bid_));
                best_bid_ = bids_.occupied.prev_at_or_below(best_bid_ - 1);
            }
        }
        return qty;
    }

    // Trade against one level in FIFO order until it or the taker runs out.
    template <class OnFill>
    Qty take_level(Level& lv, std::int64_t px, OrderId taker, Qty qty, OnFill& on_fill) {
        while (qty > 0 && lv.head != kNil) {
            const std::uint32_t n = lv.head;
            Node& maker = pool_[n];
            const Qty q = maker.qty < qty ? maker.qty : qty;
            on_fill(Fill{maker.id, taker, min_ + px, q});
            maker.qty -= q;
            lv.qty -= q;
            qty -= q;
            if (maker.qty == 0) {
                lv.head = maker.next;
                if (lv.head == kNil) lv.tail = kNil;
                else pool_[lv.head].prev = kNil;
                ids_.erase(maker.id);
                release(n);
            }
        }
        return qty;
    }

    // ---- resting and removal -------------------------------------------

    void rest(OrderId id, Side side, std::int64_t px, Qty qty) {
        const std::uint32_t n = free_head_;
        free_head_ = pool_[n].next;
        ++live_;

        Ladder& ladder = side_ladder(side);
        Level& lv = ladder.at_or_create(static_cast<std::size_t>(px));
        pool_[n] = Node{id, qty, static_cast<std::uint32_t>(px), lv.tail, kNil, side};
        if (lv.tail == kNil) lv.head = n;
        else pool_[lv.tail].next = n;
        lv.tail = n;
        lv.qty += qty;
        ids_.insert(id, n);

        ladder.occupied.insert(static_cast<std::size_t>(px));
        if (side == Side::Buy) {
            if (px > best_bid_) best_bid_ = px;
        } else {
            if (px < best_ask_) best_ask_ = px;
        }
    }

    void remove(std::uint32_t n) {
        Node& o = pool_[n];
        Ladder& ladder = side_ladder(o.side);
        Level& lv = ladder.at(o.level);
        if (o.prev == kNil) lv.head = o.next;
        else pool_[o.prev].next = o.next;
        if (o.next == kNil) lv.tail = o.prev;
        else pool_[o.next].prev = o.prev;
        lv.qty -= o.qty;

        if (lv.head == kNil) {
            const std::int64_t px = o.level;
            ladder.occupied.erase(o.level);
            if (o.side == Side::Buy) {
                if (px == best_bid_) best_bid_ = bids_.occupied.prev_at_or_below(px - 1);
            } else {
                if (px == best_ask_) best_ask_ = next_ask(px + 1);
            }
        }
        ids_.erase(o.id);
        release(n);
    }

    void release(std::uint32_t n) {
        pool_[n].next = free_head_;
        free_head_ = n;
        --live_;
    }

    // Lowest occupied ask level >= from, or nlevels_ (the "no ask" marker).
    std::int64_t next_ask(std::int64_t from) const {
        if (from >= static_cast<std::int64_t>(nlevels_)) return static_cast<std::int64_t>(nlevels_);
        const std::int64_t i = asks_.occupied.next_at_or_above(static_cast<std::size_t>(from));
        return i < 0 ? static_cast<std::int64_t>(nlevels_) : i;
    }

    Price min_;
    std::size_t nlevels_;
    Ladder bids_, asks_;
    std::vector<Node> pool_;
    IdMap ids_;
    std::uint32_t free_head_ = kNil;
    std::size_t live_ = 0;
    std::int64_t best_bid_;   // level index, kNoBid if the side is empty
    std::int64_t best_ask_;   // level index, nlevels_ if the side is empty
};

}  // namespace ob
