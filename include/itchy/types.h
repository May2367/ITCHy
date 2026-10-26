#ifndef ITCHY_TYPES_H
#define ITCHY_TYPES_H

#include <stdint.h>
#include <stdio.h>

#include <uthash.h>

#define ITCHY_TICKER_LEN 8
typedef enum {
    RUN_MODE_NORMAL = 0,
    RUN_MODE_BENCHMARK = 1
} RunMode;

typedef struct {
    const char *input_path;
    const char *output_path;
    char ticker[ITCHY_TICKER_LEN + 1];
    uint64_t update_interval_ns;
    RunMode run_mode;
} Config;

typedef struct Order {
    uint64_t order_id;
    uint64_t shares;
    uint32_t price;
    char side;
    char stock[ITCHY_TICKER_LEN + 1];
    size_t heap_index;
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
