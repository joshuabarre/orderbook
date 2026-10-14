// Throughput and latency of OrderBook vs. the std::map baseline.
//
//   ./build/bench                      synthetic flow only
//   ./build/bench data/replay.bin      synthetic + replay of real Nasdaq ITCH data
//
// Throughput is the median of 5 untimed runs per book, each on a fresh book.
// A separate run puts a timestamp around every operation for the latency
// distribution (the timer itself adds roughly one tick to each sample).
// On Apple Silicon the timer ticks at 24 MHz (41.7 ns), so per-op percentiles
// below that are reported as "<42 ns"; the mean comes from throughput and is
// exact.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../src/map_book.hpp"
#include "../src/order_book.hpp"
#include "replay_format.hpp"
#include "workload.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Sink {   // consumes fills so the compiler cannot drop the matching work
    std::uint64_t fills = 0, volume = 0, checksum = 0;
    void operator()(const Fill& f) {
        ++fills;
        volume += f.qty;
        checksum = checksum * 31 + f.maker + static_cast<std::uint64_t>(f.price) * 7 + f.qty;
    }
};

double tick_ns() {
    // Smallest nonzero step of the clock, measured.
    double best = 1e9;
    for (int i = 0; i < 1000; ++i) {
        auto a = Clock::now(), b = a;
        while (b == a) b = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count());
    }
    return best;
}

struct Stats {
    double mops = 0, mean_ns = 0;
    double p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    Sink sink;
    std::size_t resting = 0;
};

void percentiles(std::vector<std::uint32_t>& ns, Stats& s) {
    std::sort(ns.begin(), ns.end());
    auto at = [&](double q) { return static_cast<double>(ns[static_cast<std::size_t>(q * static_cast<double>(ns.size() - 1))]); };
    s.p50 = at(0.50);
    s.p99 = at(0.99);
    s.p999 = at(0.999);
    s.p9999 = at(0.9999);
    s.max = static_cast<double>(ns.back());
}

// Throughput is the median of kReps untimed runs, each on a fresh book.
constexpr int kReps = 5;

void throughput(std::vector<double> secs, std::size_t nops, Stats& s) {
    std::sort(secs.begin(), secs.end());
    const double sec = secs[secs.size() / 2];
    s.mops = static_cast<double>(nops) / sec / 1e6;
    s.mean_ns = sec * 1e9 / static_cast<double>(nops);
}

template <class Book>
void run_synth_op(Book& b, const bench::Op& op, Sink& sink) {
    switch (op.kind) {
        case bench::Op::Add: b.add(op.id, op.side, op.price, op.qty, op.tif, sink); break;
        case bench::Op::Market: b.market(op.id, op.side, op.qty, sink); break;
        case bench::Op::Cancel: b.cancel(op.id); break;
        case bench::Op::Reduce: b.reduce(op.id, op.qty); break;
    }
}

// Capacity for the synthetic run. Sized to the live-order count the flow
// actually reaches (tens of thousands), not the number of operations: a pool
// and id table sized for 10M orders is 800 MB that every lookup misses in.
constexpr std::size_t kSynthCapacity = 1 << 18;

template <class Book>
Stats synth(const std::vector<bench::Op>& ops, Price lo, Price hi) {
    Stats s;
    std::vector<double> secs;
    for (int rep = 0; rep < kReps; ++rep) {
        auto b = std::make_unique<Book>(lo, hi, kSynthCapacity);
        Sink sink;
        auto t0 = Clock::now();
        for (const auto& op : ops) run_synth_op(*b, op, sink);
        secs.push_back(std::chrono::duration<double>(Clock::now() - t0).count());
        s.sink = sink;
        s.resting = b->order_count();
    }
    throughput(secs, ops.size(), s);
    {
        auto b = std::make_unique<Book>(lo, hi, kSynthCapacity);
        Sink scratch;
        std::vector<std::uint32_t> ns(ops.size());
        for (std::size_t i = 0; i < ops.size(); ++i) {
            auto t0 = Clock::now();
            run_synth_op(*b, ops[i], scratch);
            ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        }
        percentiles(ns, s);
    }
    return s;
}

// ---- real-data replay -------------------------------------------------

struct Replay {
    std::vector<replay::Symbol> syms;
    std::vector<replay::Op> ops;
};

bool load(const char* path, Replay& r) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    replay::Header h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == replay::kMagic;
    if (ok) {
        r.syms.resize(h.nsyms);
        r.ops.resize(h.nops);
        ok = std::fread(r.syms.data(), sizeof(replay::Symbol), h.nsyms, f) == h.nsyms &&
             std::fread(r.ops.data(), sizeof(replay::Op), h.nops, f) == h.nops;
    }
    std::fclose(f);
    return ok;
}

struct ReplayCheck {
    std::uint64_t failed_cancels = 0, failed_reduces = 0, crossed_adds = 0;
};

template <class Book>
void run_replay_op(std::vector<std::unique_ptr<Book>>& books, const replay::Op& op, Sink& sink,
                   ReplayCheck& chk) {
    Book& b = *books[op.sym];
    switch (op.kind) {
        case replay::kAdd: {
            AddResult r = b.add(op.id, op.side ? Side::Sell : Side::Buy, op.price, op.qty, Tif::GTC, sink);
            if (r.filled) ++chk.crossed_adds;
            break;
        }
        case replay::kCancel: if (!b.cancel(op.id)) ++chk.failed_cancels; break;
        case replay::kReduce: if (!b.reduce(op.id, op.qty)) ++chk.failed_reduces; break;
    }
}

// Peak number of simultaneously live orders per symbol, to size each pool.
std::vector<std::size_t> peak_live(const Replay& r) {
    std::vector<std::size_t> live(r.syms.size(), 0), peak(r.syms.size(), 0);
    for (const auto& op : r.ops) {
        if (op.kind == replay::kAdd) peak[op.sym] = std::max(peak[op.sym], ++live[op.sym]);
        else if (op.kind == replay::kCancel) --live[op.sym];
    }
    return peak;
}

template <class Book>
std::vector<std::unique_ptr<Book>> make_books(const Replay& r) {
    const auto peak = peak_live(r);
    std::vector<std::unique_ptr<Book>> books;
    for (std::size_t i = 0; i < r.syms.size(); ++i)
        books.push_back(std::make_unique<Book>(0, r.syms[i].max_price, peak[i] + 1));
    return books;
}

template <class Book>
Stats replay_run(const Replay& r, ReplayCheck& chk) {
    Stats s;
    std::vector<double> secs;
    for (int rep = 0; rep < kReps; ++rep) {
        auto books = make_books<Book>(r);
        Sink sink;
        ReplayCheck c;
        auto t0 = Clock::now();
        for (const auto& op : r.ops) run_replay_op(books, op, sink, c);
        secs.push_back(std::chrono::duration<double>(Clock::now() - t0).count());
        s.sink = sink;
        chk = c;
        s.resting = 0;
        for (auto& b : books) s.resting += b->order_count();
    }
    throughput(secs, r.ops.size(), s);
    {
        auto books = make_books<Book>(r);
        Sink scratch;
        ReplayCheck ignore;
        std::vector<std::uint32_t> ns(r.ops.size());
        for (std::size_t i = 0; i < r.ops.size(); ++i) {
            auto t0 = Clock::now();
            run_replay_op(books, r.ops[i], scratch, ignore);
            ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        }
        percentiles(ns, s);
    }
    return s;
}

// ---- output ------------------------------------------------------------

std::string fmt_ns(double v, double tick) {
    char buf[32];
    if (v < tick) std::snprintf(buf, sizeof buf, "<%.0f", tick);
    else std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

void row(const char* name, const Stats& s, double tick) {
    std::printf("  %-10s %8.1f M/s %8.1f ns   p50 %5s  p99 %5s  p99.9 %6s  p99.99 %6s  max %8.0f\n",
                name, s.mops, s.mean_ns, fmt_ns(s.p50, tick).c_str(), fmt_ns(s.p99, tick).c_str(),
                fmt_ns(s.p999, tick).c_str(), fmt_ns(s.p9999, tick).c_str(), s.max);
}

}  // namespace

int main(int argc, char** argv) {
    const double tick = tick_ns();
    std::printf("timer resolution: %.1f ns\n", tick);
    std::printf("columns: throughput, mean ns/op, then per-op latency percentiles in ns\n\n");

    {
        const std::size_t n = 10'000'000;
        const Price lo = 10'000, hi = 30'000;
        auto ops = bench::synthetic(n, lo, hi, 42);
        std::printf("synthetic: %zu ops (50%% add, 33%% cancel, 12%% reduce, 5%% market)\n", n);
        Stats fast = synth<OrderBook>(ops, lo, hi);
        Stats map = synth<MapBook>(ops, lo, hi);
        row("OrderBook", fast, tick);
        row("MapBook", map, tick);
        std::printf("  speedup %.1fx   fills %llu (match: %s)   resting at end %zu\n\n",
                    fast.mops / map.mops, static_cast<unsigned long long>(fast.sink.fills),
                    fast.sink.checksum == map.sink.checksum && fast.sink.fills == map.sink.fills ? "yes" : "NO",
                    fast.resting);
    }

    if (argc > 1) {
        Replay r;
        if (!load(argv[1], r)) {
            std::fprintf(stderr, "cannot read replay file %s\n", argv[1]);
            return 1;
        }
        std::printf("replay: %zu ops across", r.ops.size());
        for (const auto& s : r.syms) std::printf(" %.*s", static_cast<int>(strcspn(s.name, " ")), s.name);
        std::printf(" (Nasdaq ITCH 5.0, 2019-01-30)\n");
        ReplayCheck cf, cm;
        Stats fast = replay_run<OrderBook>(r, cf);
        Stats map = replay_run<MapBook>(r, cm);
        row("OrderBook", fast, tick);
        row("MapBook", map, tick);
        std::printf("  speedup %.1fx   resting at end %zu\n", fast.mops / map.mops, fast.resting);
        std::printf("  feed consistency: %llu crossed adds, %llu failed cancels, %llu failed reduces\n",
                    static_cast<unsigned long long>(cf.crossed_adds),
                    static_cast<unsigned long long>(cf.failed_cancels),
                    static_cast<unsigned long long>(cf.failed_reduces));
        if (cf.crossed_adds != cm.crossed_adds || cf.failed_cancels != cm.failed_cancels)
            std::printf("  WARNING: books disagree on the replay\n");
    }
    return 0;
}
