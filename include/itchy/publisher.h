#ifndef ITCHY_PUBLISHER_H
#define ITCHY_PUBLISHER_H

#include "spsc_ring.h"
#include "types.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t enqueued;
    uint64_t dropped;
    uint64_t written;
    int write_error;
} EventPublisherStats;

typedef struct {
    SpscRing ring;
    FILE *output;
    pthread_t thread;
    uint64_t enqueued;
    uint64_t dropped;
    uint64_t written;
    int write_error;
    int started;
} EventPublisher;

int event_publisher_start(EventPublisher *publisher, const char *path, size_t capacity);
int event_publisher_publish(EventPublisher *publisher, uint64_t timestamp,
                            const Order *bid, const Order *ask);
int event_publisher_stop(EventPublisher *publisher);
void event_publisher_stats(const EventPublisher *publisher, EventPublisherStats *stats);

#endif
