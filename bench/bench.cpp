// Tickbook benchmark.
//
// What it measures:
//   1. Throughput: wall time for a whole pre-generated stream of operations,
//      with one clock read before and one after. Repeated several times.
//   2. Latency: the same stream again, with a clock read before and after
//      every operation, reported as percentiles.
//
// What it does not measure: networking, parsing, threads, or anything
// outside the engine. The stream is generated before timing starts, so the
// random number generator is not in the measurement.
//
// Usage: tickbook_bench [timed_ops] [seed] [reserve]
// reserve is how many orders the book pre-sizes its memory for.
// Default 40000, which this stream outgrows on purpose (see README).
#include "tickbook/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#endif

using namespace tickbook;
using Clock = std::chrono::steady_clock;

namespace {

enum class Kind : std::uint8_t { Passive, Aggressive, Market, Cancel };
constexpr const char* kKindName[] = {"limit, rests", "limit, crosses",
                                     "market", "cancel"};

struct Op {
  OrderId id;
  Price price;
  std::uint32_t qty;
  Kind kind;
  Side side;
};

constexpr Price kMid = 100000;
constexpr std::size_t kPrefill = 20000;  // resting orders before timing starts

// Builds the stream. A shadow book is run while generating so that cancels
// can target ids that really rested (some will have been filled since; those
// cancels miss, as they do in real markets).
std::vector<Op> generate(std::size_t timed_ops, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  auto below = [&](std::uint64_t n) { return rng() % n; };

  std::vector<Op> ops;
  ops.reserve(kPrefill + timed_ops);
  OrderBook shadow(kPrefill * 2);
  std::vector<OrderId> rested;
  OrderId next_id = 1;

  auto passive = [&] {
    const Side side = below(2) ? Side::Buy : Side::Sell;
    // Distance from the middle, skewed so most orders sit near the top.
    const Price away = 1 + Price(below(50) * below(50) / 50);
    return Op{next_id++, side == Side::Buy ? kMid - away : kMid + away,
              std::uint32_t(1 + below(100)), Kind::Passive, side};
  };

  while (ops.size() < kPrefill + timed_ops) {
    Op op{};
    const std::uint64_t roll = ops.size() < kPrefill ? 0 : below(100);
    if (roll < 50) {
      op = passive();
    } else if (roll < 54) {
      const Side side = below(2) ? Side::Buy : Side::Sell;
      const Price through = 1 + Price(below(3));  // reaches 1 to 3 ticks in
      op = {next_id++, side == Side::Buy ? kMid + through : kMid - through,
            std::uint32_t(1 + below(100)), Kind::Aggressive, side};
    } else if (roll < 57) {
      op = {next_id++, 0, std::uint32_t(1 + below(100)), Kind::Market,
            below(2) ? Side::Buy : Side::Sell};
    } else {
      if (rested.empty()) continue;
      const std::size_t pick = below(rested.size());
      op = {rested[pick], 0, 0, Kind::Cancel, Side::Buy};
      rested[pick] = rested.back();
      rested.pop_back();
    }

    switch (op.kind) {
      case Kind::Passive:
      case Kind::Aggressive:
        if (shadow.add_limit(op.id, op.side, op.price, op.qty).resting > 0)
          rested.push_back(op.id);
        break;
      case Kind::Market:
        shadow.add_market(op.id, op.side, op.qty);
        break;
      case Kind::Cancel:
        shadow.cancel(op.id);
        break;
    }
    shadow.clear_trades();
    ops.push_back(op);
  }
  return ops;
}

struct Counters {
  std::uint64_t trades = 0, filled_qty = 0, cancel_hits = 0, cancels = 0;
};

// Applies one operation. The counters are also what stops the compiler from
// throwing the work away as unused.
inline void apply(OrderBook& book, const Op& op, Counters& c) {
  switch (op.kind) {
    case Kind::Passive:
    case Kind::Aggressive:
      c.filled_qty += book.add_limit(op.id, op.side, op.price, op.qty).filled;
      break;
    case Kind::Market:
      c.filled_qty += book.add_market(op.id, op.side, op.qty).filled;
      break;
    case Kind::Cancel:
      ++c.cancels;
      c.cancel_hits += book.cancel(op.id);
      break;
  }
  // A real consumer would read the trades here. We count and drop them.
  c.trades += book.trades().size();
  book.clear_trades();
}

std::int64_t ns_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

std::int64_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto i = static_cast<std::size_t>(p * double(sorted.size()));
  return sorted[std::min(i, sorted.size() - 1)];
}

void print_percentiles(const char* name, std::vector<std::uint32_t>& v) {
  std::sort(v.begin(), v.end());
  std::printf("  %-15s %9zu  %6lld %6lld %6lld %7lld %8lld\n", name, v.size(),
              (long long)percentile(v, 0.50), (long long)percentile(v, 0.90),
              (long long)percentile(v, 0.99), (long long)percentile(v, 0.999),
              (long long)(v.empty() ? 0 : v.back()));
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t timed_ops =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  const std::size_t reserve =
      argc > 3 ? std::strtoull(argv[3], nullptr, 10) : kPrefill * 2;
  constexpr int kRuns = 11;

#ifdef __APPLE__
  // macOS cannot pin a thread to a core. The closest thing is to mark the
  // thread as user-interactive, which makes the scheduler prefer the fast
  // performance cores over the efficiency cores.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

  std::printf("tickbook benchmark\n");
  std::printf("compiler: %s\n", __VERSION__);
#if defined(NDEBUG) && defined(__OPTIMIZE__)
  std::printf("build: optimized, assertions off\n");
#else
  std::printf("build: NOT OPTIMIZED. These numbers are not meaningful.\n");
#endif

  // --- how good is the clock? ---
  {
    constexpr int kReads = 1'000'000;
    std::int64_t smallest_step = INT64_MAX;
    const auto start = Clock::now();
    auto prev = start;
    for (int i = 0; i < kReads; ++i) {
      const auto now = Clock::now();
      const std::int64_t d = ns_between(prev, now);
      if (d > 0 && d < smallest_step) smallest_step = d;
      prev = now;
    }
    std::printf("clock: steady_clock, smallest step seen %lld ns, "
                "average cost of one read %.1f ns\n",
                (long long)smallest_step,
                double(ns_between(start, prev)) / kReads);
  }

  const std::vector<Op> ops = generate(timed_ops, seed);
  std::printf("stream: seed %llu, %zu resting orders prefilled (untimed), "
              "then %zu timed operations, memory reserved for %zu orders\n",
              (unsigned long long)seed, kPrefill, timed_ops, reserve);

  // --- throughput ---
  std::printf("\nthroughput (whole timed stream, one clock read each end)\n");
  std::vector<double> mops;
  Counters counters;
  std::size_t end_orders = 0, end_levels = 0;
  for (int run = 0; run < kRuns; ++run) {
    OrderBook book(reserve);
    Counters c;
    for (std::size_t i = 0; i < kPrefill; ++i) apply(book, ops[i], c);
    c = {};
    const auto t0 = Clock::now();
    for (std::size_t i = kPrefill; i < ops.size(); ++i) apply(book, ops[i], c);
    const auto t1 = Clock::now();
    const double secs = double(ns_between(t0, t1)) / 1e9;
    mops.push_back(double(timed_ops) / secs / 1e6);
    std::printf("  run %d: %.3f s, %.2f million ops/s, %.1f ns/op average\n",
                run + 1, secs, mops.back(), secs * 1e9 / double(timed_ops));
    counters = c;
    end_orders = book.order_count();
    end_levels = book.levels(Side::Buy).size() + book.levels(Side::Sell).size();
  }
  std::sort(mops.begin(), mops.end());
  std::printf("  %d runs: best %.2f, median %.2f, worst %.2f million ops/s\n",
              kRuns, mops.back(), mops[kRuns / 2], mops.front());
  std::printf("  the stream produced %llu trades; %llu of %llu cancels hit a "
              "live order\n",
              (unsigned long long)counters.trades,
              (unsigned long long)counters.cancel_hits,
              (unsigned long long)counters.cancels);
  std::printf("  book at the end: %zu resting orders on %zu price levels\n",
              end_orders, end_levels);

  // --- latency ---
  std::vector<std::uint32_t> lat(timed_ops);
  {
    OrderBook book(reserve);
    Counters c;
    for (std::size_t i = 0; i < kPrefill; ++i) apply(book, ops[i], c);
    for (std::size_t i = 0; i < timed_ops; ++i) {
      const auto t0 = Clock::now();
      apply(book, ops[kPrefill + i], c);
      const auto t1 = Clock::now();
      lat[i] = static_cast<std::uint32_t>(
          std::min<std::int64_t>(ns_between(t0, t1), UINT32_MAX));
    }
  }
  std::vector<std::uint32_t> by_kind[4];
  for (std::size_t i = 0; i < timed_ops; ++i)
    by_kind[static_cast<int>(ops[kPrefill + i].kind)].push_back(lat[i]);

  std::printf("\nlatency per operation in ns (one clock read before and "
              "after each; includes the cost of the reads)\n");
  std::printf("  %-15s %9s  %6s %6s %6s %7s %8s\n", "operation", "count",
              "p50", "p90", "p99", "p99.9", "max");
  print_percentiles("all", lat);
  for (int k = 0; k < 4; ++k) print_percentiles(kKindName[k], by_kind[k]);
  return 0;
}
