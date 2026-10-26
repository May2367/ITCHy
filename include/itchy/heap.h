#ifndef ITCHY_HEAP_H
#define ITCHY_HEAP_H

#include "types.h"

#include <stddef.h>

typedef enum {
    HEAP_MAX = 0,
    HEAP_MIN = 1
} HeapKind;

typedef struct {
    Order **items;
    size_t size;
    size_t capacity;
    HeapKind kind;
} Heap;

int heap_init(Heap *heap, HeapKind kind);
void heap_free(Heap *heap);
int heap_insert(Heap *heap, Order *order);
Order *heap_peek(const Heap *heap);
void heap_remove_at(Heap *heap, size_t index);
void heap_fix_at(Heap *heap, size_t index);

#endif
