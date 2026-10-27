#ifndef ITCHY_TYPES_H
#define ITCHY_TYPES_H

#include <stdint.h>
#include <stdio.h>

#include <uthash.h>

#define ITCHY_TICKER_LEN 8
typedef enum {
    RUN_MODE_NORMAL = 0,
    RUN_MODE_BENCHMARK = 1,
    RUN_MODE_REPLAY = 2
} RunMode;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *publish_path;
    const char *event_log_path;
    char ticker[ITCHY_TICKER_LEN + 1];
    uint64_t update_interval_ns;
    uint64_t publish_ring_capacity;
    uint64_t max_messages;
    uint64_t progress_interval;
    RunMode run_mode;
} Config;

struct PriceLevel;

typedef struct Order {
    uint64_t order_id;
    uint64_t arrival_sequence;
    uint64_t shares;
    uint32_t price;
    char side;
    char stock[ITCHY_TICKER_LEN + 1];
    struct PriceLevel *level;
    struct Order *level_prev;
    struct Order *level_next;
    UT_hash_handle hh;
} Order;

typedef struct {
    uint64_t messages_read;
    uint64_t messages_applied;
    uint64_t messages_ignored;
    uint64_t unknown_messages;
    uint64_t malformed_messages;
    uint64_t last_timestamp;
    int io_error;
} ParseResult;

#endif
