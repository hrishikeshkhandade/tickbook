// Loads the built module in Node and checks a few trades end to end.
// Run: node wasm/smoke.mjs   (after building the wasm preset)
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const file = new URL('../build/wasm/tickbook.wasm', import.meta.url)
const module = new WebAssembly.Module(readFileSync(file))
console.log('imports:', WebAssembly.Module.imports(module).map((i) => `${i.module}.${i.name}`))
const stub = new Proxy({}, { get: () => () => 0 })
const { exports: tb } = new WebAssembly.Instance(module, { wasi_snapshot_preview1: stub, env: stub })
tb._initialize?.()

const out = (n) => new Int32Array(tb.memory.buffer, tb.tb_out(), n)
const BUY = 0, SELL = 1, GTC = 0, IOC = 1, FOK = 2

tb.tb_reset()
const a = tb.tb_add_limit(SELL, 101, 5, GTC)
const b = tb.tb_add_limit(SELL, 101, 7, GTC)
tb.tb_add_limit(SELL, 103, 4, GTC)
assert.deepEqual([a, b], [1, 2])
assert.deepEqual([...out(4)], [0, 0, 4, 0]) // last add rested in full

// Buy 8 at up to 102: takes all 5 of order 1, then 3 of order 2, both at 101. 0 left to rest.
const c = tb.tb_add_limit(BUY, 102, 8, GTC)
assert.deepEqual([...out(4)], [0, 8, 0, 0])
assert.equal(tb.tb_take_trades(), 2)
assert.deepEqual([...out(12)], [0, a, c, BUY, 101, 5, 1, b, c, BUY, 101, 3])
assert.equal(tb.tb_take_trades(), 0)

assert.equal(tb.tb_levels(SELL, 10), 2)
assert.deepEqual([...out(6)], [101, 4, 1, 103, 4, 1])
assert.equal(tb.tb_levels(BUY, 10), 0)

// FOK for more than the book holds does nothing; reject code 3.
tb.tb_add_limit(BUY, 200, 9, FOK)
assert.deepEqual([...out(4)], [3, 0, 0, 0])
// IOC takes what is there and drops the rest.
tb.tb_add_limit(BUY, 101, 6, IOC)
assert.deepEqual([...out(4)], [0, 4, 0, 2])
// Market sell into an empty bid side: nothing to trade, all cancelled.
tb.tb_add_market(SELL, 3)
assert.deepEqual([...out(4)], [0, 0, 0, 3])
// Negative prices survive the trip.
const d = tb.tb_add_limit(BUY, -7, 2, GTC)
assert.equal(tb.tb_resting(BUY), 1)
assert.deepEqual([...out(3)], [d, -7, 2])
assert.equal(tb.tb_cancel(d), 1)
assert.equal(tb.tb_cancel(d), 0)
assert.equal(tb.tb_order_count(), 1)
assert.equal(tb.tb_add_limit(2, 1, 1, GTC), 0) // bad side code

// Grow well past the starting memory.
tb.tb_reset()
for (let i = 0; i < 300000; i++) tb.tb_add_limit(BUY, 1000 - (i % 500), 1, GTC)
assert.equal(tb.tb_order_count(), 300000)
assert.equal(tb.tb_levels(BUY, 3), 3)
assert.deepEqual([...out(9)], [1000, 600, 600, 999, 600, 600, 998, 600, 600])
console.log('wasm smoke test passed,', file.pathname.split('/').pop(), readFileSync(file).length, 'bytes')
