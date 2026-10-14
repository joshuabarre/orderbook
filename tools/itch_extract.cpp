// Convert a Nasdaq TotalView-ITCH 5.0 feed into a replay file for the
// order book benchmark.
//
//   gzip -dc 01302019.NASDAQ_ITCH50.gz | ./build/itch_extract out.bin AAPL MSFT ...
//
// ITCH reports every displayed order on Nasdaq by order reference number:
// adds (A, F), executions (E, C), partial cancels (X), deletes (D) and
// replaces (U). For the chosen symbols this tool turns them into the three
// operations an order book handles, in feed order:
//
//   A/F -> add               D -> cancel
//   E/C/X -> reduce to the remaining size, or cancel if nothing remains
//   U -> cancel the old order, add the new one (a replace loses priority)
//
// Prices are converted from ITCH's 1/10000 dollar to cents, the tick size for
// stocks above $1. Executions are reported as reductions because the feed
// already says which resting order traded; the incoming marketable orders that
// caused them are not in the feed.
//
// Output: header, symbol table (name, min and max price seen), then ops.

#include <arpa/inet.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../bench/replay_format.hpp"

namespace {

std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
std::uint64_t be64(const unsigned char* p) {
    return static_cast<std::uint64_t>(be32(p)) << 32 | be32(p + 4);
}
std::uint64_t be48(const unsigned char* p) {
    return static_cast<std::uint64_t>(be16(p)) << 32 | be32(p + 2);
}

struct Live {
    std::uint16_t sym;
    std::uint32_t qty;
    std::uint8_t side;   // 0 buy, 1 sell
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gzip -dc feed.gz | %s out.bin SYMBOL...\n", argv[0]);
        return 2;
    }
    std::vector<std::string> wanted(argv + 2, argv + argc);
    std::vector<replay::Symbol> syms(wanted.size());
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        std::memset(syms[i].name, ' ', 8);
        std::memcpy(syms[i].name, wanted[i].data(), wanted[i].size() < 8 ? wanted[i].size() : 8);
        syms[i].min_price = INT64_MAX;
        syms[i].max_price = INT64_MIN;
    }

    std::unordered_map<std::uint16_t, std::uint16_t> locate_to_sym;   // ITCH locate -> our index
    std::unordered_map<std::uint64_t, Live> live;
    live.reserve(1 << 20);
    std::vector<replay::Op> ops;
    ops.reserve(1 << 24);

    std::uint64_t msgs = 0, sub_penny = 0, executions = 0, unknown_ref = 0;
    std::uint64_t first_ts = 0, last_ts = 0;
    unsigned char buf[65536];

    auto add = [&](std::uint16_t s, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t px4) {
        if (px4 % 100 != 0) { ++sub_penny; return; }   // not representable in cents; skip
        const std::int64_t px = px4 / 100;
        replay::Symbol& sy = syms[s];
        if (px < sy.min_price) sy.min_price = px;
        if (px > sy.max_price) sy.max_price = px;
        const auto sd = static_cast<std::uint8_t>(side == 'B' ? 0 : 1);
        live[ref] = Live{s, shares, sd};
        ops.push_back(replay::Op{ref, px, shares, s, replay::kAdd, sd});
    };
    // An order loses `n` shares (executed or cancelled).
    auto shrink = [&](std::uint64_t ref, std::uint32_t n) {
        auto it = live.find(ref);
        if (it == live.end()) { ++unknown_ref; return; }
        Live& o = it->second;
        o.qty = n >= o.qty ? 0 : o.qty - n;
        if (o.qty == 0) {
            ops.push_back(replay::Op{ref, 0, 0, o.sym, replay::kCancel, 0});
            live.erase(it);
        } else {
            ops.push_back(replay::Op{ref, 0, o.qty, o.sym, replay::kReduce, 0});
        }
    };

    for (;;) {
        unsigned char lenb[2];
        if (std::fread(lenb, 1, 2, stdin) != 2) break;
        const std::uint16_t len = be16(lenb);
        if (std::fread(buf, 1, len, stdin) != len) break;
        ++msgs;
        const char type = static_cast<char>(buf[0]);
        const std::uint16_t locate = be16(buf + 1);
        const unsigned char* m = buf + 11;   // past type, locate, tracking, timestamp

        if (type == 'R') {   // stock directory: learn which locate code each symbol has
            for (std::size_t i = 0; i < syms.size(); ++i)
                if (std::memcmp(m, syms[i].name, 8) == 0)
                    locate_to_sym[locate] = static_cast<std::uint16_t>(i);
            continue;
        }
        auto hit = locate_to_sym.find(locate);
        if (hit == locate_to_sym.end()) continue;
        const std::uint16_t s = hit->second;
        const std::uint64_t ts = be48(buf + 5);   // nanoseconds since midnight

        bool used = true;
        switch (type) {
            case 'A':
            case 'F': add(s, be64(m), static_cast<char>(m[8]), be32(m + 9), be32(m + 21)); break;
            case 'E':
            case 'C': ++executions; shrink(be64(m), be32(m + 8)); break;
            case 'X': shrink(be64(m), be32(m + 8)); break;
            case 'D': shrink(be64(m), UINT32_MAX); break;
            case 'U': {
                const std::uint64_t old_ref = be64(m), new_ref = be64(m + 8);
                auto it = live.find(old_ref);
                if (it == live.end()) { ++unknown_ref; break; }
                // The replacement keeps the original side; ITCH does not repeat it.
                const std::uint8_t side_flag = it->second.side;
                shrink(old_ref, UINT32_MAX);
                add(s, new_ref, side_flag == 0 ? 'B' : 'S', be32(m + 16), be32(m + 20));
                break;
            }
            default: used = false;
        }
        if (used) {
            if (first_ts == 0) first_ts = ts;
            last_ts = ts;
        }
    }

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) { std::perror(argv[1]); return 1; }
    replay::Header h{replay::kMagic, static_cast<std::uint32_t>(syms.size()), ops.size()};
    std::fwrite(&h, sizeof h, 1, out);
    std::fwrite(syms.data(), sizeof(replay::Symbol), syms.size(), out);
    std::fwrite(ops.data(), sizeof(replay::Op), ops.size(), out);
    std::fclose(out);

    std::printf("ITCH messages read: %llu\n", static_cast<unsigned long long>(msgs));
    std::printf("ops written: %zu (session %.2f h to %.2f h after midnight)\n", ops.size(),
                static_cast<double>(first_ts) / 3.6e12, static_cast<double>(last_ts) / 3.6e12);
    std::printf("executions: %llu   sub-penny adds skipped: %llu   unknown order refs: %llu\n",
                static_cast<unsigned long long>(executions), static_cast<unsigned long long>(sub_penny),
                static_cast<unsigned long long>(unknown_ref));
    for (const auto& s : syms)
        std::printf("  %.8s  price range %lld..%lld cents\n", s.name,
                    static_cast<long long>(s.min_price), static_cast<long long>(s.max_price));
    return 0;
}
