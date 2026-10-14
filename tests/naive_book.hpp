#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "../src/types.hpp"

namespace ob {

// The reference implementation for differential testing: a flat vector of
// resting orders, matched by scanning for the best price and then the lowest
// arrival sequence. O(n) per operation and written to be obviously correct,
// not fast. Both real books must agree with it on every operation.
class NaiveBook {
public:
    NaiveBook(Price min_price, Price max_price, std::size_t /*max_orders*/)
        : min_(min_price), max_(max_price) {}

    template <class OnFill>
    AddResult add(OrderId id, Side side, Price price, Qty qty, Tif tif, OnFill&& on_fill) {
        if (qty == 0 || id == kReserved || price < min_ || price > max_ || find(id) >= 0)
            return {Status::Rejected, 0};
        Qty left = take(id, side, price, qty, on_fill);
        if (left == 0) return {Status::Filled, qty};
        if (tif == Tif::IOC) return {Status::Cancelled, qty - left};
        orders_.push_back(Rest{id, side, price, left, seq_++});
        return {Status::Rested, qty - left};
    }

    template <class OnFill>
    AddResult market(OrderId id, Side side, Qty qty, OnFill&& on_fill) {
        if (qty == 0 || id == kReserved || find(id) >= 0) return {Status::Rejected, 0};
        Price any = side == Side::Buy ? std::numeric_limits<Price>::max()
                                      : std::numeric_limits<Price>::min();
        Qty left = take(id, side, any, qty, on_fill);
        return {left == 0 ? Status::Filled : Status::Cancelled, qty - left};
    }

    bool cancel(OrderId id) {
        long i = find(id);
        if (i < 0) return false;
        orders_.erase(orders_.begin() + i);
        return true;
    }

    bool reduce(OrderId id, Qty new_qty) {
        long i = find(id);
        if (i < 0 || new_qty >= orders_[static_cast<std::size_t>(i)].qty) return false;
        if (new_qty == 0) orders_.erase(orders_.begin() + i);
        else orders_[static_cast<std::size_t>(i)].qty = new_qty;
        return true;
    }

    bool has_bid() const { return best(Side::Buy) >= 0; }
    bool has_ask() const { return best(Side::Sell) >= 0; }
    Price best_bid() const { return orders_[static_cast<std::size_t>(best(Side::Buy))].price; }
    Price best_ask() const { return orders_[static_cast<std::size_t>(best(Side::Sell))].price; }

    std::uint64_t depth(Side side, Price price) const {
        std::uint64_t total = 0;
        for (const Rest& o : orders_)
            if (o.side == side && o.price == price) total += o.qty;
        return total;
    }

    std::size_t order_count() const { return orders_.size(); }

private:
    static constexpr OrderId kReserved = std::numeric_limits<OrderId>::max();

    struct Rest {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    long find(OrderId id) const {
        for (std::size_t i = 0; i < orders_.size(); ++i)
            if (orders_[i].id == id) return static_cast<long>(i);
        return -1;
    }

    // Index of the order that trades next on `side`: best price, then oldest.
    long best(Side side) const {
        long b = -1;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const Rest& o = orders_[i];
            if (o.side != side) continue;
            if (b < 0) { b = static_cast<long>(i); continue; }
            const Rest& cur = orders_[static_cast<std::size_t>(b)];
            bool better = side == Side::Buy ? o.price > cur.price : o.price < cur.price;
            if (better || (o.price == cur.price && o.seq < cur.seq)) b = static_cast<long>(i);
        }
        return b;
    }

    template <class OnFill>
    Qty take(OrderId taker, Side side, Price limit, Qty qty, OnFill& on_fill) {
        const Side other = side == Side::Buy ? Side::Sell : Side::Buy;
        while (qty > 0) {
            long m = best(other);
            if (m < 0) break;
            Rest& maker = orders_[static_cast<std::size_t>(m)];
            bool crosses = side == Side::Buy ? maker.price <= limit : maker.price >= limit;
            if (!crosses) break;
            Qty n = maker.qty < qty ? maker.qty : qty;
            on_fill(Fill{maker.id, taker, maker.price, n});
            maker.qty -= n;
            qty -= n;
            if (maker.qty == 0) orders_.erase(orders_.begin() + m);
        }
        return qty;
    }

    Price min_, max_;
    std::vector<Rest> orders_;
    std::uint64_t seq_ = 0;
};

}  // namespace ob
