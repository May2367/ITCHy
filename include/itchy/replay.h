#ifndef ITCHY_REPLAY_H
#define ITCHY_REPLAY_H

#include "orderbook.h"

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *output;
    uint64_t next_sequence;
    int error;
} EventLogWriter;

typedef struct {
    uint64_t events_checked;
    uint64_t sequence_gaps;
    uint64_t hash_mismatches;
    uint64_t malformed_lines;
    uint64_t final_hash;
    int io_error;
} ReplayResult;

int event_log_writer_open(EventLogWriter *writer, const char *path);
int event_log_writer_write(EventLogWriter *writer, uint64_t timestamp,
                           const OrderBookMessage *message, uint64_t state_hash);
int event_log_writer_close(EventLogWriter *writer);
int replay_check_file(const char *path, ReplayResult *result);

#endif
