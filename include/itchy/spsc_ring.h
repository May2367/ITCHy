#ifndef ITCHY_SPSC_RING_H
#define ITCHY_SPSC_RING_H

#include <stddef.h>
#include <stdatomic.h>

typedef struct {
    unsigned char *items;
    size_t capacity;
    size_t item_size;
    atomic_size_t head;
    atomic_size_t tail;
} SpscRing;

int spsc_ring_init(SpscRing *ring, size_t capacity, size_t item_size);
void spsc_ring_free(SpscRing *ring);
int spsc_ring_push(SpscRing *ring, const void *item);
int spsc_ring_pop(SpscRing *ring, void *item);
size_t spsc_ring_count(const SpscRing *ring);

#endif
