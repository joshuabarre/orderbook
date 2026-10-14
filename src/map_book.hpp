#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>

#include "types.hpp"

namespace ob {

// The textbook order book: a std::map of price -> FIFO list per side, plus a
// hash map from id to the order's position for cancels.
//
// Same interface and semantics as OrderBook. It exists as the baseline the
// benchmark measures against, and as a second implementation for the
// differential tests. Every order is a separate heap allocation and every
// price lookup is a red-black tree search, which is what OrderBook removes.
class MapBook {
public:
    MapBook(Price min_price, Price max_price, std::size_t /*max_orders*/)
        : min_(min_price), max_(max_price) {}

    template <class OnFill>
    AddResult add(OrderId id, Side side, Price price, Qty qty, Tif tif, OnFill&& on_fill) {
        if (qty == 0 || id == kReserved || price < min_ || price > max_ || index_.count(id))
            return {Status::Rejected, 0};
        Qty left = (side == Side::Buy) ? match(asks_, id, qty, on_fill, [&](Price p) { return p <= price; })
                                       : match(bids_, id, qty, on_fill, [&](Price p) { return p >= price; });
        if (left == 0) return {Status::Filled, qty};
        if (tif == Tif::IOC) return {Status::Cancelled, qty - left};

        auto& queue = (side == Side::Buy) ? bids_[price] : asks_[price];
        queue.push_back(Order{id, left});
        index_.emplace(id, Loc{side, price, std::prev(queue.end())});
        return {Status::Rested, qty - left};
    }

    template <class OnFill>
    AddResult market(OrderId id, Side side, Qty qty, OnFill&& on_fill) {
        if (qty == 0 || id == kReserved || index_.count(id)) return {Status::Rejected, 0};
        auto any = [](Price) { return true; };
        Qty left = (side == Side::Buy) ? match(asks_, id, qty, on_fill, any)
                                       : match(bids_, id, qty, on_fill, any);
        return {left == 0 ? Status::Filled : Status::Cancelled, qty - left};
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        erase(it);
        return true;
    }

    bool reduce(OrderId id, Qty new_qty) {
        auto it = index_.find(id);
        if (it == index_.end() || new_qty >= it->second.it->qty) return false;
        if (new_qty == 0) erase(it);
        else it->second.it->qty = new_qty;
        return true;
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    Price best_bid() const { return bids_.begin()->first; }
    Price best_ask() const { return asks_.begin()->first; }

    std::uint64_t depth(Side side, Price price) const {
        auto sum = [](const auto& book, Price p) {
            std::uint64_t total = 0;
            if (auto it = book.find(p); it != book.end())
                for (const Order& o : it->second) total += o.qty;
            return total;
        };
        return side == Side::Buy ? sum(bids_, price) : sum(asks_, price);
    }

    std::size_t order_count() const { return index_.size(); }

private:
    static constexpr OrderId kReserved = std::numeric_limits<OrderId>::max();

    struct Order {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<Order>;
    struct Loc {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <class Book, class OnFill, class Crosses>
    Qty match(Book& book, OrderId taker, Qty qty, OnFill& on_fill, Crosses crosses) {
        while (qty > 0 && !book.empty() && crosses(book.begin()->first)) {
            auto lvl = book.begin();
            Queue& q = lvl->second;
            while (qty > 0 && !q.empty()) {
                Order& maker = q.front();
                Qty n = maker.qty < qty ? maker.qty : qty;
                on_fill(Fill{maker.id, taker, lvl->first, n});
                maker.qty -= n;
                qty -= n;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
        return qty;
    }

    void erase(std::unordered_map<OrderId, Loc>::iterator it) {
        const Loc loc = it->second;
        index_.erase(it);
        if (loc.side == Side::Buy) drop(bids_, loc);
        else drop(asks_, loc);
    }

    template <class Book>
    static void drop(Book& book, const Loc& loc) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    }

    Price min_, max_;
    std::map<Price, Queue, std::greater<Price>> bids_;   // best (highest) first
    std::map<Price, Queue> asks_;                         // best (lowest) first
    std::unordered_map<OrderId, Loc> index_;
};

}  // namespace ob
