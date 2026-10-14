// Functional tests for OrderBook and MapBook.
//
// Part 1 checks each matching rule by hand on both books.
// Part 2 is differential: random order flow is fed to OrderBook, MapBook and
// the O(n) NaiveBook reference, and after every operation the three must
// agree on the result, every fill, both best prices, the order count, and
// the depth at a band of prices around the touch.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../src/level_set.hpp"
#include "../src/map_book.hpp"
#include "../src/order_book.hpp"
#include "naive_book.hpp"

using namespace ob;

static long g_checks = 0, g_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

struct Recorder {
    std::vector<Fill> fills;
    void operator()(const Fill& f) { fills.push_back(f); }
};

// ---------------------------------------------------------------- part 0

// LevelSet against std::set: random inserts and erases, then next/prev queries
// from random points. Sizes straddle the word and layer boundaries (64, 4096,
// 262144) and go up to a 20M-level range, which needs 5 layers.
void test_level_set(std::size_t n, unsigned seed, int nops) {
    std::mt19937_64 rng(seed);
    LevelSet ls(n);
    std::set<std::size_t> ref;
    // Cluster most members, as prices cluster near the touch, plus far outliers.
    auto pick = [&]() -> std::size_t {
        std::size_t c = n / 2;
        if (rng() % 10 == 0 || n < 512) return rng() % n;
        return c - 200 + rng() % 400;
    };
    for (int i = 0; i < nops; ++i) {
        const std::size_t x = pick();
        if (rng() % 2) { ls.insert(x); ref.insert(x); }
        else { ls.erase(x); ref.erase(x); }

        const std::size_t q = rng() % 3 ? pick() : rng() % n;
        auto up = ref.lower_bound(q);
        const std::int64_t want_up = up == ref.end() ? -1 : static_cast<std::int64_t>(*up);
        auto dn = ref.upper_bound(q);
        const std::int64_t want_dn = dn == ref.begin() ? -1 : static_cast<std::int64_t>(*std::prev(dn));
        CHECK(ls.next_at_or_above(q) == want_up);
        CHECK(ls.prev_at_or_below(static_cast<std::int64_t>(q)) == want_dn);
    }
    CHECK(ls.prev_at_or_below(-1) == -1);
}

// ---------------------------------------------------------------- part 1

template <class Book>
void test_rules(const char* name) {
    std::printf("  rules: %s\n", name);

    {   // A crossing order trades at the resting order's price, not its own.
        Book b(1, 1000, 64);
        Recorder r;
        CHECK(b.add(1, Side::Sell, 100, 10, Tif::GTC, r) == (AddResult{Status::Rested, 0}));
        CHECK(b.add(2, Side::Buy, 105, 4, Tif::GTC, r) == (AddResult{Status::Filled, 4}));
        CHECK(r.fills.size() == 1 && r.fills[0] == (Fill{1, 2, 100, 4}));
        CHECK(b.depth(Side::Sell, 100) == 6);
        CHECK(b.order_count() == 1);
    }
    {   // Price priority: the better price trades first even if it arrived later.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Sell, 102, 5, Tif::GTC, r);
        b.add(2, Side::Sell, 101, 5, Tif::GTC, r);
        b.add(3, Side::Buy, 102, 7, Tif::GTC, r);
        CHECK(r.fills.size() == 2);
        CHECK(r.fills[0] == (Fill{2, 3, 101, 5}));
        CHECK(r.fills[1] == (Fill{1, 3, 102, 2}));
    }
    {   // Time priority: at one price, the oldest order trades first.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Buy, 50, 3, Tif::GTC, r);
        b.add(2, Side::Buy, 50, 3, Tif::GTC, r);
        b.add(3, Side::Sell, 50, 4, Tif::GTC, r);
        CHECK(r.fills.size() == 2);
        CHECK(r.fills[0] == (Fill{1, 3, 50, 3}));
        CHECK(r.fills[1] == (Fill{2, 3, 50, 1}));
        CHECK(b.depth(Side::Buy, 50) == 2);
    }
    {   // An order that sweeps several levels rests its remainder at its limit.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Sell, 10, 1, Tif::GTC, r);
        b.add(2, Side::Sell, 11, 1, Tif::GTC, r);
        b.add(3, Side::Sell, 13, 1, Tif::GTC, r);
        CHECK(b.add(4, Side::Buy, 12, 5, Tif::GTC, r) == (AddResult{Status::Rested, 2}));
        CHECK(b.has_bid() && b.best_bid() == 12);
        CHECK(b.has_ask() && b.best_ask() == 13);
        CHECK(b.depth(Side::Buy, 12) == 3);
    }
    {   // IOC discards the remainder instead of resting it.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Sell, 10, 2, Tif::GTC, r);
        CHECK(b.add(2, Side::Buy, 10, 5, Tif::IOC, r) == (AddResult{Status::Cancelled, 2}));
        CHECK(!b.has_bid() && !b.has_ask() && b.order_count() == 0);
    }
    {   // Market orders ignore price; on an empty side they fill nothing.
        Book b(1, 1000, 64);
        Recorder r;
        CHECK(b.market(1, Side::Buy, 5, r) == (AddResult{Status::Cancelled, 0}));
        b.add(2, Side::Sell, 999, 3, Tif::GTC, r);
        CHECK(b.market(3, Side::Buy, 3, r) == (AddResult{Status::Filled, 3}));
        CHECK(r.fills.back() == (Fill{2, 3, 999, 3}));
    }
    {   // Cancel removes the order, empties the level, and moves the touch.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Buy, 40, 1, Tif::GTC, r);
        b.add(2, Side::Buy, 45, 1, Tif::GTC, r);
        CHECK(b.best_bid() == 45);
        CHECK(b.cancel(2));
        CHECK(!b.cancel(2));
        CHECK(b.best_bid() == 40);
        CHECK(b.cancel(1));
        CHECK(!b.has_bid());
    }
    {   // Reduce keeps time priority; an increase is refused.
        Book b(1, 1000, 64);
        Recorder r;
        b.add(1, Side::Sell, 20, 10, Tif::GTC, r);
        b.add(2, Side::Sell, 20, 10, Tif::GTC, r);
        CHECK(b.reduce(1, 4));
        CHECK(!b.reduce(1, 4));
        CHECK(!b.reduce(1, 50));
        CHECK(b.depth(Side::Sell, 20) == 14);
        b.add(3, Side::Buy, 20, 5, Tif::GTC, r);
        CHECK(r.fills[0] == (Fill{1, 3, 20, 4}));   // order 1 still first
        CHECK(r.fills[1] == (Fill{2, 3, 20, 1}));
        CHECK(b.reduce(2, 0));                       // reduce to zero = cancel
        CHECK(b.order_count() == 0);
    }
    {   // Rejections: duplicate live id, zero quantity, out-of-range price.
        Book b(100, 200, 64);
        Recorder r;
        CHECK(b.add(1, Side::Buy, 150, 1, Tif::GTC, r).status == Status::Rested);
        CHECK(b.add(1, Side::Buy, 151, 1, Tif::GTC, r).status == Status::Rejected);
        CHECK(b.add(2, Side::Buy, 150, 0, Tif::GTC, r).status == Status::Rejected);
        CHECK(b.add(3, Side::Buy, 99, 1, Tif::GTC, r).status == Status::Rejected);
        CHECK(b.add(4, Side::Sell, 201, 1, Tif::GTC, r).status == Status::Rejected);
        CHECK(b.add(5, Side::Sell, 200, 1, Tif::GTC, r).status == Status::Rested);
        CHECK(b.order_count() == 2);
        CHECK(b.cancel(1));
        CHECK(b.add(1, Side::Sell, 150, 1, Tif::GTC, r).status == Status::Rested);   // id reusable
    }
    {   // Stub quotes: a 20M-tick range (1 cent to $200,000 in cents), as in real
        // Nasdaq data. Levels far apart must still be found as the touch empties.
        Book b(1, 20'000'000, 64);
        Recorder r;
        b.add(1, Side::Buy, 1, 100, Tif::GTC, r);              // $0.01 stub bid
        b.add(2, Side::Sell, 19'999'999, 100, Tif::GTC, r);    // $199,999.99 stub ask
        b.add(3, Side::Buy, 16'500, 5, Tif::GTC, r);
        b.add(4, Side::Sell, 16'501, 5, Tif::GTC, r);
        CHECK(b.best_bid() == 16'500 && b.best_ask() == 16'501);
        b.cancel(3);
        b.cancel(4);
        CHECK(b.best_bid() == 1 && b.best_ask() == 19'999'999);
        CHECK(b.market(5, Side::Sell, 100, r) == (AddResult{Status::Filled, 100}));
        CHECK(r.fills.back() == (Fill{1, 5, 1, 100}));
        CHECK(!b.has_bid() && b.best_ask() == 19'999'999);
        CHECK(b.depth(Side::Buy, 12'345'678) == 0);           // never-touched chunk
    }
    {   // The touch survives emptying levels far apart (bitmap scan across words).
        Book b(0, 100000, 64);
        Recorder r;
        b.add(1, Side::Buy, 3, 1, Tif::GTC, r);
        b.add(2, Side::Buy, 99000, 1, Tif::GTC, r);
        b.add(3, Side::Sell, 99500, 1, Tif::GTC, r);
        b.add(4, Side::Sell, 100000, 1, Tif::GTC, r);
        CHECK(b.best_bid() == 99000 && b.best_ask() == 99500);
        b.cancel(2);
        b.cancel(3);
        CHECK(b.best_bid() == 3 && b.best_ask() == 100000);
        b.add(5, Side::Sell, 0, 2, Tif::GTC, r);   // sweeps the bid at 3, rests 1 at 0
        CHECK(!b.has_bid() && b.best_ask() == 0);
    }
}

// ---------------------------------------------------------------- part 2

struct Op {
    enum Kind { Add, Market, Cancel, Reduce } kind;
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    Tif tif;
};

template <class Book>
AddResult apply(Book& b, const Op& op, Recorder& r, bool& ok) {
    switch (op.kind) {
        case Op::Add: return b.add(op.id, op.side, op.price, op.qty, op.tif, r);
        case Op::Market: return b.market(op.id, op.side, op.qty, r);
        case Op::Cancel: ok = b.cancel(op.id); return {};
        case Op::Reduce: ok = b.reduce(op.id, op.qty); return {};
    }
    return {};
}

template <class A, class B>
bool same_state(const A& a, const B& b, Price lo, Price hi) {
    if (a.order_count() != b.order_count()) return false;
    if (a.has_bid() != b.has_bid() || a.has_ask() != b.has_ask()) return false;
    if (a.has_bid() && a.best_bid() != b.best_bid()) return false;
    if (a.has_ask() && a.best_ask() != b.best_ask()) return false;
    for (Price p = lo; p <= hi; ++p)
        for (Side s : {Side::Buy, Side::Sell})
            if (a.depth(s, p) != b.depth(s, p)) return false;
    return true;
}

// Random flow concentrated near a drifting mid, so orders constantly cross,
// queue, partially fill and empty levels. Ids are drawn from a small range
// on purpose, so duplicates, cancels of dead ids and id reuse all happen.
void differential(unsigned seed, int nops, Price lo, Price hi, bool stubs = false) {
    std::mt19937_64 rng(seed);
    OrderBook fast(lo, hi, 4096);
    MapBook map(lo, hi, 4096);
    NaiveBook ref(lo, hi, 4096);
    Price mid = (lo + hi) / 2;

    auto uni = [&](long a, long b) { return std::uniform_int_distribution<long>(a, b)(rng); };

    for (int i = 0; i < nops; ++i) {
        if (uni(0, 9) == 0) mid += uni(-2, 2);
        if (mid < lo + 10) mid = lo + 10;
        if (mid > hi - 10) mid = hi - 10;

        Op op{};
        long k = uni(0, 99);
        op.kind = k < 55 ? Op::Add : k < 60 ? Op::Market : k < 90 ? Op::Cancel : Op::Reduce;
        op.id = static_cast<OrderId>(uni(1, 600));
        op.side = uni(0, 1) ? Side::Buy : Side::Sell;
        op.price = mid + uni(-8, 8);
        if (uni(0, 199) == 0) op.price = uni(lo - 3, hi + 3);   // occasional out-of-range
        if (stubs && uni(0, 49) == 0) op.price = uni(0, 1) ? lo + uni(0, 2) : hi - uni(0, 2);
        op.qty = static_cast<Qty>(uni(0, 40));                   // includes zero
        op.tif = uni(0, 4) == 0 ? Tif::IOC : Tif::GTC;

        Recorder rf, rm, rr;
        bool okf = false, okm = false, okr = false;
        AddResult af = apply(fast, op, rf, okf);
        AddResult am = apply(map, op, rm, okm);
        AddResult ar = apply(ref, op, rr, okr);

        bool agree = af == ar && am == ar && okf == okr && okm == okr &&
                     rf.fills == rr.fills && rm.fills == rr.fills &&
                     same_state(fast, ref, mid - 12, mid + 12) &&
                     same_state(map, ref, mid - 12, mid + 12);
        CHECK(agree);
        if (!agree) {
            std::fprintf(stderr, "  diverged: seed %u op %d kind %d id %llu\n", seed, i,
                         op.kind, static_cast<unsigned long long>(op.id));
            return;
        }
    }
    // Full-depth comparison once at the end: the whole range when it is small,
    // otherwise the band the flow used plus both ends, where the stubs sit.
    if (hi - lo <= 100000) {
        CHECK(same_state(fast, ref, lo, hi));
        CHECK(same_state(map, ref, lo, hi));
    } else {
        for (auto [a, z] : {std::pair{lo, lo + 5}, std::pair{mid - 50, mid + 50}, std::pair{hi - 5, hi}}) {
            CHECK(same_state(fast, ref, a, z));
            CHECK(same_state(map, ref, a, z));
        }
    }
}

int main() {
    std::printf("part 0: LevelSet vs std::set\n");
    for (std::size_t n : {1ul, 2ul, 63ul, 64ul, 65ul, 4095ul, 4096ul, 4097ul, 262144ul, 262145ul, 20'000'000ul})
        test_level_set(n, static_cast<unsigned>(n), 20000);

    std::printf("part 1: matching rules\n");
    test_rules<OrderBook>("OrderBook");
    test_rules<MapBook>("MapBook");
    test_rules<NaiveBook>("NaiveBook (reference)");

    std::printf("part 2: differential, 3 books in lockstep\n");
    const int seeds = 40, ops = 25000;
    for (unsigned s = 1; s <= seeds; ++s) differential(s, ops, 1000, 1200);
    // Narrow range: the touch keeps hitting both ends of the ladder.
    for (unsigned s = 1; s <= 10; ++s) differential(1000 + s, ops, 0, 40);
    // Real-feed shape: a 20M-tick range with stub quotes parked at both ends.
    for (unsigned s = 1; s <= 10; ++s) differential(2000 + s, ops, 1, 20'000'000, true);
    std::printf("  %d random operations compared\n", (seeds + 20) * ops);

    std::printf("\n%ld checks, %ld failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
