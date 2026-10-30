# Design Notes

## Parser

`parser_run_stream()` reads one gzip-framed message at a time, decodes supported message types into `OrderBookMessage`, and applies updates for the configured ticker to one `OrderBook`. File opening, ticker requirement enforcement, CSV output, replay event-log output, and benchmark timing live outside the parser.

## Book

The book keeps an order lookup plus aggregated price levels:

- `uthash` maps order ID to `Order` for add, cancel, delete, execute, and replace.
- A price-level hash maps price to aggregate shares and a FIFO queue of resting orders for each side.
- A max-heap of bid levels and a min-heap of ask levels provide fast best-price lookup.
- Matching uses the best price level and consumes from that level's FIFO head, so the order book no longer maintains separate per-order bid/ask heaps.

The price-level hash plus heap keeps the hot update path close to the original code: order lookup is still O(1) average, best bid/ask is O(1), and adding or removing a price level is O(log n). Updating shares at an existing price is O(1). The tradeoff is depth: `orderbook_depth()` copies and sorts the current levels before returning the top N, so it is simple and deterministic but not optimized for very frequent full-depth queries.

A sorted array would make depth reads cheap but would shift more cost into inserts and deletes. A skiplist or tree would be a better long-term fit if depth queries become part of the hot path, but it would add more code and pointer-heavy behavior than this project needs right now.

## State Machine and Snapshots

`orderbook_apply()` wraps the decoded book operations in one small message API. The parser still decodes feed records the same way, but tests and benchmarks can replay decoded operations through one deterministic entry point.

`orderbook_snapshot()` serializes active orders into a byte buffer sorted by order ID, including each order arrival sequence and the next arrival counter. Price-level hashes, price-level heaps, and FIFO links are treated as derived state and rebuilt by `orderbook_restore()`. `orderbook_state_hash()` hashes that serialized image, so replay comparisons include the price-time-priority state, not just price and quantity.

The local replay path writes decoded applied messages as CSV with `seq,timestamp_ns,type,order_id,new_order_id,side,shares,price,stock,state_hash`. `--replay` reads that file, checks sequence continuity, reapplies each message through `orderbook_apply()`, and compares the post-apply state hash after every event. This log is intentionally the state-machine input stream, not the publisher top-of-book stream; top-of-book rows are useful for consumers but do not contain enough information to rebuild the full book.

## Matching Engine

`orderbook_match_limit()` is an opt-in API for crossing synthetic limit orders. Normal parser behavior is unchanged: parsed ITCH-style add/cancel/delete/execute/replace messages still apply directly to the book.

Matching uses price priority first. Within a price level, resting orders are filled by arrival sequence, so FIFO no longer depends on order ID. Any remaining incoming quantity rests through the same order and price-level structures as normal adds.

## Threaded Apply Benchmark

Phase 3a adds a C11-atomic single-producer/single-consumer ring. The benchmark uses one producer thread to enqueue deterministic decoded messages and one consumer thread to apply them to the book. The same generated message stream is also applied single-threaded so the benchmark can report a direct comparison.

The ring stores fixed-size records and uses atomic head/tail indexes. It also exposes batch push and pop helpers so the benchmark can amortize atomic index updates across decoded-message groups. It is single-producer/single-consumer only; using multiple producers or consumers would require a different queue.


## Event Publisher

Top-of-book publishing is optional and off the parser callback path. The hot path copies the current best bid/ask into a fixed-size event, assigns a monotonic sequence number, and pushes it into a C11-atomic SPSC ring. A publisher thread drains the ring through a small internal sink interface. The current sink implementation writes CSV to a file.

The publisher reports enqueue, write, and drop counters. Real-feed QQQ runs recorded zero drops at ring sizes 4096, 65536, and 262144. Synthetic stress runs are used to size the default ring and exercise saturation behavior.

## Feed Layouts

The main parser path targets raw order-level ITCH messages. Add, execute, cancel, delete, and replace messages are decoded into a small internal `OrderBookMessage` format and applied through the order-ID lookup plus price-level book.

Some aggregate sample formats use non-standard records for price-level updates. Those records are represented as synthetic order IDs and flow through the same price-level update path, so top-of-book and depth include them.

## Limitations

- Normal CLI parsing is single-symbol only and requires `--ticker`; multi-symbol book management is not implemented.
- No live feed, packet replay, or DPDK path.
- Depth is rebuilt on demand instead of maintained as a sorted structure.
- Snapshot restore is an in-memory API; the replay event log is a local test/debug format, not a long-term compatibility contract.
- Benchmark mode uses fixed-seed synthetic workloads and reports single-threaded apply, SPSC-threaded apply, publisher file-sink, and matching-engine throughput plus batched per-message latency.
