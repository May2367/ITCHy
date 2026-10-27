#include "orderbook.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT_HEADER_SIZE 24u
#define SNAPSHOT_RECORD_SIZE 38u

static const unsigned char snapshot_magic[8] = {'I', 'T', 'C', 'H', 'Y', 'O', 'B', '2'};

struct PriceLevel {
    uint32_t price;
    uint64_t shares;
    size_t heap_index;
    UT_hash_handle hh;
};

static Heap *heap_for(OrderBook *book, char side)
{
    return side == 'B' ? &book->bids : &book->asks;
}

static const Heap *const_heap_for(const OrderBook *book, char side)
{
    return side == 'B' ? &book->bids : &book->asks;
}

static PriceLevel **levels_for(OrderBook *book, char side)
{
    return side == 'B' ? &book->bid_levels : &book->ask_levels;
}

static LevelHeap *level_heap_for(OrderBook *book, char side)
{
    return side == 'B' ? &book->level_bids : &book->level_asks;
}

static const LevelHeap *const_level_heap_for(const OrderBook *book, char side)
{
    return side == 'B' ? &book->level_bids : &book->level_asks;
}

static Order *find_order(const OrderBook *book, uint64_t order_id)
{
    Order *order = NULL;
    HASH_FIND(hh, book->orders, &order_id, sizeof(order_id), order);
    return order;
}

static void write_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void write_u64(unsigned char *p, uint64_t value)
{
    p[0] = (unsigned char)(value >> 56);
    p[1] = (unsigned char)(value >> 48);
    p[2] = (unsigned char)(value >> 40);
    p[3] = (unsigned char)(value >> 32);
    p[4] = (unsigned char)(value >> 24);
    p[5] = (unsigned char)(value >> 16);
    p[6] = (unsigned char)(value >> 8);
    p[7] = (unsigned char)value;
}

static uint32_t read_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t read_u64(const unsigned char *p)
{
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) |
           (uint64_t)p[7];
}

static uint64_t fnv1a64(const unsigned char *data, size_t len)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int compare_order_ptrs_by_id(const void *a, const void *b)
{
    const Order *oa = *(const Order * const *)a;
    const Order *ob = *(const Order * const *)b;

    if (oa->order_id < ob->order_id) {
        return -1;
    }
    if (oa->order_id > ob->order_id) {
        return 1;
    }
    return 0;
}

static PriceLevel *find_level(PriceLevel *levels, uint32_t price)
{
    PriceLevel *level = NULL;
    HASH_FIND(hh, levels, &price, sizeof(price), level);
    return level;
}

static int level_better(const LevelHeap *heap, const PriceLevel *a, const PriceLevel *b)
{
    if (a->price != b->price) {
        return heap->side == 'B' ? a->price > b->price : a->price < b->price;
    }
    return 0;
}

static void level_heap_swap(LevelHeap *heap, size_t a, size_t b)
{
    PriceLevel *tmp = heap->items[a];
    heap->items[a] = heap->items[b];
    heap->items[b] = tmp;
    heap->items[a]->heap_index = a;
    heap->items[b]->heap_index = b;
}

static void level_heap_sift_up(LevelHeap *heap, size_t index)
{
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!level_better(heap, heap->items[index], heap->items[parent])) {
            break;
        }
        level_heap_swap(heap, index, parent);
        index = parent;
    }
}

static void level_heap_sift_down(LevelHeap *heap, size_t index)
{
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t best = index;

        if (left < heap->size && level_better(heap, heap->items[left], heap->items[best])) {
            best = left;
        }
        if (right < heap->size && level_better(heap, heap->items[right], heap->items[best])) {
            best = right;
        }
        if (best == index) {
            break;
        }
        level_heap_swap(heap, index, best);
        index = best;
    }
}

static int level_heap_init(LevelHeap *heap, char side)
{
    heap->items = NULL;
    heap->size = 0;
    heap->capacity = 0;
    heap->side = side;
    return 0;
}

static void level_heap_free(LevelHeap *heap)
{
    free(heap->items);
    heap->items = NULL;
    heap->size = 0;
    heap->capacity = 0;
}

static int level_heap_insert(LevelHeap *heap, PriceLevel *level)
{
    if (heap->size == heap->capacity) {
        size_t next_capacity = heap->capacity == 0 ? 1024 : heap->capacity * 2;
        PriceLevel **next = realloc(heap->items, next_capacity * sizeof(*next));
        if (next == NULL) {
            return -1;
        }
        heap->items = next;
        heap->capacity = next_capacity;
    }

    level->heap_index = heap->size;
    heap->items[heap->size++] = level;
    level_heap_sift_up(heap, level->heap_index);
    return 0;
}

static void level_heap_remove_at(LevelHeap *heap, size_t index)
{
    if (index >= heap->size) {
        return;
    }

    heap->size--;
    if (index == heap->size) {
        return;
    }

    heap->items[index] = heap->items[heap->size];
    heap->items[index]->heap_index = index;
    level_heap_sift_up(heap, index);
    level_heap_sift_down(heap, heap->items[index]->heap_index);
}

static PriceLevel *level_heap_peek(const LevelHeap *heap)
{
    return heap->size == 0 ? NULL : heap->items[0];
}

static int add_level_shares(OrderBook *book, char side, uint32_t price, uint64_t shares)
{
    PriceLevel **levels = levels_for(book, side);
    LevelHeap *heap = level_heap_for(book, side);
    PriceLevel *level = find_level(*levels, price);

    if (shares == 0) {
        return 0;
    }

    if (level != NULL) {
        level->shares += shares;
        return 0;
    }

    level = calloc(1, sizeof(*level));
    if (level == NULL) {
        return -1;
    }
    level->price = price;
    level->shares = shares;

    if (level_heap_insert(heap, level) != 0) {
        free(level);
        return -1;
    }
    HASH_ADD(hh, *levels, price, sizeof(level->price), level);
    return 0;
}

static void remove_level_shares(OrderBook *book, char side, uint32_t price, uint64_t shares)
{
    PriceLevel **levels = levels_for(book, side);
    PriceLevel *level = find_level(*levels, price);

    if (level == NULL) {
        return;
    }

    if (shares < level->shares) {
        level->shares -= shares;
        return;
    }

    level_heap_remove_at(level_heap_for(book, side), level->heap_index);
    HASH_DEL(*levels, level);
    free(level);
}

static void unlink_order(OrderBook *book, Order *order)
{
    HASH_DEL(book->orders, order);
    book->order_count--;
}

static int add_order(OrderBook *book, Order *order)
{
    if (heap_insert(heap_for(book, order->side), order) != 0) {
        return -1;
    }
    if (add_level_shares(book, order->side, order->price, order->shares) != 0) {
        heap_remove_at(heap_for(book, order->side), order->heap_index);
        return -1;
    }

    HASH_ADD(hh, book->orders, order_id, sizeof(order->order_id), order);
    book->order_count++;
    return 1;
}

int orderbook_init(OrderBook *book)
{
    memset(book, 0, sizeof(*book));
    heap_init(&book->bids, HEAP_MAX);
    heap_init(&book->asks, HEAP_MIN);
    level_heap_init(&book->level_bids, 'B');
    level_heap_init(&book->level_asks, 'S');
    book->next_arrival_sequence = 1;
    return 0;
}

void orderbook_free(OrderBook *book)
{
    Order *order;
    Order *order_tmp;
    PriceLevel *level;
    PriceLevel *level_tmp;

    HASH_ITER(hh, book->orders, order, order_tmp) {
        HASH_DEL(book->orders, order);
        free(order);
    }
    HASH_ITER(hh, book->bid_levels, level, level_tmp) {
        HASH_DEL(book->bid_levels, level);
        free(level);
    }
    HASH_ITER(hh, book->ask_levels, level, level_tmp) {
        HASH_DEL(book->ask_levels, level);
        free(level);
    }

    heap_free(&book->bids);
    heap_free(&book->asks);
    level_heap_free(&book->level_bids);
    level_heap_free(&book->level_asks);
    memset(book, 0, sizeof(*book));
}

int orderbook_add(OrderBook *book, uint64_t order_id, char side, uint32_t shares,
                  const char *stock, uint32_t price)
{
    Order *order;

    if ((side != 'B' && side != 'S') || shares == 0 || find_order(book, order_id) != NULL) {
        return 0;
    }

    order = calloc(1, sizeof(*order));
    if (order == NULL) {
        return -1;
    }

    order->order_id = order_id;
    order->arrival_sequence = book->next_arrival_sequence++;
    order->side = side;
    order->shares = shares;
    order->price = price;
    strncpy(order->stock, stock, ITCHY_TICKER_LEN);
    order->stock[ITCHY_TICKER_LEN] = '\0';

    if (add_order(book, order) != 1) {
        free(order);
        return -1;
    }
    return 1;
}

int orderbook_execute(OrderBook *book, uint64_t order_id, uint32_t shares)
{
    Order *order = find_order(book, order_id);
    if (order == NULL || shares == 0) {
        return 0;
    }

    if ((uint64_t)shares >= order->shares) {
        return orderbook_delete(book, order_id);
    }

    order->shares -= shares;
    remove_level_shares(book, order->side, order->price, shares);
    return 1;
}

int orderbook_cancel(OrderBook *book, uint64_t order_id, uint32_t shares)
{
    return orderbook_execute(book, order_id, shares);
}

int orderbook_delete(OrderBook *book, uint64_t order_id)
{
    Order *order = find_order(book, order_id);
    if (order == NULL) {
        return 0;
    }

    remove_level_shares(book, order->side, order->price, order->shares);
    heap_remove_at(heap_for(book, order->side), order->heap_index);
    unlink_order(book, order);
    free(order);
    return 1;
}

int orderbook_replace(OrderBook *book, uint64_t old_order_id, uint64_t new_order_id,
                      uint32_t shares, uint32_t price)
{
    Order *old = find_order(book, old_order_id);
    char side;
    char stock[ITCHY_TICKER_LEN + 1];

    if (old == NULL) {
        return 0;
    }

    side = old->side;
    strncpy(stock, old->stock, sizeof(stock));
    stock[ITCHY_TICKER_LEN] = '\0';
    orderbook_delete(book, old_order_id);
    return orderbook_add(book, new_order_id, side, shares, stock, price);
}

int orderbook_upsert_level(OrderBook *book, uint64_t level_id, char side, uint32_t shares,
                           const char *stock, uint32_t price)
{
    Order *order = find_order(book, level_id);
    if (shares == 0) {
        return order == NULL ? 0 : orderbook_delete(book, level_id);
    }
    if (side != 'B' && side != 'S') {
        return 0;
    }

    if (order == NULL) {
        order = calloc(1, sizeof(*order));
        if (order == NULL) {
            return -1;
        }
        order->order_id = level_id;
        order->arrival_sequence = book->next_arrival_sequence++;
        order->side = side;
        order->shares = shares;
        order->price = price;
        strncpy(order->stock, stock, ITCHY_TICKER_LEN);
        order->stock[ITCHY_TICKER_LEN] = '\0';
        if (add_order(book, order) != 1) {
            free(order);
            return -1;
        }
        return 1;
    }

    if (order->side == side && order->price == price) {
        PriceLevel *level = find_level(*levels_for(book, side), price);
        if (level == NULL || level->shares < order->shares) {
            return -1;
        }
        level->shares = level->shares - order->shares + shares;
        order->shares = shares;
        strncpy(order->stock, stock, ITCHY_TICKER_LEN);
        order->stock[ITCHY_TICKER_LEN] = '\0';
        return 1;
    }

    remove_level_shares(book, order->side, order->price, order->shares);
    if (order->side != side) {
        heap_remove_at(heap_for(book, order->side), order->heap_index);
        order->side = side;
        if (heap_insert(heap_for(book, side), order) != 0) {
            unlink_order(book, order);
            free(order);
            return -1;
        }
    }

    order->shares = shares;
    order->price = price;
    strncpy(order->stock, stock, ITCHY_TICKER_LEN);
    order->stock[ITCHY_TICKER_LEN] = '\0';

    if (add_level_shares(book, side, price, shares) != 0) {
        return -1;
    }
    heap_fix_at(heap_for(book, order->side), order->heap_index);
    return 1;
}

static int crosses(char incoming_side, uint32_t incoming_price, const PriceLevel *opposite)
{
    if (opposite == NULL) {
        return 0;
    }
    return incoming_side == 'B'
        ? opposite->price <= incoming_price
        : opposite->price >= incoming_price;
}

int orderbook_match_limit(OrderBook *book, uint64_t order_id, char side, uint32_t shares,
                          const char *stock, uint32_t price, MatchReport *report)
{
    uint64_t remaining = shares;
    char opposite_side;

    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    if ((side != 'B' && side != 'S') || shares == 0 || find_order(book, order_id) != NULL) {
        return 0;
    }

    opposite_side = side == 'B' ? 'S' : 'B';
    while (remaining > 0) {
        PriceLevel *level = level_heap_peek(level_heap_for(book, opposite_side));
        Order *resting;
        uint64_t fill;

        if (!crosses(side, price, level)) {
            break;
        }

        resting = heap_peek(heap_for(book, opposite_side));
        if (resting == NULL || resting->price != level->price) {
            break;
        }

        fill = remaining < resting->shares ? remaining : resting->shares;
        if (orderbook_execute(book, resting->order_id, (uint32_t)fill) < 0) {
            return -1;
        }
        remaining -= fill;
        if (report != NULL) {
            report->executed_shares += fill;
            report->trades++;
        }
    }

    if (remaining > 0) {
        int rc = orderbook_add(book, order_id, side, (uint32_t)remaining, stock, price);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            return report != NULL && report->executed_shares > 0 ? 1 : 0;
        }
        if (report != NULL) {
            report->resting_shares = remaining;
        }
    }

    return 1;
}

int orderbook_apply(OrderBook *book, const OrderBookMessage *msg, MatchReport *report)
{
    if (msg == NULL) {
        return 0;
    }

    switch (msg->type) {
    case ORDERBOOK_MSG_ADD:
        return orderbook_add(book, msg->order_id, msg->side, msg->shares,
                             msg->stock, msg->price);
    case ORDERBOOK_MSG_EXECUTE:
        return orderbook_execute(book, msg->order_id, msg->shares);
    case ORDERBOOK_MSG_CANCEL:
        return orderbook_cancel(book, msg->order_id, msg->shares);
    case ORDERBOOK_MSG_DELETE:
        return orderbook_delete(book, msg->order_id);
    case ORDERBOOK_MSG_REPLACE:
        return orderbook_replace(book, msg->order_id, msg->new_order_id,
                                 msg->shares, msg->price);
    case ORDERBOOK_MSG_UPSERT_LEVEL:
        return orderbook_upsert_level(book, msg->order_id, msg->side, msg->shares,
                                      msg->stock, msg->price);
    case ORDERBOOK_MSG_MATCH_LIMIT:
        return orderbook_match_limit(book, msg->order_id, msg->side, msg->shares,
                                     msg->stock, msg->price, report);
    }
    return 0;
}

Order *orderbook_best_bid(const OrderBook *book)
{
    OrderBook *mutable_book = (OrderBook *)book;
    PriceLevel *level = level_heap_peek(const_level_heap_for(book, 'B'));

    if (level == NULL) {
        return NULL;
    }

    mutable_book->best_bid_view.order_id = 0;
    mutable_book->best_bid_view.side = 'B';
    mutable_book->best_bid_view.price = level->price;
    mutable_book->best_bid_view.shares = level->shares;
    mutable_book->best_bid_view.stock[0] = '\0';
    return &mutable_book->best_bid_view;
}

Order *orderbook_best_ask(const OrderBook *book)
{
    OrderBook *mutable_book = (OrderBook *)book;
    PriceLevel *level = level_heap_peek(const_level_heap_for(book, 'S'));

    if (level == NULL) {
        return NULL;
    }

    mutable_book->best_ask_view.order_id = 0;
    mutable_book->best_ask_view.side = 'S';
    mutable_book->best_ask_view.price = level->price;
    mutable_book->best_ask_view.shares = level->shares;
    mutable_book->best_ask_view.stock[0] = '\0';
    return &mutable_book->best_ask_view;
}

size_t orderbook_depth(const OrderBook *book, char side, BookLevel *levels, size_t max_levels)
{
    const LevelHeap *heap;
    BookLevel *scratch;
    size_t count;

    if ((side != 'B' && side != 'S') || levels == NULL || max_levels == 0) {
        return 0;
    }

    heap = const_level_heap_for(book, side);
    if (heap->size == 0) {
        return 0;
    }

    scratch = malloc(heap->size * sizeof(*scratch));
    if (scratch == NULL) {
        return 0;
    }

    for (size_t i = 0; i < heap->size; i++) {
        scratch[i].price = heap->items[i]->price;
        scratch[i].shares = heap->items[i]->shares;
    }

    for (size_t i = 1; i < heap->size; i++) {
        BookLevel key = scratch[i];
        size_t j = i;
        while (j > 0 && ((side == 'B' && key.price > scratch[j - 1].price) ||
                         (side == 'S' && key.price < scratch[j - 1].price))) {
            scratch[j] = scratch[j - 1];
            j--;
        }
        scratch[j] = key;
    }

    count = heap->size < max_levels ? heap->size : max_levels;
    memcpy(levels, scratch, count * sizeof(*levels));
    free(scratch);
    return count;
}

int orderbook_snapshot(const OrderBook *book, OrderBookSnapshot *snapshot)
{
    Order **orders = NULL;
    Order *order;
    Order *tmp;
    unsigned char *data;
    unsigned char *p;
    size_t count;
    size_t i = 0;
    size_t size;

    if (snapshot == NULL) {
        return -1;
    }
    snapshot->data = NULL;
    snapshot->size = 0;

    count = (size_t)HASH_COUNT(book->orders);
    if (count > (SIZE_MAX - SNAPSHOT_HEADER_SIZE) / SNAPSHOT_RECORD_SIZE) {
        return -1;
    }
    size = SNAPSHOT_HEADER_SIZE + count * SNAPSHOT_RECORD_SIZE;

    if (count > 0) {
        orders = malloc(count * sizeof(*orders));
        if (orders == NULL) {
            return -1;
        }
        HASH_ITER(hh, book->orders, order, tmp) {
            orders[i++] = order;
        }
        qsort(orders, count, sizeof(*orders), compare_order_ptrs_by_id);
    }

    data = malloc(size);
    if (data == NULL) {
        free(orders);
        return -1;
    }

    memcpy(data, snapshot_magic, sizeof(snapshot_magic));
    write_u64(data + 8, (uint64_t)count);
    write_u64(data + 16, book->next_arrival_sequence);
    p = data + SNAPSHOT_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        order = orders[i];
        write_u64(p, order->order_id);
        write_u64(p + 8, order->arrival_sequence);
        write_u64(p + 16, order->shares);
        write_u32(p + 24, order->price);
        p[28] = (unsigned char)order->side;
        memset(p + 29, 0, ITCHY_TICKER_LEN + 1);
        memcpy(p + 29, order->stock, ITCHY_TICKER_LEN);
        p += SNAPSHOT_RECORD_SIZE;
    }

    free(orders);
    snapshot->data = data;
    snapshot->size = size;
    return 0;
}

void orderbook_snapshot_free(OrderBookSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->data);
    snapshot->data = NULL;
    snapshot->size = 0;
}

int orderbook_restore(OrderBook *book, const OrderBookSnapshot *snapshot)
{
    OrderBook restored;
    const unsigned char *p;
    uint64_t encoded_count;
    uint64_t next_arrival_sequence;
    size_t count;

    if (snapshot == NULL || snapshot->data == NULL || snapshot->size < SNAPSHOT_HEADER_SIZE ||
        memcmp(snapshot->data, snapshot_magic, sizeof(snapshot_magic)) != 0) {
        return -1;
    }

    encoded_count = read_u64(snapshot->data + 8);
    if (encoded_count > (uint64_t)((SIZE_MAX - SNAPSHOT_HEADER_SIZE) / SNAPSHOT_RECORD_SIZE)) {
        return -1;
    }
    count = (size_t)encoded_count;
    next_arrival_sequence = read_u64(snapshot->data + 16);
    if (next_arrival_sequence == 0) {
        return -1;
    }
    if (snapshot->size != SNAPSHOT_HEADER_SIZE + count * SNAPSHOT_RECORD_SIZE) {
        return -1;
    }

    if (orderbook_init(&restored) != 0) {
        return -1;
    }
    restored.next_arrival_sequence = next_arrival_sequence;

    p = snapshot->data + SNAPSHOT_HEADER_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t order_id = read_u64(p);
        uint64_t arrival_sequence = read_u64(p + 8);
        uint64_t shares = read_u64(p + 16);
        uint32_t price = read_u32(p + 24);
        char side = (char)p[28];
        char stock[ITCHY_TICKER_LEN + 1];
        Order *order;

        if (arrival_sequence == 0 || shares == 0 || shares > UINT32_MAX ||
            (side != 'B' && side != 'S')) {
            orderbook_free(&restored);
            return -1;
        }
        memcpy(stock, p + 29, ITCHY_TICKER_LEN);
        stock[ITCHY_TICKER_LEN] = '\0';

        order = calloc(1, sizeof(*order));
        if (order == NULL) {
            orderbook_free(&restored);
            return -1;
        }
        order->order_id = order_id;
        order->arrival_sequence = arrival_sequence;
        order->side = side;
        order->shares = shares;
        order->price = price;
        strncpy(order->stock, stock, ITCHY_TICKER_LEN);
        order->stock[ITCHY_TICKER_LEN] = '\0';
        if (add_order(&restored, order) != 1) {
            free(order);
            orderbook_free(&restored);
            return -1;
        }
        if (arrival_sequence >= restored.next_arrival_sequence) {
            orderbook_free(&restored);
            return -1;
        }
        p += SNAPSHOT_RECORD_SIZE;
    }

    orderbook_free(book);
    *book = restored;
    return 0;
}

int orderbook_state_hash(const OrderBook *book, uint64_t *hash)
{
    OrderBookSnapshot snapshot;

    if (hash == NULL || orderbook_snapshot(book, &snapshot) != 0) {
        return -1;
    }
    *hash = fnv1a64(snapshot.data, snapshot.size);
    orderbook_snapshot_free(&snapshot);
    return 0;
}

int orderbook_shadow_best_matches(const OrderBook *book)
{
    Order *shadow_bid = heap_peek(const_heap_for(book, 'B'));
    Order *shadow_ask = heap_peek(const_heap_for(book, 'S'));
    Order *bid = orderbook_best_bid(book);
    Order *ask = orderbook_best_ask(book);

    if ((shadow_bid == NULL) != (bid == NULL)) {
        return 0;
    }
    if ((shadow_ask == NULL) != (ask == NULL)) {
        return 0;
    }
    if (shadow_bid != NULL && shadow_bid->price != bid->price) {
        return 0;
    }
    if (shadow_ask != NULL && shadow_ask->price != ask->price) {
        return 0;
    }
    return 1;
}
