#include "tickbook/order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace tickbook;
using enum Side;

namespace {

using Resting = std::vector<RestingOrder>;

// ---------- empty book ----------

TEST(Empty, HasNoBestPricesAndNoOrders) {
  OrderBook b;
  EXPECT_FALSE(b.best_bid());
  EXPECT_FALSE(b.best_ask());
  EXPECT_EQ(b.order_count(), 0u);
  EXPECT_TRUE(b.trades().empty());
  EXPECT_TRUE(b.levels(Buy).empty());
}

// ---------- limit orders ----------

TEST(Limit, RestsWhenNothingToTradeWith) {
  OrderBook b;
  AddResult r = b.add_limit(1, Buy, 100, 10);
  EXPECT_EQ(r, (AddResult{Reject::None, 0, 10, 0}));
  EXPECT_EQ(b.best_bid(), 100);
  EXPECT_FALSE(b.best_ask());
  EXPECT_EQ(b.order_count(), 1u);
  EXPECT_EQ(b.depth_at(Buy, 100), 10u);
}

TEST(Limit, DoesNotTradeWhenPricesDoNotCross) {
  OrderBook b;
  b.add_limit(1, Sell, 101, 10);
  AddResult r = b.add_limit(2, Buy, 100, 10);
  EXPECT_EQ(r.resting, 10u);
  EXPECT_TRUE(b.trades().empty());
  EXPECT_EQ(b.best_bid(), 100);
  EXPECT_EQ(b.best_ask(), 101);
}

TEST(Limit, FullFillTradesAtMakerPrice) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 10);
  // Buyer is willing to pay 105 but the resting seller asked 100.
  AddResult r = b.add_limit(2, Buy, 105, 10);
  EXPECT_EQ(r, (AddResult{Reject::None, 10, 0, 0}));
  ASSERT_EQ(b.trades().size(), 1u);
  EXPECT_EQ(b.trades()[0], (Trade{0, 1, 2, Buy, 100, 10}));
  EXPECT_EQ(b.order_count(), 0u);
  EXPECT_FALSE(b.best_ask());
  EXPECT_FALSE(b.best_bid());
}

TEST(Limit, TakerPartialFillRestsRemainder) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 4);
  AddResult r = b.add_limit(2, Buy, 100, 10);
  EXPECT_EQ(r, (AddResult{Reject::None, 4, 6, 0}));
  EXPECT_EQ(b.resting(Buy), (Resting{{2, 100, 6}}));
  EXPECT_TRUE(b.resting(Sell).empty());
}

TEST(Limit, MakerPartialFillKeepsRemainderAndPriority) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 10);
  b.add_limit(2, Sell, 100, 10);
  b.add_limit(3, Buy, 100, 4);
  EXPECT_EQ(b.resting(Sell), (Resting{{1, 100, 6}, {2, 100, 10}}));
  // Order 1 was partly filled but is still first in line.
  b.add_limit(4, Buy, 100, 3);
  ASSERT_EQ(b.trades().size(), 2u);
  EXPECT_EQ(b.trades()[1].maker_id, 1u);
  EXPECT_EQ(b.depth_at(Sell, 100), 13u);
}

TEST(Limit, BetterPriceTradesFirst) {
  OrderBook b;
  b.add_limit(1, Sell, 102, 5);
  b.add_limit(2, Sell, 100, 5);  // arrived later but cheaper
  b.add_limit(3, Sell, 101, 5);
  b.add_limit(4, Buy, 102, 12);
  ASSERT_EQ(b.trades().size(), 3u);
  EXPECT_EQ(b.trades()[0], (Trade{0, 2, 4, Buy, 100, 5}));
  EXPECT_EQ(b.trades()[1], (Trade{1, 3, 4, Buy, 101, 5}));
  EXPECT_EQ(b.trades()[2], (Trade{2, 1, 4, Buy, 102, 2}));
  EXPECT_EQ(b.resting(Sell), (Resting{{1, 102, 3}}));
}

TEST(Limit, BetterPriceTradesFirstOnBuySide) {
  OrderBook b;
  b.add_limit(1, Buy, 98, 5);
  b.add_limit(2, Buy, 100, 5);  // highest bid is best
  b.add_limit(3, Buy, 99, 5);
  b.add_limit(4, Sell, 98, 12);
  ASSERT_EQ(b.trades().size(), 3u);
  EXPECT_EQ(b.trades()[0], (Trade{0, 2, 4, Sell, 100, 5}));
  EXPECT_EQ(b.trades()[1], (Trade{1, 3, 4, Sell, 99, 5}));
  EXPECT_EQ(b.trades()[2], (Trade{2, 1, 4, Sell, 98, 2}));
}

TEST(Limit, SamePriceTradesInArrivalOrder) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 5);
  b.add_limit(2, Sell, 100, 5);
  b.add_limit(3, Sell, 100, 5);
  b.add_limit(4, Buy, 100, 12);
  ASSERT_EQ(b.trades().size(), 3u);
  EXPECT_EQ(b.trades()[0].maker_id, 1u);
  EXPECT_EQ(b.trades()[1].maker_id, 2u);
  EXPECT_EQ(b.trades()[2].maker_id, 3u);
  EXPECT_EQ(b.trades()[2].qty, 2u);
}

TEST(Limit, StopsAtItsLimitPriceAndRestsTheRest) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 5);
  b.add_limit(2, Sell, 103, 5);
  AddResult r = b.add_limit(3, Buy, 101, 8);
  EXPECT_EQ(r, (AddResult{Reject::None, 5, 3, 0}));
  EXPECT_EQ(b.best_bid(), 101);
  EXPECT_EQ(b.best_ask(), 103);
}

TEST(Limit, NegativePricesWork) {
  OrderBook b;
  b.add_limit(1, Sell, -5, 10);
  b.add_limit(2, Buy, -7, 10);
  EXPECT_TRUE(b.trades().empty());
  b.add_limit(3, Buy, -5, 4);
  ASSERT_EQ(b.trades().size(), 1u);
  EXPECT_EQ(b.trades()[0].price, -5);
}

// ---------- market orders ----------

TEST(Market, SweepsLevelsBestFirst) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 5);
  b.add_limit(2, Sell, 105, 5);
  AddResult r = b.add_market(3, Buy, 8);
  EXPECT_EQ(r, (AddResult{Reject::None, 8, 0, 0}));
  ASSERT_EQ(b.trades().size(), 2u);
  EXPECT_EQ(b.trades()[0], (Trade{0, 1, 3, Buy, 100, 5}));
  EXPECT_EQ(b.trades()[1], (Trade{1, 2, 3, Buy, 105, 3}));
  EXPECT_EQ(b.resting(Sell), (Resting{{2, 105, 2}}));
}

TEST(Market, RemainderIsDiscardedNotRested) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 5);
  AddResult r = b.add_market(2, Sell, 8);
  EXPECT_EQ(r, (AddResult{Reject::None, 5, 0, 3}));
  EXPECT_EQ(b.order_count(), 0u);
  EXPECT_FALSE(b.best_ask());
}

TEST(Market, OnEmptyBookDoesNothing) {
  OrderBook b;
  AddResult r = b.add_market(1, Buy, 8);
  EXPECT_EQ(r, (AddResult{Reject::None, 0, 0, 8}));
  EXPECT_TRUE(b.trades().empty());
  EXPECT_EQ(b.order_count(), 0u);
}

// ---------- cancel ----------

TEST(Cancel, RemovesRestingOrder) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 10);
  EXPECT_TRUE(b.cancel(1));
  EXPECT_EQ(b.order_count(), 0u);
  EXPECT_FALSE(b.best_bid());
  EXPECT_EQ(b.depth_at(Buy, 100), 0u);
}

TEST(Cancel, UnknownIdReturnsFalse) {
  OrderBook b;
  EXPECT_FALSE(b.cancel(42));
}

TEST(Cancel, SecondCancelReturnsFalse) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 10);
  EXPECT_TRUE(b.cancel(1));
  EXPECT_FALSE(b.cancel(1));
}

TEST(Cancel, FullyFilledOrderCannotBeCancelled) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 5);
  b.add_limit(2, Buy, 100, 5);
  EXPECT_FALSE(b.cancel(1));
  EXPECT_FALSE(b.cancel(2));
}

TEST(Cancel, PartlyFilledOrderCancelsItsRemainder) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 10);
  b.add_limit(2, Buy, 100, 4);
  EXPECT_TRUE(b.cancel(1));
  EXPECT_EQ(b.depth_at(Sell, 100), 0u);
  EXPECT_FALSE(b.best_ask());
}

TEST(Cancel, HeadMiddleAndTailKeepTheQueueInOrder) {
  for (OrderId victim : {1u, 2u, 3u}) {
    OrderBook b;
    b.add_limit(1, Sell, 100, 5);
    b.add_limit(2, Sell, 100, 5);
    b.add_limit(3, Sell, 100, 5);
    ASSERT_TRUE(b.cancel(victim));
    Resting want;
    for (OrderId id : {1u, 2u, 3u})
      if (id != victim) want.push_back({id, 100, 5});
    EXPECT_EQ(b.resting(Sell), want) << "victim " << victim;
    EXPECT_EQ(b.depth_at(Sell, 100), 10u);
    // The survivors still trade in arrival order.
    b.add_market(9, Buy, 10);
    ASSERT_EQ(b.trades().size(), 2u);
    EXPECT_EQ(b.trades()[0].maker_id, want[0].id);
    EXPECT_EQ(b.trades()[1].maker_id, want[1].id);
  }
}

TEST(Cancel, EmptyLevelDisappearsAndBestPriceMoves) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 5);
  b.add_limit(2, Buy, 99, 5);
  EXPECT_EQ(b.best_bid(), 100);
  b.cancel(1);
  EXPECT_EQ(b.best_bid(), 99);
  EXPECT_EQ(b.levels(Buy), (std::vector<LevelView>{{99, 5, 1}}));
}

// ---------- rejects and ids ----------

TEST(Reject, ZeroQuantity) {
  OrderBook b;
  EXPECT_EQ(b.add_limit(1, Buy, 100, 0).reject, Reject::ZeroQty);
  EXPECT_EQ(b.add_market(2, Buy, 0).reject, Reject::ZeroQty);
  EXPECT_EQ(b.order_count(), 0u);
}

TEST(Reject, IdOfARestingOrder) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 10);
  EXPECT_EQ(b.add_limit(1, Sell, 100, 10).reject, Reject::DuplicateId);
  EXPECT_EQ(b.add_market(1, Sell, 10).reject, Reject::DuplicateId);
  // Nothing happened: the original order is untouched.
  EXPECT_EQ(b.resting(Buy), (Resting{{1, 100, 10}}));
  EXPECT_TRUE(b.trades().empty());
}

TEST(Reject, IdCanBeReusedOnceTheOrderIsGone) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 10);
  b.cancel(1);
  EXPECT_EQ(b.add_limit(1, Buy, 101, 10).reject, Reject::None);
  EXPECT_EQ(b.best_bid(), 101);
}

// ---------- IOC ----------

TEST(Ioc, FillsWhatItCanAndDiscardsTheRest) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 4);
  b.add_limit(2, Sell, 102, 4);
  AddResult r = b.add_limit(3, Buy, 101, 10, TimeInForce::IOC);
  EXPECT_EQ(r, (AddResult{Reject::None, 4, 0, 6}));
  EXPECT_FALSE(b.best_bid());  // nothing rested
  EXPECT_EQ(b.best_ask(), 102);
}

TEST(Ioc, WithNoCrossDoesNothing) {
  OrderBook b;
  b.add_limit(1, Sell, 102, 4);
  AddResult r = b.add_limit(2, Buy, 101, 10, TimeInForce::IOC);
  EXPECT_EQ(r, (AddResult{Reject::None, 0, 0, 10}));
  EXPECT_EQ(b.order_count(), 1u);
}

// ---------- FOK ----------

TEST(Fok, FillsCompletelyAcrossLevels) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 4);
  b.add_limit(2, Sell, 101, 4);
  AddResult r = b.add_limit(3, Buy, 101, 8, TimeInForce::FOK);
  EXPECT_EQ(r, (AddResult{Reject::None, 8, 0, 0}));
  EXPECT_EQ(b.trades().size(), 2u);
  EXPECT_EQ(b.order_count(), 0u);
}

TEST(Fok, RejectedAndBookUntouchedWhenNotEnoughQuantity) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 4);
  b.add_limit(2, Sell, 101, 4);
  AddResult r = b.add_limit(3, Buy, 101, 9, TimeInForce::FOK);
  EXPECT_EQ(r, (AddResult{Reject::FokNotFillable, 0, 0, 0}));
  EXPECT_TRUE(b.trades().empty());
  EXPECT_EQ(b.resting(Sell), (Resting{{1, 100, 4}, {2, 101, 4}}));
}

TEST(Fok, OnlyCountsQuantityWithinItsLimitPrice) {
  OrderBook b;
  b.add_limit(1, Buy, 100, 4);
  b.add_limit(2, Buy, 98, 10);  // too low for the seller below
  AddResult r = b.add_limit(3, Sell, 99, 5, TimeInForce::FOK);
  EXPECT_EQ(r.reject, Reject::FokNotFillable);
  EXPECT_EQ(b.order_count(), 2u);
  r = b.add_limit(4, Sell, 98, 5, TimeInForce::FOK);
  EXPECT_EQ(r, (AddResult{Reject::None, 5, 0, 0}));
}

// ---------- views and the trade log ----------

TEST(Views, LevelsAreBestFirstWithTotals) {
  OrderBook b;
  b.add_limit(1, Buy, 99, 5);
  b.add_limit(2, Buy, 100, 5);
  b.add_limit(3, Buy, 100, 7);
  b.add_limit(4, Sell, 103, 1);
  b.add_limit(5, Sell, 102, 2);
  EXPECT_EQ(b.levels(Buy),
            (std::vector<LevelView>{{100, 12, 2}, {99, 5, 1}}));
  EXPECT_EQ(b.levels(Sell),
            (std::vector<LevelView>{{102, 2, 1}, {103, 1, 1}}));
  EXPECT_EQ(b.levels(Buy, 1), (std::vector<LevelView>{{100, 12, 2}}));
  EXPECT_EQ(b.resting(Buy), (Resting{{2, 100, 5}, {3, 100, 7}, {1, 99, 5}}));
}

TEST(TradeLog, SequenceNumbersKeepCountingAfterClear) {
  OrderBook b;
  b.add_limit(1, Sell, 100, 1);
  b.add_limit(2, Buy, 100, 1);
  b.clear_trades();
  EXPECT_TRUE(b.trades().empty());
  b.add_limit(3, Sell, 100, 1);
  b.add_limit(4, Buy, 100, 1);
  ASSERT_EQ(b.trades().size(), 1u);
  EXPECT_EQ(b.trades()[0].seq, 1u);
}

TEST(Pool, ManyAddCancelCyclesStayConsistent) {
  OrderBook b;
  for (OrderId i = 0; i < 10000; ++i) {
    b.add_limit(i, i % 2 ? Buy : Sell, i % 2 ? 90 : 110, 1 + i % 7);
    if (i % 3 == 0) EXPECT_TRUE(b.cancel(i));
  }
  std::size_t resting = b.resting(Buy).size() + b.resting(Sell).size();
  EXPECT_EQ(b.order_count(), resting);
  EXPECT_EQ(resting, 10000u - 3334u);
}

}  // namespace
