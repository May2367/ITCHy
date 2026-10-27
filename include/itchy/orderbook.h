#ifndef ITCHY_ORDERBOOK_H
#define ITCHY_ORDERBOOK_H

#include "heap.h"
#include "types.h"

#include <stddef.h>

typedef struct PriceLevel PriceLevel;

typedef struct {
    PriceLevel **items;
    size_t size;
    size_t capacity;
    char side;
} LevelHeap;

typedef struct {
    uint32_t price;
    uint64_t shares;
} BookLevel;

typedef struct {
    uint64_t executed_shares;
    uint64_t resting_shares;
    uint64_t trades;
} MatchReport;

typedef enum {
    ORDERBOOK_MSG_ADD,
    ORDERBOOK_MSG_EXECUTE,
    ORDERBOOK_MSG_CANCEL,
    ORDERBOOK_MSG_DELETE,
    ORDERBOOK_MSG_REPLACE,
    ORDERBOOK_MSG_UPSERT_LEVEL,
    ORDERBOOK_MSG_MATCH_LIMIT
} OrderBookMessageType;

typedef struct {
    OrderBookMessageType type;
    uint64_t order_id;
    uint64_t new_order_id;
    char side;
    uint32_t shares;
    uint32_t price;
    char stock[ITCHY_TICKER_LEN + 1];
} OrderBookMessage;

typedef struct {
    unsigned char *data;
    size_t size;
} OrderBookSnapshot;

typedef struct {
    Heap bids;
    Heap asks;
    Order *orders;
    size_t order_count;
    PriceLevel *bid_levels;
    PriceLevel *ask_levels;
    LevelHeap level_bids;
    LevelHeap level_asks;
    uint64_t next_arrival_sequence;
    Order best_bid_view;
    Order best_ask_view;
} OrderBook;

int orderbook_init(OrderBook *book);
void orderbook_free(OrderBook *book);
int orderbook_add(OrderBook *book, uint64_t order_id, char side, uint32_t shares,
                  const char *stock, uint32_t price);
int orderbook_execute(OrderBook *book, uint64_t order_id, uint32_t shares);
int orderbook_cancel(OrderBook *book, uint64_t order_id, uint32_t shares);
int orderbook_delete(OrderBook *book, uint64_t order_id);
int orderbook_replace(OrderBook *book, uint64_t old_order_id, uint64_t new_order_id,
                      uint32_t shares, uint32_t price);
int orderbook_upsert_level(OrderBook *book, uint64_t level_id, char side, uint32_t shares,
                           const char *stock, uint32_t price);
int orderbook_match_limit(OrderBook *book, uint64_t order_id, char side, uint32_t shares,
                          const char *stock, uint32_t price, MatchReport *report);
int orderbook_apply(OrderBook *book, const OrderBookMessage *msg, MatchReport *report);
Order *orderbook_best_bid(const OrderBook *book);
Order *orderbook_best_ask(const OrderBook *book);
size_t orderbook_depth(const OrderBook *book, char side, BookLevel *levels, size_t max_levels);
int orderbook_shadow_best_matches(const OrderBook *book);
int orderbook_snapshot(const OrderBook *book, OrderBookSnapshot *snapshot);
void orderbook_snapshot_free(OrderBookSnapshot *snapshot);
int orderbook_restore(OrderBook *book, const OrderBookSnapshot *snapshot);
int orderbook_state_hash(const OrderBook *book, uint64_t *hash);

#endif
