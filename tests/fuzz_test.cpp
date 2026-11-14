// Randomized differential test: feed the same generated order stream to the
// real engine and to the slow reference book, and require identical results
// after every single operation.
//
// Streams are fully determined by their seed, so a failure prints the seed
// and step and can be replayed exactly.
//
// TICKBOOK_FUZZ_SEEDS and TICKBOOK_FUZZ_OPS (environment variables) change
// how many streams are run and how long each one is.
#include "reference_book.hpp"
#include "tickbook/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <vector>

using namespace tickbook;

namespace {

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 10) : fallback;
}

// Checks that must hold for any book, whatever the reference says.
void check_invariants(const OrderBook& book) {
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  // If the best bid reached the best ask they should have traded already.
  if (bid && ask) ASSERT_LT(*bid, *ask) << "book is crossed";

  std::size_t orders = 0;
  for (Side side : {Side::Buy, Side::Sell}) {
    const auto resting = book.resting(side);
    orders += resting.size();
    // Level totals must equal the sum of the orders in each level.
    std::size_t i = 0;
    for (const LevelView& level : book.levels(side)) {
      Qty sum = 0;
      std::size_t n = 0;
      while (i < resting.size() && resting[i].price == level.price) {
        ASSERT_GT(resting[i].qty, 0u);
        sum += resting[i].qty;
        ++n;
        ++i;
      }
      ASSERT_EQ(sum, level.qty);
      ASSERT_EQ(n, level.orders);
      ASSERT_GT(n, 0u) << "empty level left in the book";
    }
    ASSERT_EQ(i, resting.size());
  }
  ASSERT_EQ(orders, book.order_count());
}

struct StreamStats {
  std::uint64_t ops = 0, trades = 0, cancel_hits = 0, rejects = 0;
};

void run_stream(std::uint64_t seed, std::uint64_t ops, StreamStats& stats) {
  std::mt19937_64 rng(seed);
  auto below = [&](std::uint64_t n) { return rng() % n; };

  OrderBook book;
  reference::ReferenceBook ref;
  std::vector<OrderId> maybe_live;  // ids that rested at some point
  OrderId next_id = 1;
  // Each stream gets its own price band width. Narrow bands make deep queues
  // at few prices; wide bands make many thin levels.
  const std::uint64_t band = 2 + below(40);

  for (std::uint64_t step = 0; step < ops; ++step) {
    SCOPED_TRACE(::testing::Message() << "seed " << seed << " step " << step);
    const std::uint64_t roll = below(100);

    if (roll < 22) {  // cancel
      OrderId id;
      if (!maybe_live.empty() && below(10) < 8) {
        const std::size_t pick = below(maybe_live.size());
        id = maybe_live[pick];  // may already be filled; that is a valid test
        maybe_live[pick] = maybe_live.back();
        maybe_live.pop_back();
      } else {
        id = below(next_id + 5);  // any id, often dead or never used
      }
      const bool got = book.cancel(id);
      ASSERT_EQ(got, ref.cancel(id)) << "cancel " << id;
      stats.cancel_hits += got;
    } else {
      // Mostly fresh ids; sometimes reuse an old one, which must be rejected
      // if that order is still resting and accepted if it is gone.
      const OrderId id = below(50) == 0 ? 1 + below(next_id) : next_id++;
      const Side side = below(2) ? Side::Buy : Side::Sell;
      const Qty qty = below(40) == 0 ? 0 : 1 + below(100);  // sometimes zero
      AddResult got, want;
      if (roll < 32) {
        got = book.add_market(id, side, qty);
        want = ref.add_market(id, side, qty);
      } else {
        // Buys and sells draw from overlapping price ranges so that a good
        // share of orders cross.
        const Price price = (side == Side::Buy ? 1000 : 1000 + Price(band) / 3) +
                            Price(below(band)) - Price(band) / 2;
        const TimeInForce tif = roll < 42   ? TimeInForce::IOC
                                : roll < 50 ? TimeInForce::FOK
                                            : TimeInForce::GTC;
        got = book.add_limit(id, side, price, qty, tif);
        want = ref.add_limit(id, side, price, qty, tif);
      }
      ASSERT_EQ(got, want) << "add id " << id;
      if (got.reject == Reject::None) {
        ASSERT_EQ(got.filled + got.resting + got.cancelled, qty);
      } else {
        ++stats.rejects;
      }
      if (got.resting > 0) maybe_live.push_back(id);
    }

    // Same trades, in the same order, with the same prices and sizes.
    ASSERT_EQ(book.trades(), ref.trades());
    stats.trades += book.trades().size();
    book.clear_trades();
    ref.clear_trades();

    // Same book, order by order. Comparing the full book is slow, so do it
    // on every step early on (when most structural bugs show) and then
    // every 16th step, plus once at the end.
    if (step < 256 || step % 16 == 0 || step + 1 == ops) {
      ASSERT_EQ(book.resting(Side::Buy), ref.resting(Side::Buy));
      ASSERT_EQ(book.resting(Side::Sell), ref.resting(Side::Sell));
      check_invariants(book);
    }
    if (::testing::Test::HasFatalFailure()) return;
  }
  stats.ops += ops;
}

TEST(Fuzz, EngineMatchesReferenceOnRandomStreams) {
  const std::uint64_t seeds = env_or("TICKBOOK_FUZZ_SEEDS", 300);
  const std::uint64_t ops = env_or("TICKBOOK_FUZZ_OPS", 4000);
  StreamStats stats;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    run_stream(seed, ops, stats);
    if (HasFatalFailure()) return;
  }
  // Guard against a generator that never exercises the interesting paths.
  EXPECT_GT(stats.trades, stats.ops / 10);
  EXPECT_GT(stats.cancel_hits, stats.ops / 50);
  EXPECT_GT(stats.rejects, 0u);
  std::printf("fuzz: %llu streams, %llu ops, %llu trades, %llu cancel hits, "
              "%llu rejects\n",
              static_cast<unsigned long long>(seeds),
              static_cast<unsigned long long>(stats.ops),
              static_cast<unsigned long long>(stats.trades),
              static_cast<unsigned long long>(stats.cancel_hits),
              static_cast<unsigned long long>(stats.rejects));
}

}  // namespace
