// A deliberately slow order book used only to check the real engine.
//
// It keeps every resting order in one flat vector and finds the next order
// to trade by scanning all of them. There are no price levels, no linked
// lists, no pool and no id index, so it shares no data structure (and no
// data structure bug) with the real engine. Each rule is written the way you
// would say it out loud.
#pragma once

#include "tickbook/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace tickbook::reference {

class ReferenceBook {
 public:
  AddResult add_limit(OrderId id, Side side, Price price, Qty qty,
                      TimeInForce tif = TimeInForce::GTC) {
    return add(id, side, price, qty, tif);
  }

  AddResult add_market(OrderId id, Side side, Qty qty) {
    return add(id, side, std::nullopt, qty, TimeInForce::IOC);
  }

  bool cancel(OrderId id) {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (orders_[i].id == id) {
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  }

  // Resting orders on one side, best price first, then oldest first.
  std::vector<RestingOrder> resting(Side side) const {
    std::vector<Order> mine;
    for (const Order& o : orders_) {
      if (o.side == side) mine.push_back(o);
    }
    std::sort(mine.begin(), mine.end(), [&](const Order& a, const Order& b) {
      if (a.price != b.price) {
        return side == Side::Buy ? a.price > b.price : a.price < b.price;
      }
      return a.arrival < b.arrival;
    });
    std::vector<RestingOrder> out;
    for (const Order& o : mine) out.push_back({o.id, o.price, o.qty});
    return out;
  }

  const std::vector<Trade>& trades() const { return trades_; }
  void clear_trades() { trades_.clear(); }

 private:
  struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    std::uint64_t arrival;  // 0, 1, 2, ... in the order they rested
  };

  // Can a taker (side, limit) trade with this resting order? No limit means
  // a market order, which accepts any price.
  static bool tradable(Side side, std::optional<Price> limit, const Order& o) {
    if (o.side == side) return false;
    if (!limit) return true;
    return side == Side::Buy ? o.price <= *limit : o.price >= *limit;
  }

  AddResult add(OrderId id, Side side, std::optional<Price> limit, Qty qty,
                TimeInForce tif) {
    AddResult r;
    if (qty == 0) {
      r.reject = Reject::ZeroQty;
      return r;
    }
    for (const Order& o : orders_) {
      if (o.id == id) {
        r.reject = Reject::DuplicateId;
        return r;
      }
    }
    if (tif == TimeInForce::FOK) {
      Qty available = 0;
      for (const Order& o : orders_) {
        if (tradable(side, limit, o)) available += o.qty;
      }
      if (available < qty) {
        r.reject = Reject::FokNotFillable;
        return r;
      }
    }

    Qty left = qty;
    while (left > 0) {
      // Scan everything for the single best order to trade with next.
      Order* best = nullptr;
      for (Order& o : orders_) {
        if (!tradable(side, limit, o)) continue;
        if (best == nullptr) {
          best = &o;
          continue;
        }
        // The taker wants the lowest ask when buying, the highest bid when
        // selling. Ties go to the order that arrived first.
        const bool better_price =
            side == Side::Buy ? o.price < best->price : o.price > best->price;
        const bool same_price_but_older =
            o.price == best->price && o.arrival < best->arrival;
        if (better_price || same_price_but_older) best = &o;
      }
      if (best == nullptr) break;

      const Qty fill = std::min(left, best->qty);
      trades_.push_back({next_seq_++, best->id, id, side, best->price, fill});
      left -= fill;
      best->qty -= fill;
      if (best->qty == 0) orders_.erase(orders_.begin() + (best - orders_.data()));
    }

    r.filled = qty - left;
    if (left > 0) {
      if (tif == TimeInForce::GTC) {
        orders_.push_back({id, side, *limit, left, next_arrival_++});
        r.resting = left;
      } else {
        r.cancelled = left;
      }
    }
    return r;
  }

  std::vector<Order> orders_;
  std::vector<Trade> trades_;
  std::uint64_t next_arrival_ = 0;
  std::uint64_t next_seq_ = 0;
};

}  // namespace tickbook::reference
