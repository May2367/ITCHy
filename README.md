# ITCHy

ITCHy is an offline parser and order book engine for a gzip-compressed, length-prefixed ITCH-style feed. It reads records from a file, applies supported book updates, and can emit top-of-book CSV rows or run the built-in benchmark mode.

It is not a live market data gateway. It does not use DPDK, packet replay, sockets, or Kafka. The parser path applies feed updates offline; the matching engine is an opt-in API for synthetic crossing orders. An optional publisher thread can drain top-of-book events to a CSV file sink, and an event-log/replay path can check deterministic state hashes from local files.

## Build

Dependencies on Debian/Ubuntu:

```sh
sudo apt-get install make gcc libc6-dev zlib1g-dev uthash-dev
```

Build and test:

```sh
make build
make test
make test-sanitize
```

Build the Docker image if you want a clean Debian build environment:

```sh
docker build -t itchy .
```

Run the small benchmark smoke target:

```sh
make bench
```

Benchmark mode uses a fixed-seed synthetic apply workload. The positional input argument is kept for CLI compatibility and is not read in benchmark mode:

```sh
./itchy synthetic --benchmark
```

## Usage

```sh
./itchy input.gz [--ticker SYMBOL] [--output FILE] [--interval-ns N] [--publish FILE] [--publish-ring N] [--event-log FILE] [--benchmark]
./itchy event-log.csv --replay
```

Normal mode writes CSV with:

```text
timestamp_ns,best_bid,bid_shares,best_ask,ask_shares,spread
```

`--publish FILE` also sends top-of-book events through a bounded SPSC ring to a publisher thread. Publisher output adds a leading `seq` column before the normal CSV fields. If the ring fills, the hot path drops the event, consumes a sequence number, and reports the dropped count on stderr at shutdown.

`--event-log FILE` writes the decoded applied order-book messages with a sequence number and post-apply state hash. `--replay` reads that log, reapplies each message, checks sequence continuity, and compares the stored hash after every event. This is a local deterministic replay check; it is separate from the top-of-book publisher CSV and does not require Kafka.

Benchmark mode generates deterministic synthetic workloads, runs warmup passes, then reports single-threaded apply performance, a two-thread SPSC producer/consumer comparison, a file-sink publisher comparison, and a matching-engine workload. Environment variables control the run: `ITCHY_BENCH_MESSAGES`, `ITCHY_BENCH_RUNS`, `ITCHY_BENCH_WARMUP_RUNS`, `ITCHY_BENCH_BATCH`, `ITCHY_BENCH_RING`, `ITCHY_MATCH_BENCH_MESSAGES`, and `ITCHY_BENCH_SEED`.

## Input Format

The parser expects gzip-compressed records framed as:

```text
2-byte big-endian length
message bytes
```

Supported order-book messages include add, execute, cancel, delete, and replace-style records for the layouts used by this project. Known administrative/reference messages are counted but do not change the book.

The sample feed this project was built around contains non-standard 34-byte `U` records. Those records do not match Nasdaq TotalView-ITCH 5.0 Order Replace. They are treated as aggregate price-level updates, as described in [docs/assumptions.md](docs/assumptions.md).

## Current Book Model

The current book keeps an order-ID hash table for order updates and aggregated price levels per side. Best bid/ask queries use bid/ask price-level heaps, and `orderbook_depth()` returns sorted top-N levels. The old per-order heaps remain internally as a shadow check during the price-level transition. `orderbook_apply()` is the decoded-message state-machine entry point; snapshots, restore, and `orderbook_state_hash()` support deterministic replay checks. The parser can emit decoded applied messages to a local event log, and `--replay` replays that log against the state hash. `orderbook_match_limit()` provides simple price-time matching for crossing limit orders without changing normal parser behavior. The optional publisher path keeps file writes off the parser callback path by enqueueing top-of-book events into a bounded SPSC ring. The publisher drains through a small sink interface; the implemented sink is CSV file output.

See [docs/design.md](docs/design.md) for the current design notes and known limitations.

## FABRIC Workflow

FABRIC VM transfer scripts and the notebook live under [scripts/fabric/vm_link_test](scripts/fabric/vm_link_test). That workflow transfers the gzip file between VMs and then parses it offline on the receiver.
