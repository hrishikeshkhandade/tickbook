#include "tickbook/order_book.hpp"

namespace tickbook {

// Stub: every operation does nothing. The tests are written first and must
// fail against this before the real engine is written.
OrderBook::OrderBook(std::size_t) {}
AddResult OrderBook::add_limit(OrderId, Side, Price, Qty, TimeInForce) {
  return {};
}
AddResult OrderBook::add_market(OrderId, Side, Qty) { return {}; }
bool OrderBook::cancel(OrderId) { return false; }
std::optional<Price> OrderBook::best_bid() const { return std::nullopt; }
std::optional<Price> OrderBook::best_ask() const { return std::nullopt; }
Qty OrderBook::depth_at(Side, Price) const { return 0; }
std::vector<LevelView> OrderBook::levels(Side, std::size_t) const { return {}; }
std::vector<RestingOrder> OrderBook::resting(Side) const { return {}; }

}  // namespace tickbook
