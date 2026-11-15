// A thin C interface over the engine, for WebAssembly.
//
// Why this file exists: JavaScript cannot call C++ member functions or read a
// std::vector. It can call plain C functions that take and return numbers, and
// it can read the module's memory as an array of integers. So every function
// here takes numbers, and anything bigger than one number (a list of price
// levels, a list of trades) is written into one buffer of 32-bit integers that
// JavaScript reads through tb_out().
//
// This file adds no matching logic. It owns one OrderBook, hands out order
// ids, and copies results into the buffer.
//
// `extern "C"` turns off C++ name mangling, so the function is exported under
// the exact name written here (tb_add_limit, not something like
// _Z12tb_add_limitjijj).
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "tickbook/order_book.hpp"

namespace {

using tickbook::OrderBook;
using tickbook::Side;
using tickbook::TimeInForce;

// The state lives inside a function and is built on first use. A global
// OrderBook would need its constructor to run before any export is called,
// which a WebAssembly module with no main() does not promise.
struct State {
  std::unique_ptr<OrderBook> book = std::make_unique<OrderBook>();
  tickbook::OrderId next_id = 1;
  std::vector<std::uint32_t> out;
};

State& state() {
  static State s;
  return s;
}

// The engine uses 64-bit quantities and ids. The buffer is 32-bit so that
// JavaScript can read it as plain numbers. A value too big for 32 bits is
// clamped to the largest one, never wrapped around.
std::uint32_t narrow(std::uint64_t v) {
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(v, std::numeric_limits<std::uint32_t>::max()));
}

// Prices are signed. They are stored in the same buffer bit for bit, and
// JavaScript reads those slots through a signed view.
std::uint32_t price_bits(tickbook::Price p) {
  const auto lo = std::numeric_limits<std::int32_t>::min();
  const auto hi = std::numeric_limits<std::int32_t>::max();
  return static_cast<std::uint32_t>(
      static_cast<std::int32_t>(std::clamp<tickbook::Price>(p, lo, hi)));
}

void write_result(const tickbook::AddResult& r) {
  auto& out = state().out;
  out.assign({static_cast<std::uint32_t>(r.reject), narrow(r.filled),
              narrow(r.resting), narrow(r.cancelled)});
}

}  // namespace

extern "C" {

// Throws the book away and starts an empty one. Order ids restart at 1.
void tb_reset() { state() = State{}; }

// Where the last call left its data. Ask again after every call: the buffer
// can move when it grows.
const std::uint32_t* tb_out() { return state().out.data(); }

// side: 0 buy, 1 sell. tif: 0 GTC, 1 IOC, 2 FOK.
// Returns the id given to the order, or 0 if side or tif is not a valid code.
// Buffer afterwards: [reject, filled, resting, cancelled].
std::uint32_t tb_add_limit(std::uint32_t side, std::int32_t price,
                           std::uint32_t qty, std::uint32_t tif) {
  if (side > 1 || tif > 2) return 0;
  State& s = state();
  const tickbook::OrderId id = s.next_id++;
  write_result(s.book->add_limit(id, static_cast<Side>(side), price, qty,
                                 static_cast<TimeInForce>(tif)));
  return narrow(id);
}

// Same return value and buffer as tb_add_limit.
std::uint32_t tb_add_market(std::uint32_t side, std::uint32_t qty) {
  if (side > 1) return 0;
  State& s = state();
  const tickbook::OrderId id = s.next_id++;
  write_result(s.book->add_market(id, static_cast<Side>(side), qty));
  return narrow(id);
}

// Returns 1 if the order was resting and is now removed, 0 otherwise.
std::uint32_t tb_cancel(std::uint32_t id) {
  return state().book->cancel(id) ? 1 : 0;
}

// Price levels on one side, best price first. Returns how many.
// Buffer: [price, qty, orders] for each level.
std::uint32_t tb_levels(std::uint32_t side, std::uint32_t max_levels) {
  State& s = state();
  s.out.clear();
  if (side > 1) return 0;
  const auto levels = s.book->levels(static_cast<Side>(side), max_levels);
  for (const auto& l : levels) {
    s.out.push_back(price_bits(l.price));
    s.out.push_back(narrow(l.qty));
    s.out.push_back(narrow(l.orders));
  }
  return narrow(levels.size());
}

// Every resting order on one side, in the order it would trade.
// Returns how many. Buffer: [id, price, qty] for each order.
std::uint32_t tb_resting(std::uint32_t side) {
  State& s = state();
  s.out.clear();
  if (side > 1) return 0;
  const auto orders = s.book->resting(static_cast<Side>(side));
  for (const auto& o : orders) {
    s.out.push_back(narrow(o.id));
    s.out.push_back(price_bits(o.price));
    s.out.push_back(narrow(o.qty));
  }
  return narrow(orders.size());
}

// Trades since the last call, oldest first, then empties the engine's trade
// log. Returns how many.
// Buffer: [seq, maker_id, taker_id, taker_side, price, qty] for each trade.
std::uint32_t tb_take_trades() {
  State& s = state();
  s.out.clear();
  const auto& trades = s.book->trades();
  for (const auto& t : trades) {
    s.out.push_back(narrow(t.seq));
    s.out.push_back(narrow(t.maker_id));
    s.out.push_back(narrow(t.taker_id));
    s.out.push_back(static_cast<std::uint32_t>(t.taker_side));
    s.out.push_back(price_bits(t.price));
    s.out.push_back(narrow(t.qty));
  }
  const std::uint32_t n = narrow(trades.size());
  s.book->clear_trades();
  return n;
}

// Number of orders resting in the book, both sides.
std::uint32_t tb_order_count() { return narrow(state().book->order_count()); }

}  // extern "C"
