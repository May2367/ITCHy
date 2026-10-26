#include "heap.h"
#include "orderbook.h"
#include "parser.h"
#include "publisher.h"
#include "spsc_ring.h"
#include "types.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint64_t callbacks;
    uint64_t last_timestamp;
    uint32_t last_bid_price;
    uint64_t last_bid_shares;
    uint32_t last_ask_price;
    uint64_t last_ask_shares;
} ParserEvents;

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put_u48(unsigned char *p, uint64_t v)
{
    p[0] = (unsigned char)(v >> 40);
    p[1] = (unsigned char)(v >> 32);
    p[2] = (unsigned char)(v >> 24);
    p[3] = (unsigned char)(v >> 16);
    p[4] = (unsigned char)(v >> 8);
    p[5] = (unsigned char)v;
}

static void put_u64(unsigned char *p, uint64_t v)
{
    p[0] = (unsigned char)(v >> 56);
    p[1] = (unsigned char)(v >> 48);
    p[2] = (unsigned char)(v >> 40);
    p[3] = (unsigned char)(v >> 32);
    p[4] = (unsigned char)(v >> 24);
    p[5] = (unsigned char)(v >> 16);
    p[6] = (unsigned char)(v >> 8);
    p[7] = (unsigned char)v;
}

static void put_stock(unsigned char *p, const char *stock)
{
    size_t len = strlen(stock);
    memset(p, ' ', ITCHY_TICKER_LEN);
    if (len > ITCHY_TICKER_LEN) {
        len = ITCHY_TICKER_LEN;
    }
    memcpy(p, stock, len);
}

static void write_record(gzFile f, const unsigned char *msg, uint16_t len)
{
    unsigned char len_buf[2];
    put_u16(len_buf, len);
    CHECK(gzwrite(f, len_buf, sizeof(len_buf)) == (int)sizeof(len_buf));
    CHECK(gzwrite(f, msg, len) == len);
}

static void make_add(unsigned char *msg, uint64_t timestamp, uint64_t order_id,
                     char side, uint32_t shares, const char *stock, uint32_t price)
{
    unsigned char *p;
    memset(msg, 0, 34);
    msg[0] = 'A';
    put_u48(msg + 3, timestamp);
    p = msg + 9;
    put_u64(p, order_id);
    p[8] = (unsigned char)side;
    put_u32(p + 9, shares);
    put_stock(p + 13, stock);
    put_u32(p + 21, price);
}

static void make_add_mpid(unsigned char *msg, uint64_t timestamp, uint64_t order_id,
                          char side, uint32_t shares, const char *stock,
                          uint32_t price, const char *mpid)
{
    make_add(msg, timestamp, order_id, side, shares, stock, price);
    msg[0] = 'F';
    memcpy(msg + 34, mpid, 4);
}

static void make_execute(unsigned char *msg, uint64_t timestamp, uint64_t order_id,
                         uint32_t shares)
{
    memset(msg, 0, 29);
    msg[0] = 'E';
    put_u48(msg + 3, timestamp);
    put_u64(msg + 9, order_id);
    put_u32(msg + 17, shares);
}

static void make_execute_price(unsigned char *msg, uint64_t timestamp, uint64_t order_id,
                               uint32_t shares, uint32_t price)
{
    memset(msg, 0, 34);
    msg[0] = 'C';
    put_u48(msg + 3, timestamp);
    put_u64(msg + 9, order_id);
    put_u32(msg + 17, shares);
    put_u32(msg + 30, price);
}

static void make_cancel(unsigned char *msg, uint64_t timestamp, uint64_t order_id,
                        uint32_t shares)
{
    memset(msg, 0, 21);
    msg[0] = 'X';
    put_u48(msg + 3, timestamp);
    put_u64(msg + 9, order_id);
    put_u32(msg + 17, shares);
}

static void make_delete(unsigned char *msg, uint64_t timestamp, uint64_t order_id)
{
    memset(msg, 0, 17);
    msg[0] = 'D';
    put_u48(msg + 3, timestamp);
    put_u64(msg + 9, order_id);
}

static void make_replace(unsigned char *msg, uint64_t timestamp, uint64_t old_order_id,
                         uint64_t new_order_id, uint32_t shares, uint32_t price)
{
    memset(msg, 0, 33);
    msg[0] = 'U';
    put_u48(msg + 3, timestamp);
    put_u64(msg + 9, old_order_id);
    put_u64(msg + 17, new_order_id);
    put_u32(msg + 25, shares);
    put_u32(msg + 29, price);
}

static void make_tvagg_u(unsigned char *msg, uint64_t timestamp, char side,
                         uint32_t shares, const char *stock, uint32_t price)
{
    memset(msg, 0, 34);
    msg[0] = 'U';
    put_u48(msg + 3, timestamp);
    msg[9] = (unsigned char)side;
    put_u32(msg + 10, shares);
    put_u32(msg + 14, shares);
    put_stock(msg + 18, stock);
    put_u32(msg + 26, price);
    memcpy(msg + 30, "NSDQ", 4);
}

static void on_book(uint64_t timestamp, const Order *bid, const Order *ask, void *user_data)
{
    ParserEvents *events = user_data;
    events->callbacks++;
    events->last_timestamp = timestamp;
    events->last_bid_price = bid == NULL ? 0 : bid->price;
    events->last_bid_shares = bid == NULL ? 0 : bid->shares;
    events->last_ask_price = ask == NULL ? 0 : ask->price;
    events->last_ask_shares = ask == NULL ? 0 : ask->shares;
}

static ParseResult run_fixture(const char *path, const char *ticker, ParserEvents *events)
{
    Config config = {0};
    ParserRunOptions options = {0};
    ParseResult result;
    gzFile in;

    if (ticker != NULL) {
        strcpy(config.ticker, ticker);
    }
    config.update_interval_ns = 0;
    if (events != NULL) {
        options.book_callback = on_book;
        options.book_user_data = events;
    }

    in = gzopen(path, "rb");
    CHECK(in != NULL);
    result = parser_run_stream(in, &config, &options);
    CHECK(gzclose(in) == Z_OK);
    return result;
}


static void test_spsc_ring(void)
{
    SpscRing ring;
    int value;
    int out;

    CHECK(spsc_ring_init(&ring, 4, sizeof(value)) == 0);
    CHECK(spsc_ring_count(&ring) == 0);

    value = 10;
    CHECK(spsc_ring_push(&ring, &value) == 1);
    value = 20;
    CHECK(spsc_ring_push(&ring, &value) == 1);
    value = 30;
    CHECK(spsc_ring_push(&ring, &value) == 1);
    value = 40;
    CHECK(spsc_ring_push(&ring, &value) == 0);
    CHECK(spsc_ring_count(&ring) == 3);

    CHECK(spsc_ring_pop(&ring, &out) == 1 && out == 10);
    CHECK(spsc_ring_pop(&ring, &out) == 1 && out == 20);
    value = 40;
    CHECK(spsc_ring_push(&ring, &value) == 1);
    CHECK(spsc_ring_pop(&ring, &out) == 1 && out == 30);
    CHECK(spsc_ring_pop(&ring, &out) == 1 && out == 40);
    CHECK(spsc_ring_pop(&ring, &out) == 0);

    spsc_ring_free(&ring);
}

static void test_event_publisher(void)
{
    const char *path = "tests/tmp_events.csv";
    EventPublisher publisher;
    EventPublisherStats stats;
    Order bid = {.price = 100000, .shares = 50};
    Order ask = {.price = 100500, .shares = 70};
    FILE *in;
    char line[256];
    int lines = 0;

    CHECK(event_publisher_start_file(&publisher, path, 8) == 0);
    CHECK(event_publisher_publish(&publisher, 100, &bid, &ask) == 1);
    CHECK(event_publisher_publish(&publisher, 200, &bid, NULL) == 1);
    CHECK(event_publisher_publish(&publisher, 300, NULL, &ask) == 1);
    CHECK(event_publisher_stop(&publisher) == 0);

    event_publisher_stats(&publisher, &stats);
    CHECK(stats.enqueued == 3);
    CHECK(stats.dropped == 0);
    CHECK(stats.written == 3);
    CHECK(stats.write_error == 0);

    in = fopen(path, "r");
    CHECK(in != NULL);
    while (fgets(line, sizeof(line), in) != NULL) {
        lines++;
    }
    CHECK(fclose(in) == 0);
    CHECK(lines == 4);
}

static void test_utils(void)
{
    unsigned char buf[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    char out[9];
    char price[32];

    CHECK(read_uint16_be(buf) == 0x0123u);
    CHECK(read_uint32_be(buf) == 0x01234567u);
    CHECK(read_uint48_be(buf) == 0x0123456789abull);
    CHECK(read_uint64_be(buf) == 0x0123456789abcdefull);
    copy_itch_string(out, sizeof(out), (const unsigned char *)"AMAT    ", 8);
    CHECK(strcmp(out, "AMAT") == 0);
    CHECK(ticker_matches("", "AMAT"));
    CHECK(ticker_matches("AMAT", "AMAT"));
    CHECK(!ticker_matches("MSFT", "AMAT"));
    format_price(price, sizeof(price), 1234567u);
    CHECK(strcmp(price, "123.4567") == 0);
}

static void test_heap(void)
{
    Heap heap;
    Order a = {.order_id = 1, .price = 100};
    Order b = {.order_id = 2, .price = 105};
    Order c = {.order_id = 3, .price = 101};

    CHECK(heap_init(&heap, HEAP_MAX) == 0);
    CHECK(heap_insert(&heap, &a) == 0);
    CHECK(heap_insert(&heap, &b) == 0);
    CHECK(heap_insert(&heap, &c) == 0);
    CHECK(heap_peek(&heap) == &b);
    heap_remove_at(&heap, b.heap_index);
    CHECK(heap_peek(&heap) == &c);
    c.price = 99;
    heap_fix_at(&heap, c.heap_index);
    CHECK(heap_peek(&heap) == &a);
    heap_free(&heap);
}

static void check_shadow(const OrderBook *book)
{
    CHECK(orderbook_shadow_best_matches(book));
}

static void test_orderbook(void)
{
    OrderBook book;
    Order *bid;
    Order *ask;
    BookLevel levels[4];

    CHECK(orderbook_init(&book) == 0);
    CHECK(orderbook_add(&book, 1001, 'B', 100, "AMAT", 100000) == 1);
    check_shadow(&book);
    CHECK(orderbook_add(&book, 1002, 'B', 60, "AMAT", 100000) == 1);
    check_shadow(&book);
    CHECK(orderbook_add(&book, 1003, 'B', 30, "AMAT", 99900) == 1);
    CHECK(orderbook_add(&book, 2001, 'S', 50, "AMAT", 100500) == 1);
    CHECK(orderbook_add(&book, 2002, 'S', 80, "AMAT", 100400) == 1);
    check_shadow(&book);

    bid = orderbook_best_bid(&book);
    ask = orderbook_best_ask(&book);
    CHECK(bid != NULL && bid->price == 100000 && bid->shares == 160);
    CHECK(ask != NULL && ask->price == 100400 && ask->shares == 80);

    CHECK(orderbook_depth(&book, 'B', levels, 4) == 2);
    CHECK(levels[0].price == 100000 && levels[0].shares == 160);
    CHECK(levels[1].price == 99900 && levels[1].shares == 30);
    CHECK(orderbook_depth(&book, 'S', levels, 4) == 2);
    CHECK(levels[0].price == 100400 && levels[0].shares == 80);
    CHECK(levels[1].price == 100500 && levels[1].shares == 50);

    CHECK(orderbook_cancel(&book, 1001, 25) == 1);
    CHECK(orderbook_best_bid(&book)->shares == 135);
    CHECK(orderbook_cancel(&book, 1002, 60) == 1);
    CHECK(orderbook_best_bid(&book)->shares == 75);
    check_shadow(&book);

    CHECK(orderbook_replace(&book, 2001, 3001, 20, 100300) == 1);
    CHECK(orderbook_best_ask(&book)->price == 100300);
    CHECK(orderbook_best_ask(&book)->shares == 20);
    CHECK(orderbook_upsert_level(&book, 4001, 'S', 120, "AMAT", 100400) == 1);
    CHECK(orderbook_depth(&book, 'S', levels, 2) == 2);
    CHECK(levels[0].price == 100300 && levels[0].shares == 20);
    CHECK(levels[1].price == 100400 && levels[1].shares == 200);
    check_shadow(&book);

    CHECK(orderbook_delete(&book, 3001) == 1);
    CHECK(orderbook_best_ask(&book)->price == 100400);
    CHECK(orderbook_best_ask(&book)->shares == 200);
    CHECK(orderbook_execute(&book, 1001, 75) == 1);
    CHECK(orderbook_best_bid(&book)->price == 99900);
    CHECK(orderbook_best_bid(&book)->shares == 30);
    check_shadow(&book);

    orderbook_free(&book);
}

static void test_orderbook_shadow_sequence(void)
{
    OrderBook book;

    CHECK(orderbook_init(&book) == 0);
    for (uint64_t i = 1; i <= 200; i++) {
        char side = (i % 3) == 0 ? 'S' : 'B';
        uint32_t price = side == 'B' ? 100000u + (uint32_t)(i % 17) * 10u
                                     : 100500u + (uint32_t)(i % 19) * 10u;
        CHECK(orderbook_add(&book, i, side, (uint32_t)(10 + i % 11), "AMAT", price) == 1);
        check_shadow(&book);
    }
    for (uint64_t i = 1; i <= 200; i += 4) {
        CHECK(orderbook_cancel(&book, i, 3) == 1);
        check_shadow(&book);
    }
    for (uint64_t i = 2; i <= 200; i += 5) {
        CHECK(orderbook_delete(&book, i) == 1);
        check_shadow(&book);
    }
    for (uint64_t i = 3; i <= 200; i += 7) {
        uint64_t new_id = 1000 + i;
        uint32_t price = 100100u + (uint32_t)(i % 23) * 10u;
        int rc = orderbook_replace(&book, i, new_id, 25, price);
        CHECK(rc == 0 || rc == 1);
        check_shadow(&book);
    }
    orderbook_free(&book);
}


static void test_matching_engine(void)
{
    OrderBook book;
    MatchReport report;
    Order *bid;
    Order *ask;
    BookLevel levels[3];

    CHECK(orderbook_init(&book) == 0);
    CHECK(orderbook_add(&book, 10, 'S', 40, "AMAT", 100000) == 1);
    CHECK(orderbook_add(&book, 5, 'S', 25, "AMAT", 100000) == 1);
    CHECK(orderbook_add(&book, 20, 'S', 50, "AMAT", 100100) == 1);

    CHECK(orderbook_match_limit(&book, 1000, 'B', 50, "AMAT", 100000, &report) == 1);
    CHECK(report.executed_shares == 50);
    CHECK(report.resting_shares == 0);
    CHECK(report.trades == 2);
    ask = orderbook_best_ask(&book);
    CHECK(ask != NULL && ask->price == 100000 && ask->shares == 15);

    CHECK(orderbook_match_limit(&book, 1001, 'B', 100, "AMAT", 100100, &report) == 1);
    CHECK(report.executed_shares == 65);
    CHECK(report.resting_shares == 35);
    CHECK(report.trades == 2);
    CHECK(orderbook_best_ask(&book) == NULL);
    bid = orderbook_best_bid(&book);
    CHECK(bid != NULL && bid->price == 100100 && bid->shares == 35);

    CHECK(orderbook_match_limit(&book, 2000, 'S', 20, "AMAT", 100100, &report) == 1);
    CHECK(report.executed_shares == 20);
    CHECK(report.resting_shares == 0);
    CHECK(report.trades == 1);
    bid = orderbook_best_bid(&book);
    CHECK(bid != NULL && bid->price == 100100 && bid->shares == 15);

    CHECK(orderbook_match_limit(&book, 2001, 'S', 10, "AMAT", 100200, &report) == 1);
    CHECK(report.executed_shares == 0);
    CHECK(report.resting_shares == 10);
    CHECK(report.trades == 0);
    CHECK(orderbook_depth(&book, 'S', levels, 3) == 1);
    CHECK(levels[0].price == 100200 && levels[0].shares == 10);
    check_shadow(&book);

    orderbook_free(&book);
}

static void test_orderbook_state_machine(void)
{
    OrderBook book;
    OrderBook replay;
    OrderBook restored;
    OrderBook same_a;
    OrderBook same_b;
    OrderBookSnapshot snapshot = {0};
    MatchReport report;
    BookLevel levels[4];
    uint64_t hash_before;
    uint64_t hash_replay;
    uint64_t hash_restored;
    uint64_t hash_mutated;
    uint64_t hash_same_a;
    uint64_t hash_same_b;
    OrderBookMessage msgs[] = {
        {.type = ORDERBOOK_MSG_ADD, .order_id = 1, .side = 'B', .shares = 100, .price = 100000, .stock = "AMAT"},
        {.type = ORDERBOOK_MSG_ADD, .order_id = 2, .side = 'S', .shares = 80, .price = 100500, .stock = "AMAT"},
        {.type = ORDERBOOK_MSG_CANCEL, .order_id = 1, .shares = 25},
        {.type = ORDERBOOK_MSG_REPLACE, .order_id = 2, .new_order_id = 3, .shares = 40, .price = 100400},
        {.type = ORDERBOOK_MSG_UPSERT_LEVEL, .order_id = 900, .side = 'S', .shares = 70, .price = 100300, .stock = "AMAT"},
        {.type = ORDERBOOK_MSG_MATCH_LIMIT, .order_id = 4, .side = 'B', .shares = 30, .price = 100300, .stock = "AMAT"}
    };

    CHECK(orderbook_init(&book) == 0);
    CHECK(orderbook_init(&replay) == 0);
    CHECK(orderbook_init(&restored) == 0);
    CHECK(orderbook_init(&same_a) == 0);
    CHECK(orderbook_init(&same_b) == 0);

    for (size_t i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        CHECK(orderbook_apply(&book, &msgs[i], &report) == 1);
        CHECK(orderbook_apply(&replay, &msgs[i], &report) == 1);
    }
    CHECK(report.executed_shares == 30);
    CHECK(report.resting_shares == 0);
    CHECK(report.trades == 1);
    CHECK(orderbook_shadow_best_matches(&book));

    CHECK(orderbook_state_hash(&book, &hash_before) == 0);
    CHECK(orderbook_state_hash(&replay, &hash_replay) == 0);
    CHECK(hash_before == hash_replay);

    CHECK(orderbook_snapshot(&book, &snapshot) == 0);
    CHECK(snapshot.data != NULL && snapshot.size > 0);
    CHECK(orderbook_restore(&restored, &snapshot) == 0);
    CHECK(orderbook_state_hash(&restored, &hash_restored) == 0);
    CHECK(hash_before == hash_restored);
    CHECK(orderbook_best_bid(&restored)->price == 100000);
    CHECK(orderbook_best_bid(&restored)->shares == 75);
    CHECK(orderbook_depth(&restored, 'S', levels, 4) == 2);
    CHECK(levels[0].price == 100300 && levels[0].shares == 40);
    CHECK(levels[1].price == 100400 && levels[1].shares == 40);

    CHECK(orderbook_delete(&restored, 1) == 1);
    CHECK(orderbook_state_hash(&restored, &hash_mutated) == 0);
    CHECK(hash_mutated != hash_before);
    CHECK(orderbook_restore(&restored, &snapshot) == 0);
    CHECK(orderbook_state_hash(&restored, &hash_restored) == 0);
    CHECK(hash_restored == hash_before);

    CHECK(orderbook_add(&same_a, 10, 'B', 10, "AMAT", 100000) == 1);
    CHECK(orderbook_add(&same_a, 2, 'S', 20, "AMAT", 100500) == 1);
    CHECK(orderbook_add(&same_b, 2, 'S', 20, "AMAT", 100500) == 1);
    CHECK(orderbook_add(&same_b, 10, 'B', 10, "AMAT", 100000) == 1);
    CHECK(orderbook_state_hash(&same_a, &hash_same_a) == 0);
    CHECK(orderbook_state_hash(&same_b, &hash_same_b) == 0);
    CHECK(hash_same_a == hash_same_b);

    orderbook_snapshot_free(&snapshot);
    orderbook_free(&same_b);
    orderbook_free(&same_a);
    orderbook_free(&restored);
    orderbook_free(&replay);
    orderbook_free(&book);
}

static void test_parser_fixture(void)
{
    const char *path = "tests/tmp_parser_fixture.gz";
    ParserEvents events = {0};
    ParseResult result;
    gzFile out;
    unsigned char msg[64];

    out = gzopen(path, "wb");
    CHECK(out != NULL);
    make_add(msg, 100, 1001, 'B', 100, "AMAT", 100000);
    write_record(out, msg, 34);
    make_add(msg, 200, 1002, 'S', 50, "AMAT", 100500);
    write_record(out, msg, 34);
    make_cancel(msg, 300, 1001, 25);
    write_record(out, msg, 21);
    make_delete(msg, 400, 1002);
    write_record(out, msg, 17);
    make_tvagg_u(msg, 500, 'S', 200, "AMAT", 100800);
    write_record(out, msg, 34);
    memset(msg, 0, 10);
    msg[0] = 'S';
    put_u48(msg + 3, 600);
    write_record(out, msg, 10);
    msg[0] = 'Z';
    write_record(out, msg, 1);
    CHECK(gzclose(out) == Z_OK);

    result = run_fixture(path, "AMAT", &events);

    CHECK(result.io_error == 0);
    CHECK(result.messages_read == 7);
    CHECK(result.messages_applied == 5);
    CHECK(result.messages_ignored == 1);
    CHECK(result.unknown_messages == 1);
    CHECK(result.malformed_messages == 0);
    CHECK(result.last_timestamp == 600);
    CHECK(events.callbacks == 5);
    CHECK(events.last_timestamp == 500);
    CHECK(events.last_bid_price == 100000);
    CHECK(events.last_bid_shares == 75);
    CHECK(events.last_ask_price == 100800);
    CHECK(events.last_ask_shares == 200);
}

static void test_parser_standard_itch_messages(void)
{
    const char *path = "tests/tmp_parser_standard.gz";
    ParserEvents events = {0};
    ParseResult result;
    gzFile out;
    unsigned char msg[64];

    out = gzopen(path, "wb");
    CHECK(out != NULL);
    make_add_mpid(msg, 100, 2001, 'B', 100, "AMAT", 100000, "ABCD");
    write_record(out, msg, 38);
    make_add(msg, 110, 9999, 'S', 75, "MSFT", 100200);
    write_record(out, msg, 34);
    make_add(msg, 120, 2002, 'S', 80, "AMAT", 100500);
    write_record(out, msg, 34);
    make_execute(msg, 130, 2001, 40);
    write_record(out, msg, 29);
    make_execute_price(msg, 140, 2002, 30, 100400);
    write_record(out, msg, 34);
    make_replace(msg, 150, 2002, 3002, 25, 100300);
    write_record(out, msg, 33);
    make_execute(msg, 160, 2001, 60);
    write_record(out, msg, 29);
    CHECK(gzclose(out) == Z_OK);

    result = run_fixture(path, "AMAT", &events);

    CHECK(result.io_error == 0);
    CHECK(result.messages_read == 7);
    CHECK(result.messages_applied == 6);
    CHECK(result.messages_ignored == 1);
    CHECK(result.unknown_messages == 0);
    CHECK(result.malformed_messages == 0);
    CHECK(result.last_timestamp == 160);
    CHECK(events.callbacks == 6);
    CHECK(events.last_timestamp == 160);
    CHECK(events.last_bid_price == 0);
    CHECK(events.last_ask_price == 100300);
    CHECK(events.last_ask_shares == 25);
}

static void test_parser_aggregate_u_edges(void)
{
    const char *path = "tests/tmp_parser_tvagg_edges.gz";
    ParserEvents events = {0};
    ParseResult result;
    gzFile out;
    unsigned char msg[64];

    out = gzopen(path, "wb");
    CHECK(out != NULL);
    make_tvagg_u(msg, 100, 'S', 200, "AMAT", 100800);
    write_record(out, msg, 34);
    make_tvagg_u(msg, 110, 'S', 125, "AMAT", 100800);
    write_record(out, msg, 34);
    make_tvagg_u(msg, 120, 'B', 50, "MSFT", 100100);
    write_record(out, msg, 34);
    make_tvagg_u(msg, 130, 'S', 0, "AMAT", 100800);
    write_record(out, msg, 34);
    make_tvagg_u(msg, 140, 'B', 90, "AMAT", 100200);
    write_record(out, msg, 34);
    make_tvagg_u(msg, 150, 'X', 90, "AMAT", 100300);
    write_record(out, msg, 34);
    CHECK(gzclose(out) == Z_OK);

    result = run_fixture(path, "AMAT", &events);

    CHECK(result.io_error == 0);
    CHECK(result.messages_read == 6);
    CHECK(result.messages_applied == 4);
    CHECK(result.messages_ignored == 2);
    CHECK(result.unknown_messages == 0);
    CHECK(result.malformed_messages == 0);
    CHECK(result.last_timestamp == 150);
    CHECK(events.callbacks == 4);
    CHECK(events.last_timestamp == 140);
    CHECK(events.last_bid_price == 100200);
    CHECK(events.last_bid_shares == 90);
    CHECK(events.last_ask_price == 0);
}

static void test_parser_bad_records(void)
{
    ParseResult result;
    gzFile out;
    unsigned char msg[8] = {0};
    unsigned char len_buf[2];

    out = gzopen("tests/tmp_parser_bad_len.gz", "wb");
    CHECK(out != NULL);
    put_u16(len_buf, 0);
    CHECK(gzwrite(out, len_buf, sizeof(len_buf)) == (int)sizeof(len_buf));
    CHECK(gzclose(out) == Z_OK);
    result = run_fixture("tests/tmp_parser_bad_len.gz", "AMAT", NULL);
    CHECK(result.io_error != 0);
    CHECK(result.messages_read == 0);
    CHECK(result.malformed_messages == 1);

    out = gzopen("tests/tmp_parser_truncated.gz", "wb");
    CHECK(out != NULL);
    put_u16(len_buf, 34);
    CHECK(gzwrite(out, len_buf, sizeof(len_buf)) == (int)sizeof(len_buf));
    CHECK(gzwrite(out, msg, 8) == 8);
    CHECK(gzclose(out) == Z_OK);
    result = run_fixture("tests/tmp_parser_truncated.gz", "AMAT", NULL);
    CHECK(result.io_error != 0);
    CHECK(result.messages_read == 0);
    CHECK(result.malformed_messages == 0);
}

int main(void)
{
    test_spsc_ring();
    test_event_publisher();
    test_utils();
    test_heap();
    test_orderbook();
    test_orderbook_shadow_sequence();
    test_matching_engine();
    test_orderbook_state_machine();
    test_parser_fixture();
    test_parser_standard_itch_messages();
    test_parser_aggregate_u_edges();
    test_parser_bad_records();
    puts("all tests passed");
    return 0;
}
