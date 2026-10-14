# orderbook — a limit order book and matching engine in C++20

A single-instrument **limit order book** with **price-time priority matching**:
the data structure at the centre of every exchange and every trading system
that tracks one. It supports limit (GTC and IOC) and market orders, cancels,
and in-place size reductions, and reports each trade as it happens.

It is tested against a deliberately naive reference implementation on 1.5
million random operations, and replayed against **a full trading day of real
Nasdaq order data** (11.5 million operations on AAPL, MSFT, AMZN, SPY and QQQ),
staying exactly in sync with the exchange's feed: zero crossed adds, zero
failed cancels, zero failed reductions.

```cpp
ob::OrderBook book(/*min_price=*/1, /*max_price=*/20'000'000, /*max_orders=*/1 << 20);
auto on_fill = [](const ob::Fill& f) { /* maker, taker, price, qty */ };

book.add(1, ob::Side::Sell, 16'501, 300, ob::Tif::GTC, on_fill);   // rests
book.add(2, ob::Side::Buy,  16'505, 100, ob::Tif::GTC, on_fill);   // trades 100 @ 16501
book.reduce(1, 150);       // keeps time priority
book.cancel(1);
book.market(3, ob::Side::Buy, 50, on_fill);
```

Prices are integer ticks (cents for US equities). Floating point never touches
a price.

## Build and run

```bash
make            # tests and benchmark
make test       # unit + differential tests
make san        # the tests under UndefinedBehaviorSanitizer
make leaks      # macOS leak checker
make bench      # synthetic benchmark

# Real-data benchmark: one trading day of Nasdaq TotalView-ITCH 5.0 (4.8 GB)
curl -o data/01302019.NASDAQ_ITCH50.gz \
  "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
make build/itch_extract
gzip -dc data/01302019.NASDAQ_ITCH50.gz | ./build/itch_extract data/replay.bin AAPL MSFT AMZN SPY QQQ
./build/bench data/replay.bin
```

Requires a C++20 compiler. Header-only, no dependencies.

## Matching rules

* An incoming order trades against the opposite side while prices cross:
  a buy at P against asks at or below P, a sell against bids at or above.
* **Price priority**: the best price trades first.
* **Time priority**: at one price, the oldest order trades first (FIFO).
* Every trade prints at the **resting (maker) order's price**.
* Whatever does not trade rests (GTC) or is discarded (IOC). A market order is
  an IOC with no price limit.
* `reduce` shrinks a resting order without losing its place in the queue. An
  increase is refused, because exchanges send an increase to the back of the
  queue; a caller does that explicitly with cancel and add.
* Rejected: quantity zero, a price outside the book's range, or an id that is
  already resting.

## How it works

There are two implementations with the same interface:

* **`MapBook`** (`src/map_book.hpp`) is the textbook design: a `std::map` from
  price to a `std::list` of orders per side, plus a `std::unordered_map` from
  id to the order's position. Correct and short, but every order is a heap
  allocation and every price lookup is a red-black tree search with a pointer
  chase per level.
* **`OrderBook`** (`src/order_book.hpp`) removes those costs. It is the one to
  read.

### The price ladder

Each side is indexed directly by price: level `i` holds orders at
`min_price + i`. Looking up a price is array indexing, not a tree search.

The obvious version is one dense array covering the whole price range. Real
data breaks it. In the Nasdaq feed every one of the five symbols has resting
orders at **$0.01 and $199,999.99**: market makers' "stub quotes", parked far
from the market to satisfy quoting obligations. In cents, that range is 20
million levels, so a dense ladder would need over 600 MB per symbol.

So levels live in **4096-level chunks**, allocated the first time an order
rests in one. The ladder holds a small table of chunk pointers (about 5,000 for
a 20M range). A stub quote costs one 64 KB chunk instead of the range between
it and the market.

### Finding the next best price: `LevelSet`

When the best level empties, the book needs the next occupied level, which may
be one tick away or (with stub quotes) twenty million. Walking empty levels one
by one is out of the question.

`src/level_set.hpp` is a **hierarchical bitmap**. Layer 0 has one bit per price
level. Layer 1 has one bit per 64-bit word of layer 0, set if that word has any
bit set; layer 2 summarizes layer 1 the same way, until a layer fits in one
word. For 20M levels that is 5 layers (312,500 words, then 4,883, 77, 2, 1).

"Next occupied level at or above i":

1. Mask off the bits below `i` in i's layer-0 word. If anything is left,
   `countr_zero` gives the answer.
2. Otherwise ask layer 1 for the next non-empty layer-0 word after this one
   (the same question, one layer up), and take that word's lowest bit.

Each step is one word read and one instruction (`countr_zero` / `countl_zero`
compile to single instructions), and the recursion is as deep as the number of
layers. So the cost is **O(log₆₄ range)** whatever the gap: at most a handful
of word reads to jump from the touch to a stub quote 20 million ticks away.

Insert and erase keep the layers consistent by touching a summary bit only
when a word changes between empty and non-empty, which is rare, so the common
case is a single word update.

The layers are allocated with `calloc`, so the pages of a huge, mostly empty
range are never written and use no physical memory.

### Orders: a pool and intrusive lists

Every order is a fixed-size node in **one preallocated array**, recycled
through a free list. Nothing allocates after construction.

Orders at a price form a doubly linked FIFO **through pool indices** (32-bit,
not pointers): the level stores head and tail, each node its prev and next.
The head is always the oldest order, which is time priority for free, and
cancel unlinks a node from the middle in O(1).

Each level also keeps its total quantity, so depth queries never walk a queue.

### Finding an order by id: `IdMap`

Cancel and reduce arrive by order id. `src/id_map.hpp` maps id to pool index
with **open addressing and linear probing** in one flat array, at most half
full. A lookup is usually one cache line, where `std::unordered_map` chases a
pointer to a separately allocated node.

Two details matter:

* **Fibonacci hashing** (one multiply by 2⁶⁴/φ, then index with bits from the
  upper half of the product). Real feeds
  issue sequential ids; this spreads them evenly instead of clustering them.
* **Backward-shift deletion** instead of tombstones. When an entry is removed,
  later entries in the same probe run that are allowed to move back into the
  hole are moved back. An order book does millions of insert/erase cycles a
  day, and tombstones would accumulate and lengthen every probe until the
  table was rebuilt. With backward shift, the table after any sequence of
  operations is as if only the live entries had ever been inserted.

### Complexity

| operation | OrderBook | MapBook |
|---|---|---|
| add (no trade) | O(1) | O(log levels) + allocation |
| each fill | O(1) | O(1) |
| cancel | O(1) | O(log levels): finds the price level in the map |
| reduce | O(1) | O(1) average |
| best bid / ask | O(1) | O(1) |
| best level empties | O(log₆₄ range) | O(log levels) |

## Testing

`tests/test_book.cpp`, run by `make test`: **1,940,330 checks, all passing**,
also clean under UndefinedBehaviorSanitizer and the macOS leak checker.

1. **LevelSet against `std::set`**: random inserts and erases with next/prev
   queries after each, at sizes chosen to straddle word and layer boundaries
   (1, 63, 64, 65, 4095, 4096, 4097, 262144, 262145) and at 20 million.
2. **Matching rules by hand**, on all three books: price priority, time
   priority, trade price, sweeping several levels, IOC, market orders,
   cancels that move the touch, reductions that keep priority, rejections,
   and a 20M-level book with stub quotes at both ends.
3. **Differential testing.** `tests/naive_book.hpp` is a reference book written
   to be obviously correct rather than fast: a flat vector, scanned for the
   best price and then the lowest arrival number on every operation. Random
   order flow (adds, IOCs, market orders, cancels, reductions, duplicate ids,
   zero sizes, out-of-range prices, id reuse) goes to `OrderBook`, `MapBook`
   and the reference in lockstep. After **every** operation all three must
   agree on the result, every fill in order, both best prices, the order
   count and the depth around the touch. 1.5 million operations over 60 seeds,
   on a normal range, a narrow range where the touch keeps hitting both ends,
   and a 20M-level range with stub quotes.

**The tests were checked against known bugs.** Six bugs were planted one at a
time (a stale best price after a cancel, an id left in the map after a fill,
LIFO instead of FIFO queues, depth not reduced on cancel, a LevelSet layer not
updated on erase, an off-by-one in the layer search). Each was caught, by 50 to
10,000 failing checks.

**Real-feed consistency.** The replay below is also a correctness test. Nasdaq
says exactly which resting order each execution, cancel and delete applies to.
Over the full day, the book never failed to find one, and no add ever crossed
the book (which would mean the book's state had diverged from Nasdaq's).

## Performance

Apple M4, macOS 27, Apple clang 21, `-O3 -march=native`, single thread.
Throughput is the median of 5 runs, each on a fresh book. Latency is a
separate run with a timestamp around every operation; on Apple Silicon the
clock ticks every 41 ns, so per-operation figures under that show as `<41`.

**Real data**: one full trading day (2019-01-30, 4:00 to 20:00) of Nasdaq
TotalView-ITCH 5.0 for AAPL, MSFT, AMZN, SPY and QQQ: 368 million feed messages
filtered to 11.5 million book operations, one book per symbol, replayed in
feed order.

| | throughput | mean | p50 | p99 | p99.9 | p99.99 |
|---|---|---|---|---|---|---|
| **OrderBook** | 40–43 M ops/s | 23–25 ns | 41 ns | 84–167 ns | 208–375 ns | 0.4–1.0 µs |
| MapBook | 8–14 M ops/s | 74–131 ns | 83 ns | 250–625 ns | 0.6–1.4 µs | 1–17 µs |

**About 3x faster** (3.2–5.6x across runs; the baseline varies more).

**Synthetic flow**: 10 million operations around a random-walking mid (50%
adds of which about 15% trade on arrival, 33% cancels, 12% reductions, 5%
market orders). This exercises matching far more than the real feed does.

| | throughput | mean | p99 | p99.9 |
|---|---|---|---|---|
| **OrderBook** | 56–59 M ops/s | 17–18 ns | 84–292 ns | 209–667 ns |
| MapBook | 17–20 M ops/s | 50–59 ns | 167–417 ns | 250–1,417 ns |

**About 2.9x faster**, with 4,855,216 fills identical between the two books.

Ranges are across three runs. The machine was a working laptop, not an
isolated benchmark box, and runs taken while it was under heavy load (a
large download, a video call) were up to 2.5x slower for both books; those
are not included.

### What made the difference

* **No allocation on the hot path.** The pool and chunked ladder allocate up
  front (chunks once per new price region); `MapBook` allocates a list node per
  order and a tree node per new level.
* **Indexing instead of searching.** A price lookup is an array index, and
  finding the next level is a few `countr_zero`s, not a tree walk.

## Scope and limitations

* **One instrument per book, one thread.** A real gateway runs one book per
  symbol, usually one symbol per core, so the book itself does not need locks.
* **The real-data replay exercises adds, cancels and reductions, not matching.**
  ITCH reports executions against resting orders but not the incoming
  marketable orders that caused them, so executions are replayed as
  reductions. Matching is covered by the synthetic flow and the tests.
* **No self-trade prevention, iceberg/hidden orders, or auctions.**
* **Sub-penny orders are skipped** by the extractor (28 out of millions on
  this day); prices are in cents.
* **Fixed capacity.** If the pool is full, a new resting order is rejected
  rather than growing the pool mid-session.

## Files

| file | |
|---|---|
| `src/order_book.hpp` | the order book |
| `src/level_set.hpp` | hierarchical bitmap for finding the next price level |
| `src/id_map.hpp` | open-addressing id → order table |
| `src/map_book.hpp` | `std::map` baseline |
| `src/types.hpp` | ids, prices, sides, fills, results |
| `tests/test_book.cpp` | unit, differential and LevelSet tests |
| `tests/naive_book.hpp` | O(n) reference implementation |
| `bench/bench.cpp` | throughput and latency benchmark |
| `bench/workload.hpp` | synthetic order-flow generator |
| `tools/itch_extract.cpp` | Nasdaq ITCH 5.0 → replay file |
