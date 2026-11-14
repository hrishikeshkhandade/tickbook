#include "tickbook/order_book.hpp"

#include <algorithm>
#include <iterator>

namespace tickbook {

namespace {

// Would a taker on `side` with limit price `limit` trade at `level_price`?
// A buyer accepts any price at or below its limit; a seller any price at or
// above.
bool crosses(Side side, Price limit, Price level_price) {
  return side == Side::Buy ? level_price <= limit : level_price >= limit;
}

// A market order is a limit order with the most generous price possible.
Price market_limit(Side side) {
  return side == Side::Buy ? std::numeric_limits<Price>::max()
                           : std::numeric_limits<Price>::min();
}

}  // namespace

OrderBook::OrderBook(std::size_t expected_orders) {
  pool_.reserve(expected_orders);
  index_.reserve(expected_orders);
}

AddResult OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty,
                               TimeInForce tif) {
  return add(id, side, price, qty, tif);
}

AddResult OrderBook::add_market(OrderId id, Side side, Qty qty) {
  return add(id, side, market_limit(side), qty, TimeInForce::IOC);
}

AddResult OrderBook::add(OrderId id, Side side, Price limit, Qty qty,
                         TimeInForce tif) {
  AddResult r;
  if (qty == 0) {
    r.reject = Reject::ZeroQty;
    return r;
  }
  if (index_.contains(id)) {
    r.reject = Reject::DuplicateId;
    return r;
  }
  // FOK must be decided before touching the book, so a failed FOK leaves no
  // trace.
  if (tif == TimeInForce::FOK && !can_fill(side, limit, qty)) {
    r.reject = Reject::FokNotFillable;
    return r;
  }

  const Qty left = match(id, side, limit, qty);
  r.filled = qty - left;
  if (left > 0) {
    if (tif == TimeInForce::GTC) {
      rest(id, side, limit, left);
      r.resting = left;
    } else {
      r.cancelled = left;
    }
  }
  return r;
}

// Is there at least `qty` resting on the opposite side at prices the taker
// accepts? Uses each level's running total, so it visits levels, not orders.
bool OrderBook::can_fill(Side side, Price limit, Qty qty) const {
  Qty need = qty;
  auto enough = [&](const Levels::value_type& entry) {
    need -= std::min(need, entry.second.total);
    return need == 0;
  };
  if (side == Side::Buy) {
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
      if (!crosses(side, limit, it->first)) break;
      if (enough(*it)) return true;
    }
  } else {
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
      if (!crosses(side, limit, it->first)) break;
      if (enough(*it)) return true;
    }
  }
  return false;
}

// Trade the incoming order against the opposite side. Returns the quantity
// that is left over.
Qty OrderBook::match(OrderId taker_id, Side side, Price limit, Qty qty) {
  Levels& opposite = side == Side::Buy ? asks_ : bids_;
  while (qty > 0 && !opposite.empty()) {
    // Best ask is the lowest price (first entry); best bid is the highest
    // (last entry).
    auto best = side == Side::Buy ? opposite.begin()
                                  : std::prev(opposite.end());
    if (!crosses(side, limit, best->first)) break;
    Level& level = best->second;

    while (qty > 0 && level.head != kNil) {
      const std::uint32_t idx = level.head;
      Node& maker = pool_[idx];
      const Qty fill = std::min(qty, maker.qty);
      trades_.push_back(
          {next_seq_++, maker.id, taker_id, side, best->first, fill});
      qty -= fill;
      maker.qty -= fill;
      level.total -= fill;
      if (maker.qty == 0) {
        // Maker is done: pop it off the front of the queue.
        level.head = maker.next;
        if (level.head != kNil) {
          pool_[level.head].prev = kNil;
        } else {
          level.tail = kNil;
        }
        --level.count;
        index_.erase(maker.id);
        free_node(idx);
      }
    }
    if (level.count == 0) opposite.erase(best);
  }
  return qty;
}

// Put the order at the back of the queue at its price.
void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  Levels& own = side == Side::Buy ? bids_ : asks_;
  // try_emplace finds the level, or creates an empty one if it is new.
  const auto level_it = own.try_emplace(price).first;
  Level& level = level_it->second;

  const std::uint32_t idx = alloc_node();
  Node& node = pool_[idx];
  node.id = id;
  node.qty = qty;
  node.level = level_it;
  node.prev = level.tail;
  node.next = kNil;
  node.side = side;

  if (level.tail != kNil) {
    pool_[level.tail].next = idx;
  } else {
    level.head = idx;
  }
  level.tail = idx;
  level.total += qty;
  ++level.count;
  index_.emplace(id, idx);
}

bool OrderBook::cancel(OrderId id) {
  const auto found = index_.find(id);
  if (found == index_.end()) return false;
  const std::uint32_t idx = found->second;
  Node& node = pool_[idx];
  Level& level = node.level->second;

  // Unlink from the queue. Because the order knows its neighbours, this
  // takes the same time wherever it sits in the queue.
  if (node.prev != kNil) {
    pool_[node.prev].next = node.next;
  } else {
    level.head = node.next;
  }
  if (node.next != kNil) {
    pool_[node.next].prev = node.prev;
  } else {
    level.tail = node.prev;
  }
  level.total -= node.qty;
  --level.count;
  if (level.count == 0) {
    (node.side == Side::Buy ? bids_ : asks_).erase(node.level);
  }

  index_.erase(found);
  free_node(idx);
  return true;
}

// The pool hands out slots in pool_. Freed slots form a singly linked list
// (through Node::next) and are reused before the vector grows.
std::uint32_t OrderBook::alloc_node() {
  if (free_head_ != kNil) {
    const std::uint32_t idx = free_head_;
    free_head_ = pool_[idx].next;
    return idx;
  }
  pool_.emplace_back();
  return static_cast<std::uint32_t>(pool_.size() - 1);
}

void OrderBook::free_node(std::uint32_t idx) {
  pool_[idx].next = free_head_;
  free_head_ = idx;
}

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return std::prev(bids_.end())->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

Qty OrderBook::depth_at(Side side, Price price) const {
  const Levels& own = side == Side::Buy ? bids_ : asks_;
  const auto it = own.find(price);
  return it == own.end() ? 0 : it->second.total;
}

std::vector<LevelView> OrderBook::levels(Side side,
                                         std::size_t max_levels) const {
  std::vector<LevelView> out;
  auto take = [&](const Levels::value_type& entry) {
    out.push_back({entry.first, entry.second.total, entry.second.count});
  };
  if (side == Side::Buy) {
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
      if (out.size() == max_levels) break;
      take(*it);
    }
  } else {
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
      if (out.size() == max_levels) break;
      take(*it);
    }
  }
  return out;
}

std::vector<RestingOrder> OrderBook::resting(Side side) const {
  std::vector<RestingOrder> out;
  out.reserve(index_.size());
  auto take = [&](const Levels::value_type& entry) {
    for (std::uint32_t i = entry.second.head; i != kNil; i = pool_[i].next) {
      out.push_back({pool_[i].id, entry.first, pool_[i].qty});
    }
  };
  if (side == Side::Buy) {
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) take(*it);
  } else {
    for (auto it = asks_.begin(); it != asks_.end(); ++it) take(*it);
  }
  return out;
}

}  // namespace tickbook
