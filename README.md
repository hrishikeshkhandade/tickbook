# Tickbook

[![CI](https://github.com/hrishikeshkhandade/tickbook/actions/workflows/ci.yml/badge.svg)](https://github.com/hrishikeshkhandade/tickbook/actions/workflows/ci.yml)

**[Trade against it in your browser](https://portfolio.hrishikhandade.workers.dev/#tickbook)**: the same engine, compiled to WebAssembly.

A limit order book and matching engine in C++20. It takes buy and sell orders
for one instrument, matches them by price-time priority, and records every
trade.

Prices are whole numbers of ticks and quantities are whole units. There is no
floating point anywhere in the engine, so money never picks up rounding error.

## Build and test

Needs CMake 3.25 or newer, Ninja, and a C++20 compiler. The first configure
downloads GoogleTest 1.17.0 from GitHub, so it needs a network connection once.

```bash
cmake --workflow --preset release
```

That one command configures, builds and runs all 33 tests. Then run the
benchmark:

```bash
./build/release/tickbook_bench
```

To run the tests under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --workflow --preset asan
```

## What it does

| Order | Behaviour |
|---|---|
| Limit, GTC (default) | Trades against the other side while prices cross, then the remainder rests in the book. |
| Limit, IOC | Trades what it can right away. The remainder is discarded. |
| Limit, FOK | Trades the whole quantity right away or does nothing at all. |
| Market | Trades against whatever is resting, best price first. The remainder is discarded because it has no price to rest at. |
| Cancel | Removes a resting order by id. |

Matching rules:

- **Price priority.** The best price trades first: lowest ask, highest bid.
- **Time priority.** At the same price, the order that arrived first trades first.
- **Maker price.** A trade happens at the resting order's price. If you bid 105
  and the best ask is 100, you pay 100.
- A partly filled resting order keeps its place in the queue.

Other rules, each one a decision:

- An order with quantity 0 is rejected.
- An order whose id belongs to an order resting in the book right now is
  rejected. Once that order is filled or cancelled the id can be used again.
- Prices may be negative (some real futures have traded below zero).
- There is no self-trade prevention. The engine does not know who owns an order.

## Layout

```
include/tickbook/order_book.hpp   public API and data layout
src/order_book.cpp                the engine
tests/order_book_test.cpp         32 unit tests
tests/reference_book.hpp          slow, obviously correct order book
tests/fuzz_test.cpp               randomized test: engine vs reference
bench/bench.cpp                   benchmark
bench/results/                    raw benchmark output quoted below
wasm/tickbook_wasm.cpp            C interface for the WebAssembly build
wasm/smoke.mjs                    Node check of the built module
```

The engine is a plain library with no I/O, no threads and no clock. The tests
and the benchmark link the same library.

## Data structures and why

There are four, and each one answers one question.

### 1. Which price is best? `std::map<Price, Level>` per side

Each side of the book is a sorted map from price to a price level. The best
ask is the first entry and the best bid is the last.

`std::map` is a balanced binary tree. Adding or finding a price level costs
O(log P), where P is the number of distinct prices on that side. Its iterators
stay valid when other entries are added or removed, which lets every order
keep a direct handle to its level.

Alternatives:

- **Array indexed by tick.** O(1) for everything and very cache friendly. It
  needs a known, bounded price range up front. This is what I would try next
  if the map shows up in a profile.
- **Sorted `std::vector`.** Cache friendly to scan, but inserting a new price
  in the middle shifts everything after it: O(P).
- **Heap (priority queue).** Finds the best price fast but cannot remove an
  arbitrary level, which cancel needs.

Both sides use the same ascending map type, so the code needs no templates.
The cost: on Apple's standard library (libc++), stepping back from `end()` to
reach the best bid walks down the tree, so it is O(log P) rather than O(1).
Finding the best ask is O(1). A fix would be to cache the best bid or to store
bids in descending order.

### 2. Who is first at a price? A doubly linked list per level

Each level is a first-in-first-out queue. New orders join at the tail and
trades take from the head. The list is intrusive: each order stores the
position of the order before and after it, and the level stores head and tail.

Because an order knows both neighbours, removing it from the middle of the
queue is O(1). That matters because cancels are a large share of real order
flow.

Alternatives:

- **`std::deque` or `std::vector`.** Good for taking from the front, but
  cancelling from the middle is O(n) in the queue length.
- **`std::list`.** Same idea, but it makes one heap allocation per order and
  scatters orders across memory.

Each level also keeps a running total quantity and order count. That makes
depth queries O(1) once the level is found, and lets fill-or-kill decide by
looking at levels and not at every order.

### 3. Where is order 12345? `std::unordered_map<OrderId, slot>`

Cancel arrives with only an id. A hash map from id to the order's slot finds
it in O(1) on average. Without it, cancel would have to scan the book.

Alternatives: a plain array indexed by id is faster, but only works if ids are
small and dense. An open-addressing hash map would be more cache friendly than
`std::unordered_map`, which allocates a node per entry. I kept the standard
one for the first version because it is correct and well understood.

### 4. Where do orders live? A pool inside one `std::vector`

All resting orders live in one vector. When an order is removed, its slot goes
on a free list and the next new order reuses it. This avoids calling the
allocator for every order, and keeps orders close together in memory.

Orders refer to each other by 32-bit index into that vector, not by pointer.
An index stays correct when the vector grows and moves its contents. A pointer
would not. Indexes are also half the size of pointers.

## Cost of each operation

P is the number of price levels on one side. "Average" refers to the hash map,
whose worst case is O(n) if many ids collide.

| Operation | Cost | Why |
|---|---|---|
| Add limit order that rests | O(log P), average | Hash lookup for the duplicate check, tree lookup for the level, then O(1) to join the tail. |
| Match | O(1) average per fill, plus the best-price lookup per level | Each fill updates two orders and maybe removes one. Removing an emptied level by iterator is amortized O(1). |
| Market order | Same as match | It is a limit order with the most generous possible price. |
| Fill-or-kill check | O(levels within the limit price) | Sums level totals, never walks orders. |
| Cancel | O(1) average | Hash lookup, unlink from the list, free the slot. |
| Best bid, best ask | O(1) for ask, at most O(log P) for bid | See the libc++ note above. |
| Depth at a price | O(log P) | One tree lookup. |

## How correctness is checked

**Unit tests (32).** One behaviour each: every order type, price priority on
both sides, time priority, partial fills on the taker and the maker, cancel
from the head, middle and tail of a queue, rejects, id reuse, negative prices.

**Randomized test against a reference.** `tests/reference_book.hpp` is a second
order book written to be obviously correct and slow. It keeps all orders in
one flat vector and finds the next order to trade by scanning all of them. It
shares no data structure with the engine.

The test generates random order streams from a seed and feeds each one to both
books. After every operation the result and the trades must be identical. The
full resting book is compared on each of the first 256 steps, then every 16th
step, then at the end. A failure prints the seed and step, so it can be
replayed.

One default run, measured: 300 streams, 1,200,000 operations, 556,489 trades,
83,809 successful cancels, 100,771 rejected orders. It takes about 1.2 seconds
in a release build.

**Does the randomized test catch bugs?** I broke the engine on purpose in five
ways, one at a time, and ran only the randomized test:

| Injected bug | Result |
|---|---|
| Trade at the taker's price, not the maker's | Test failed |
| Cancel does not update the queue tail | Test failed |
| New order goes to the front of the queue | Test failed |
| Fill-or-kill ignores its limit price | Test failed |
| Emptied level is not removed after matching | Engine looped forever, test hung |

The last one was a hang, not a clean failure. Tests now have a 120 second
timeout so a hang is reported as a failure.

**Sanitizers.** All 33 tests pass under AddressSanitizer and
UndefinedBehaviorSanitizer (`cmake --workflow --preset asan`).

**CI.** Every push runs the release and sanitizer workflows on Linux (GCC)
and macOS (Clang) with GitHub Actions.

## Benchmark

### Setup

- Machine: MacBook with Apple M5 Pro (15 cores), 24 GB memory, macOS 27.0.
- Compiler: Apple clang 21.0.0, CMake `Release` build (`-O3 -DNDEBUG`).
- Single thread. On macOS a thread cannot be pinned to a core, so the
  benchmark asks the scheduler for the fast cores by marking itself
  user-interactive.
- Date: 2026-11-14. Raw output is in `bench/results/`.

### Workload

A synthetic stream generated from seed 1 before timing starts. 20,000 resting
orders are placed first and not timed. Then 5,000,000 operations are timed:

| Operation | Count | Share |
|---|---|---|
| Limit order that rests (1 to 50 ticks from the middle) | 2,500,108 | 50.0% |
| Limit order that crosses (1 to 3 ticks through) | 199,443 | 4.0% |
| Market order | 149,820 | 3.0% |
| Cancel | 2,150,629 | 43.0% |

Quantities are 1 to 100. The stream produces 682,399 trades. 1,855,349 of the
cancels hit a live order; the rest target orders that were already filled.
The book grows during the run and ends with 324,724 resting orders on 98 price
levels. After every operation the benchmark counts the new trades and clears
the trade log.

### Throughput

Wall time for all 5,000,000 operations with one clock read at each end,
repeated 11 times on a fresh book.

| Run file | Best | Median | Worst |
|---|---|---|---|
| `run2` | 19.11 million ops/s | 17.26 million ops/s | 13.62 million ops/s |
| `run1` | 11.52 million ops/s | 10.35 million ops/s | 8.56 million ops/s |

Both are the same binary and the same stream, minutes apart. The difference
is the machine: it is a laptop with a browser and other apps running, and
during `run1` Chrome was using more than one full core. The honest summary is
**about 10 to 19 million operations per second on one core, depending on what
else the laptop is doing**, which is roughly 52 to 97 ns per operation on
average. I have not measured it on an idle, dedicated machine.

### Latency

Same stream, with a clock read before and after every operation. Nanoseconds.
From `run2`:

| Operation | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| All | 42 | 167 | 416 | 708 | 32,815,334 |
| Limit, rests | 42 | 42 | 166 | 292 | 32,815,334 |
| Limit, crosses | 42 | 84 | 291 | 625 | 17,958 |
| Market | 42 | 125 | 333 | 667 | 44,541 |
| Cancel | 42 | 250 | 500 | 792 | 96,959 |

In `run1`, on the busier machine: p50 42, p90 208, p99 417, p99.9 583, max
6,303,959.

How to read these:

- **The clock is coarse.** The benchmark measured the smallest step of
  `steady_clock` on this machine as 41 ns, and each read costs about 15 to 18
  ns. Every value is a multiple of about 41.7 ns. A p50 of 42 means "one clock
  tick or less", not exactly 42 ns. The throughput figure is the better guide
  to typical cost.
- **The worst case is memory growth, not matching.** The default run reserves
  memory for 40,000 orders and the book grows to over 320,000, so the order
  pool and the hash map have to grow and move their contents several times.
  The maximum always lands on "limit, rests", the only operation that adds an
  order. With memory reserved for 400,000 orders
  (`tickbook_bench 5000000 1 400000`) the maximum over two runs was 799,375 ns
  and 72,291 ns, against 4.7 to 32.8 million ns across the default runs I
  made. Those two runs are not saved in `bench/results/`.
- The remaining tail includes the operating system interrupting the thread,
  which this benchmark cannot separate from the engine.

## Limits

- One instrument, one thread. No locking; the caller must not share a book
  across threads.
- No order modify, stop orders, iceberg orders or self-trade prevention.
- No persistence and no recovery. The book lives in memory.
- The trade log grows until the caller clears it.
- Growing past the reserved size causes a pause of several milliseconds (see
  above). Pass the expected order count to the constructor to avoid it.
- At most about 4.29 billion resting orders at once (32-bit slot index).
- A level's total quantity is a 64-bit sum and is not checked for overflow.
- `std::unordered_map` has an O(n) worst case under heavy hash collisions.
- The benchmark stream is synthetic. It is not real market data, and real
  flow has different shapes (bursts, price moves, deeper cancels).
- Benchmarked on one laptop under normal desktop load, with a 41 ns clock.

## WebAssembly build

The same engine compiles to WebAssembly with Emscripten. `wasm/tickbook_wasm.cpp`
is a thin C interface around one `OrderBook`: nine plain functions that take
numbers, plus one buffer of 32-bit integers that JavaScript reads results
from. It adds no matching logic and the engine source is unchanged.

```bash
emcmake cmake --preset wasm
```

```bash
cmake --build --preset wasm
```

```bash
node wasm/smoke.mjs
```

That writes `build/wasm/tickbook.wasm` (23,345 bytes with Emscripten 7.0.0)
and checks a few trades through it in Node. The module needs no JavaScript
glue file. It asks its host for one function, a notice that its memory grew.

Things the interface does that the engine does not:

- It hands out order ids itself: 1, 2, 3, and so on.
- Results cross as 32-bit integers. A quantity or id above 4,294,967,295 is
  clamped to that value, and a price outside the signed 32-bit range is
  clamped too. The engine underneath still uses 64 bits.

The portfolio site runs this module as a playable order book.

### Speed in a browser

Not measured properly yet. The portfolio scene has a button that times
500,000 operations after 20,000 resting orders, with the same mix as the
native benchmark (50% resting limits, 4% crossing limits, 3% market, 43%
cancels) but a different random stream.

I ran it 55 times on the same laptop in Chromium 156. The results ranged from
36 ms to 144 ms for the same work, which is about 3.5 to 13.9 million
operations per second. The runs were made in a browser pane that was hidden
for at least some of them, and a hidden page runs as a background tab. So the
spread says more about the browser tab than about the engine, and I do not
quote a browser figure. Every run is in
`bench/results/2026-11-14-wasm-browser.txt`, with what a proper measurement
would need.

Whatever the figure turns out to be, it will not be directly comparable with
the native one: the stream is different and ten times shorter, and every
operation pays for a call from JavaScript into the module.

## Next

- Time the WebAssembly build in a visible browser window on a quiet machine.
- Try an array indexed by tick in place of the map, if the map shows up in a
  profile.

## License

MIT. See [LICENSE](LICENSE).
