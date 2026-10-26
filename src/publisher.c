#define _POSIX_C_SOURCE 200809L

#include "publisher.h"

#include "utils.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t sequence;
    uint64_t timestamp;
    uint64_t bid_shares;
    uint64_t ask_shares;
    uint32_t bid_price;
    uint32_t ask_price;
    unsigned bid_present;
    unsigned ask_present;
    unsigned stop;
} BookEvent;

struct EventSink {
    void *ctx;
    int (*write)(void *ctx, const BookEvent *event);
    int (*close)(void *ctx);
};

typedef struct {
    FILE *output;
} FileSink;

static int file_sink_write(void *ctx, const BookEvent *event)
{
    FileSink *sink = ctx;
    char bid_price[32] = "";
    char ask_price[32] = "";
    char spread[32] = "";

    if (event->bid_present) {
        format_price(bid_price, sizeof(bid_price), event->bid_price);
    }
    if (event->ask_present) {
        format_price(ask_price, sizeof(ask_price), event->ask_price);
    }
    if (event->bid_present && event->ask_present && event->ask_price >= event->bid_price) {
        format_price(spread, sizeof(spread), event->ask_price - event->bid_price);
    }

    fprintf(sink->output, "%llu,%llu,%s,%llu,%s,%llu,%s\n",
            (unsigned long long)event->sequence,
            (unsigned long long)event->timestamp,
            bid_price, (unsigned long long)event->bid_shares,
            ask_price, (unsigned long long)event->ask_shares,
            spread);
    return ferror(sink->output) ? -1 : 0;
}

static int file_sink_close(void *ctx)
{
    FileSink *sink = ctx;
    int status = 0;

    if (sink->output != NULL) {
        if (fflush(sink->output) != 0) {
            status = -1;
        }
        if (fclose(sink->output) != 0) {
            status = -1;
        }
    }
    free(sink);
    return status;
}

static EventSink *file_sink_open(const char *path)
{
    EventSink *sink;
    FileSink *file;

    file = calloc(1, sizeof(*file));
    sink = calloc(1, sizeof(*sink));
    if (file == NULL || sink == NULL) {
        free(file);
        free(sink);
        return NULL;
    }

    file->output = fopen(path, "w");
    if (file->output == NULL) {
        free(file);
        free(sink);
        return NULL;
    }

    fprintf(file->output, "seq,timestamp_ns,best_bid,bid_shares,best_ask,ask_shares,spread\n");
    if (ferror(file->output)) {
        fclose(file->output);
        free(file);
        free(sink);
        return NULL;
    }

    sink->ctx = file;
    sink->write = file_sink_write;
    sink->close = file_sink_close;
    return sink;
}

static void *publisher_main(void *arg)
{
    EventPublisher *publisher = arg;
    int done = 0;

    while (!done) {
        BookEvent event;
        if (!spsc_ring_pop(&publisher->ring, &event)) {
            sched_yield();
            continue;
        }
        if (event.stop) {
            done = 1;
            continue;
        }

        if (publisher->sink->write(publisher->sink->ctx, &event) != 0) {
            publisher->write_error = 1;
            done = 1;
        } else {
            publisher->written++;
        }
    }

    return NULL;
}

static int event_publisher_start_sink(EventPublisher *publisher, EventSink *sink,
                                      size_t capacity)
{
    memset(publisher, 0, sizeof(*publisher));
    if (sink == NULL || capacity < 2) {
        return -1;
    }

    publisher->sink = sink;
    if (spsc_ring_init(&publisher->ring, capacity, sizeof(BookEvent)) != 0) {
        publisher->sink = NULL;
        return -1;
    }
    if (pthread_create(&publisher->thread, NULL, publisher_main, publisher) != 0) {
        spsc_ring_free(&publisher->ring);
        publisher->sink = NULL;
        return -1;
    }

    publisher->started = 1;
    return 0;
}

int event_publisher_start_file(EventPublisher *publisher, const char *path, size_t capacity)
{
    EventSink *sink;
    if (path == NULL) {
        return -1;
    }

    sink = file_sink_open(path);
    if (sink == NULL) {
        return -1;
    }
    if (event_publisher_start_sink(publisher, sink, capacity) != 0) {
        sink->close(sink->ctx);
        free(sink);
        return -1;
    }
    return 0;
}

int event_publisher_publish(EventPublisher *publisher, uint64_t timestamp,
                            const Order *bid, const Order *ask)
{
    BookEvent event;

    if (publisher == NULL || !publisher->started) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.sequence = publisher->next_sequence++;
    event.timestamp = timestamp;
    if (bid != NULL) {
        event.bid_present = 1;
        event.bid_price = bid->price;
        event.bid_shares = bid->shares;
    }
    if (ask != NULL) {
        event.ask_present = 1;
        event.ask_price = ask->price;
        event.ask_shares = ask->shares;
    }

    if (!spsc_ring_push(&publisher->ring, &event)) {
        publisher->dropped++;
        return 0;
    }
    publisher->enqueued++;
    return 1;
}

int event_publisher_stop(EventPublisher *publisher)
{
    BookEvent stop = {0};
    EventSink *sink;
    int status;

    if (publisher == NULL || !publisher->started) {
        return 0;
    }

    stop.stop = 1;
    while (!spsc_ring_push(&publisher->ring, &stop)) {
        sched_yield();
    }

    pthread_join(publisher->thread, NULL);
    status = publisher->write_error ? -1 : 0;
    spsc_ring_free(&publisher->ring);
    sink = publisher->sink;
    publisher->sink = NULL;
    if (sink->close(sink->ctx) != 0) {
        status = -1;
    }
    free(sink);
    publisher->started = 0;
    return status;
}

void event_publisher_stats(const EventPublisher *publisher, EventPublisherStats *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (publisher == NULL) {
        return;
    }
    stats->enqueued = publisher->enqueued;
    stats->dropped = publisher->dropped;
    stats->written = publisher->written;
    stats->next_sequence = publisher->next_sequence;
    stats->write_error = publisher->write_error;
}
