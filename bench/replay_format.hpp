#pragma once

#include <cstdint>

// On-disk format shared by tools/itch_extract (writer) and bench (reader).
namespace replay {

constexpr std::uint64_t kMagic = 0x3150455242444f4cull;   // "LODBREP1"

enum : std::uint8_t { kAdd = 0, kCancel = 1, kReduce = 2 };

struct Header {
    std::uint64_t magic;
    std::uint32_t nsyms;
    std::uint64_t nops;
};

struct Symbol {
    char name[8];             // space-padded, as in ITCH
    std::int64_t min_price;   // cents, over every add in the file
    std::int64_t max_price;
};

struct Op {
    std::uint64_t id;
    std::int64_t price;       // cents; add only
    std::uint32_t qty;        // add: size; reduce: new remaining size
    std::uint16_t sym;
    std::uint8_t kind;
    std::uint8_t side;        // add only: 0 buy, 1 sell
};

}  // namespace replay
