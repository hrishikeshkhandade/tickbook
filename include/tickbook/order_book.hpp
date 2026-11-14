// Tickbook: a single-instrument limit order book and matching engine.
//
// Rules in one paragraph: orders match by price-time priority. The best price
// goes first; at the same price the order that arrived first goes first. A
// trade always happens at the resting (maker) order's price. Prices are whole
// numbers of ticks and quantities are whole units, so no floating point is
// ever used for money.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tickbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // in ticks; may be negative
using Qty = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// What happens to the part of a limit order that cannot trade right away.
enum class TimeInForce : std::uint8_t {
  GTC,  // good till cancelled: the remainder rests in the book
  IOC,  // immediate or cancel: the remainder is discarded
  FOK,  // fill or kill: trade the whole quantity now, or do nothing at all
};

enum class Reject : std::uint8_t {
  None,
  ZeroQty,         // quantity was 0
  DuplicateId,     // an order with this id is resting in the book right now
  FokNotFillable,  // FOK order could not be filled completely
};

struct Trade {
  std::uint64_t seq;  // 0, 1, 2, ... over the life of the book
  OrderId maker_id;   // the order that was resting
  OrderId taker_id;   // the order that arrived and crossed
  Side taker_side;
  Price price;  // always the maker's price
  Qty qty;
  bool operator==(const Trade&) const = default;
};

// Outcome of adding an order. When reject == None,
// filled + resting + cancelled == the quantity that was submitted.
struct AddResult {
  Reject reject = Reject::None;
  Qty filled = 0;
  Qty resting = 0;
  Qty cancelled = 0;
  bool operator==(const AddResult&) const = default;
};

struct RestingOrder {
  OrderId id;
  Price price;
  Qty qty;
  bool operator==(const RestingOrder&) const = default;
};

struct LevelView {
  Price price;
  Qty qty;             // total quantity resting at this price
  std::size_t orders;  // number of orders at this price
  bool operator==(const LevelView&) const = default;
};

class OrderBook {
 public:
  // expected_orders only pre-sizes memory; the book grows past it if needed.
  explicit OrderBook(std::size_t expected_orders = 0);

  AddResult add_limit(OrderId id, Side side, Price price, Qty qty,
                      TimeInForce tif = TimeInForce::GTC);
  // A market order trades against whatever is there; any remainder is
  // discarded because it has no price to rest at.
  AddResult add_market(OrderId id, Side side, Qty qty);
  // Returns false if no order with this id is resting.
  bool cancel(OrderId id);

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  Qty depth_at(Side side, Price price) const;
  std::size_t order_count() const { return index_.size(); }

  // Snapshots, best price first. These copy; they are for tests and for a
  // future visualizer, not for the hot path.
  std::vector<LevelView> levels(
      Side side,
      std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const;
  std::vector<RestingOrder> resting(Side side) const;

  // Every fill is appended here. The caller decides when to clear it.
  const std::vector<Trade>& trades() const { return trades_; }
  void clear_trades() { trades_.clear(); }

 private:
  static constexpr std::uint32_t kNil =
      std::numeric_limits<std::uint32_t>::max();

  // One price level: a first-in-first-out queue of orders, stored as a
  // doubly linked list threaded through the order pool.
  struct Level {
    Qty total = 0;
    std::uint32_t head = kNil;  // oldest order, first to trade
    std::uint32_t tail = kNil;  // newest order
    std::uint32_t count = 0;
  };
  // Both sides use ascending order. Best ask is the first entry, best bid is
  // the last. Using one map type for both sides keeps the code free of
  // templates.
  using Levels = std::map<Price, Level>;

  // One resting order. prev/next are indices into pool_, not pointers, so
  // they stay valid when the pool vector grows and moves.
  struct Node {
    OrderId id = 0;
    Qty qty = 0;  // remaining quantity
    Levels::iterator level;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;  // doubles as the free-list link when unused
    Side side = Side::Buy;
  };

  AddResult add(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif);
  bool can_fill(Side side, Price limit, Qty qty) const;
  Qty match(OrderId taker_id, Side side, Price limit, Qty qty);
  void rest(OrderId id, Side side, Price price, Qty qty);
  std::uint32_t alloc_node();
  void free_node(std::uint32_t idx);

  Levels bids_;
  Levels asks_;
  std::vector<Node> pool_;
  std::uint32_t free_head_ = kNil;
  std::unordered_map<OrderId, std::uint32_t> index_;  // id -> slot in pool_
  std::vector<Trade> trades_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace tickbook
