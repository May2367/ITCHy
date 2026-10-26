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

size_t spsc_ring_push_batch(SpscRing *ring, const void *items, size_t count)
{
    const unsigned char *src = items;
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    size_t used;
    size_t available;
    size_t first;
    size_t pushed;

    if (count == 0) {
        return 0;
    }

    used = head >= tail ? head - tail : ring->capacity - tail + head;
    available = ring->capacity - used - 1u;
    if (available == 0) {
        return 0;
    }

    pushed = count < available ? count : available;
    first = ring->capacity - head;
    if (first > pushed) {
        first = pushed;
    }

    memcpy(ring->items + head * ring->item_size, src, first * ring->item_size);
    if (pushed > first) {
        memcpy(ring->items, src + first * ring->item_size,
               (pushed - first) * ring->item_size);
    }

    atomic_store_explicit(&ring->head, (head + pushed) % ring->capacity,
                          memory_order_release);
    return pushed;
}

size_t spsc_ring_pop_batch(SpscRing *ring, void *items, size_t count)
{
    unsigned char *dest = items;
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t available;
    size_t first;
    size_t popped;

    if (count == 0) {
        return 0;
    }

    available = head >= tail ? head - tail : ring->capacity - tail + head;
    if (available == 0) {
        return 0;
    }

    popped = count < available ? count : available;
    first = ring->capacity - tail;
    if (first > popped) {
        first = popped;
    }

    memcpy(dest, ring->items + tail * ring->item_size, first * ring->item_size);
    if (popped > first) {
        memcpy(dest + first * ring->item_size, ring->items,
               (popped - first) * ring->item_size);
    }

    atomic_store_explicit(&ring->tail, (tail + popped) % ring->capacity,
                          memory_order_release);
    return popped;
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
