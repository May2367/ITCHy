#ifndef ITCHY_PARSER_H
#define ITCHY_PARSER_H

#include "types.h"
#include "orderbook.h"

#include <stdint.h>
#include <zlib.h>

typedef void (*ParserBookCallback)(uint64_t timestamp, const Order *bid,
                                   const Order *ask, void *user_data);
typedef void (*ParserMessageCallback)(uint64_t timestamp, int applied,
                                      void *user_data);
typedef void (*ParserAppliedMessageCallback)(uint64_t timestamp,
                                             const OrderBookMessage *message,
                                             uint64_t state_hash,
                                             void *user_data);

typedef struct {
    uint64_t max_messages;
    ParserBookCallback book_callback;
    void *book_user_data;
    ParserMessageCallback message_callback;
    void *message_user_data;
    ParserAppliedMessageCallback applied_message_callback;
    void *applied_message_user_data;
} ParserRunOptions;

ParseResult parser_run_stream(gzFile input, const Config *config,
                              const ParserRunOptions *options);

#endif
