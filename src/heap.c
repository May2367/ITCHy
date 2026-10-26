#include "heap.h"

#include <stdlib.h>

static int heap_better(const Heap *heap, const Order *a, const Order *b)
{
    if (a->price != b->price) {
        return heap->kind == HEAP_MAX ? a->price > b->price : a->price < b->price;
    }
    return a->order_id < b->order_id;
}

static void heap_swap(Heap *heap, size_t a, size_t b)
{
    Order *tmp = heap->items[a];
    heap->items[a] = heap->items[b];
    heap->items[b] = tmp;
    heap->items[a]->heap_index = a;
    heap->items[b]->heap_index = b;
}

static void heap_sift_up(Heap *heap, size_t index)
{
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!heap_better(heap, heap->items[index], heap->items[parent])) {
            break;
        }
        heap_swap(heap, index, parent);
        index = parent;
    }
}

static void heap_sift_down(Heap *heap, size_t index)
{
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t best = index;

        if (left < heap->size && heap_better(heap, heap->items[left], heap->items[best])) {
            best = left;
        }
        if (right < heap->size && heap_better(heap, heap->items[right], heap->items[best])) {
            best = right;
        }
        if (best == index) {
            break;
        }
        heap_swap(heap, index, best);
        index = best;
    }
}

int heap_init(Heap *heap, HeapKind kind)
{
    heap->items = NULL;
    heap->size = 0;
    heap->capacity = 0;
    heap->kind = kind;
    return 0;
}

void heap_free(Heap *heap)
{
    free(heap->items);
    heap->items = NULL;
    heap->size = 0;
    heap->capacity = 0;
}

int heap_insert(Heap *heap, Order *order)
{
    if (heap->size == heap->capacity) {
        size_t next_capacity = heap->capacity == 0 ? 1024 : heap->capacity * 2;
        Order **next = realloc(heap->items, next_capacity * sizeof(*next));
        if (next == NULL) {
            return -1;
        }
        heap->items = next;
        heap->capacity = next_capacity;
    }

    order->heap_index = heap->size;
    heap->items[heap->size++] = order;
    heap_sift_up(heap, order->heap_index);
    return 0;
}

Order *heap_peek(const Heap *heap)
{
    return heap->size == 0 ? NULL : heap->items[0];
}

void heap_remove_at(Heap *heap, size_t index)
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
    heap_fix_at(heap, index);
}

void heap_fix_at(Heap *heap, size_t index)
{
    if (index >= heap->size) {
        return;
    }
    heap_sift_up(heap, index);
    heap_sift_down(heap, heap->items[index]->heap_index);
}
