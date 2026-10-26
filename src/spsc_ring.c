#include "spsc_ring.h"

#include <stdlib.h>
#include <string.h>

int spsc_ring_init(SpscRing *ring, size_t capacity, size_t item_size)
{
    if (capacity < 2 || item_size == 0) {
        return -1;
    }

    ring->items = calloc(capacity, item_size);
    if (ring->items == NULL) {
        return -1;
    }

    ring->capacity = capacity;
    ring->item_size = item_size;
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    return 0;
}

void spsc_ring_free(SpscRing *ring)
{
    free(ring->items);
    ring->items = NULL;
    ring->capacity = 0;
    ring->item_size = 0;
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
}

int spsc_ring_push(SpscRing *ring, const void *item)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t next = (head + 1u) % ring->capacity;
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (next == tail) {
        return 0;
    }

    memcpy(ring->items + head * ring->item_size, item, ring->item_size);
    atomic_store_explicit(&ring->head, next, memory_order_release);
    return 1;
}

int spsc_ring_pop(SpscRing *ring, void *item)
{
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head) {
        return 0;
    }

    memcpy(item, ring->items + tail * ring->item_size, ring->item_size);
    atomic_store_explicit(&ring->tail, (tail + 1u) % ring->capacity, memory_order_release);
    return 1;
}

size_t spsc_ring_count(const SpscRing *ring)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (head >= tail) {
        return head - tail;
    }
    return ring->capacity - tail + head;
}
