# Design Notes

## Parser

`parser_run_stream()` reads one gzip-framed message at a time, decodes supported message types, and applies updates to an `OrderBook`. File opening, CSV output, and benchmark timing live outside the parser.

## Book

The book now keeps two views of the same state:

- `uthash` maps order ID to `Order` for add, cancel, delete, execute, and replace.
- A price-level hash maps price to aggregate shares for each side.
- A max-heap of bid levels and a min-heap of ask levels provide fast best-price lookup.
- The old per-order bid/ask heaps remain as a shadow path during the price-level transition. Tests compare shadow best prices against price-level best prices after deterministic update sequences.

The price-level hash plus heap keeps the hot update path close to the original code: order lookup is still O(1) average, best bid/ask is O(1), and adding or removing a price level is O(log n). Updating shares at an existing price is O(1). The tradeoff is depth: `orderbook_depth()` copies and sorts the current levels before returning the top N, so it is simple and deterministic but not optimized for very frequent full-depth queries.

A sorted array would make depth reads cheap but would shift more cost into inserts and deletes. A skiplist or tree would be a better long-term fit if depth queries become part of the hot path, but it would add more code and pointer-heavy behavior than this project needs right now.

## State Machine and Snapshots

`orderbook_apply()` wraps the decoded book operations in one small message API. The parser still decodes feed records the same way, but tests and benchmarks can replay decoded operations through one deterministic entry point.

`orderbook_snapshot()` serializes the active orders into a byte buffer sorted by order ID. The heaps and price-level hashes are treated as derived state and rebuilt by `orderbook_restore()`. `orderbook_state_hash()` hashes that serialized image, so two runs with the same final active orders compare equal even if hash-table iteration or heap insertion order differed internally.

## Matching Engine

`orderbook_match_limit()` is an opt-in API for crossing synthetic limit orders. Normal parser behavior is unchanged: parsed ITCH-style add/cancel/delete/execute/replace messages still apply directly to the book.

Matching uses price priority first. Within a price level, the current implementation uses the lowest resting order ID as the time-priority proxy. That matches the synthetic tests and keeps the implementation small, but it is not a full exchange timestamp or sequence-number model. Any remaining incoming quantity rests through the same order and price-level structures as normal adds.

## Threaded Apply Benchmark

Phase 3a adds a C11-atomic single-producer/single-consumer ring. The benchmark uses one producer thread to enqueue deterministic decoded messages and one consumer thread to apply them to the book. The same generated message stream is also applied single-threaded so the benchmark can report a direct comparison.

The ring stores fixed-size records and uses atomic head/tail indexes. It also exposes batch push and pop helpers so the benchmark can amortize atomic index updates across decoded-message groups. It is single-producer/single-consumer only; using multiple producers or consumers would require a different queue.


## Event Publisher

Top-of-book publishing is optional and off the parser callback path. The hot path copies the current best bid/ask into a fixed-size event, assigns a monotonic sequence number, and tries to push it into a C11-atomic SPSC ring. A publisher thread drains the ring through a small sink interface. The current sink implementation writes CSV to a file.

The queue policy is bounded and nonblocking: if the ring is full, the event is dropped, a counter is incremented, and the consumed sequence number leaves a detectable gap. That favors parser latency over guaranteed delivery. Normal parser output still works without the publisher, and benchmark mode reports a `publisher_file_sink` section plus enqueue, write, and drop counts. A Kafka sink would plug in at this sink boundary, but there is no librdkafka dependency or Kafka output path yet.

## Sample Aggregate U Records

The sample-specific 34-byte `U` records are represented as synthetic order IDs. They still flow through the same order and price-level update path, so aggregate top-of-book and depth include those updates.

## Limitations

- No live feed, packet replay, or DPDK path.
- Depth is rebuilt on demand instead of maintained as a sorted structure.
- Snapshot restore is an in-memory API; there is no external snapshot file format or compatibility promise yet.
- Benchmark mode uses fixed-seed synthetic workloads and reports single-threaded apply, SPSC-threaded apply, publisher file-sink, and matching-engine throughput plus batched per-message latency.
