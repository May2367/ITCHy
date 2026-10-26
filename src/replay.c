#define _POSIX_C_SOURCE 200809L

#include "replay.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *message_type_name(OrderBookMessageType type)
{
    switch (type) {
    case ORDERBOOK_MSG_ADD:
        return "ADD";
    case ORDERBOOK_MSG_EXECUTE:
        return "EXECUTE";
    case ORDERBOOK_MSG_CANCEL:
        return "CANCEL";
    case ORDERBOOK_MSG_DELETE:
        return "DELETE";
    case ORDERBOOK_MSG_REPLACE:
        return "REPLACE";
    case ORDERBOOK_MSG_UPSERT_LEVEL:
        return "UPSERT_LEVEL";
    case ORDERBOOK_MSG_MATCH_LIMIT:
        return "MATCH_LIMIT";
    }
    return "UNKNOWN";
}

static int parse_message_type(const char *value, OrderBookMessageType *type)
{
    if (strcmp(value, "ADD") == 0) {
        *type = ORDERBOOK_MSG_ADD;
    } else if (strcmp(value, "EXECUTE") == 0) {
        *type = ORDERBOOK_MSG_EXECUTE;
    } else if (strcmp(value, "CANCEL") == 0) {
        *type = ORDERBOOK_MSG_CANCEL;
    } else if (strcmp(value, "DELETE") == 0) {
        *type = ORDERBOOK_MSG_DELETE;
    } else if (strcmp(value, "REPLACE") == 0) {
        *type = ORDERBOOK_MSG_REPLACE;
    } else if (strcmp(value, "UPSERT_LEVEL") == 0) {
        *type = ORDERBOOK_MSG_UPSERT_LEVEL;
    } else if (strcmp(value, "MATCH_LIMIT") == 0) {
        *type = ORDERBOOK_MSG_MATCH_LIMIT;
    } else {
        return -1;
    }
    return 0;
}

static int parse_u64(const char *value, uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)parsed;
    return 0;
}

static int parse_u32(const char *value, uint32_t *out)
{
    uint64_t parsed;
    if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)parsed;
    return 0;
}

int event_log_writer_open(EventLogWriter *writer, const char *path)
{
    if (writer == NULL || path == NULL) {
        return -1;
    }

    memset(writer, 0, sizeof(*writer));
    writer->output = fopen(path, "w");
    if (writer->output == NULL) {
        return -1;
    }

    fprintf(writer->output,
            "seq,timestamp_ns,type,order_id,new_order_id,side,shares,price,stock,state_hash\n");
    if (ferror(writer->output)) {
        event_log_writer_close(writer);
        return -1;
    }
    return 0;
}

int event_log_writer_write(EventLogWriter *writer, uint64_t timestamp,
                           const OrderBookMessage *message, uint64_t state_hash)
{
    char side[2] = "-";
    const char *stock;

    if (writer == NULL || writer->output == NULL || message == NULL) {
        return 0;
    }
    if (message->side != '\0') {
        side[0] = message->side;
        side[1] = '\0';
    }
    stock = message->stock[0] == '\0' ? "-" : message->stock;

    fprintf(writer->output, "%llu,%llu,%s,%llu,%llu,%s,%u,%u,%s,%llu\n",
            (unsigned long long)writer->next_sequence++,
            (unsigned long long)timestamp,
            message_type_name(message->type),
            (unsigned long long)message->order_id,
            (unsigned long long)message->new_order_id,
            side,
            message->shares,
            message->price,
            stock,
            (unsigned long long)state_hash);
    if (ferror(writer->output)) {
        writer->error = 1;
        return -1;
    }
    return 0;
}

int event_log_writer_close(EventLogWriter *writer)
{
    int status = 0;

    if (writer == NULL || writer->output == NULL) {
        return 0;
    }
    if (fflush(writer->output) != 0) {
        status = -1;
    }
    if (fclose(writer->output) != 0) {
        status = -1;
    }
    writer->output = NULL;
    return writer->error || status != 0 ? -1 : 0;
}

static int parse_event_line(char *line, uint64_t *sequence, uint64_t *timestamp,
                            OrderBookMessage *message, uint64_t *expected_hash)
{
    char *fields[10];
    char *save = NULL;
    char *token;
    size_t count = 0;

    line[strcspn(line, "\n")] = '\0';
    for (token = strtok_r(line, ",", &save); token != NULL && count < 10;
         token = strtok_r(NULL, ",", &save)) {
        fields[count++] = token;
    }
    if (count != 10 || strtok_r(NULL, ",", &save) != NULL) {
        return -1;
    }

    memset(message, 0, sizeof(*message));
    if (parse_u64(fields[0], sequence) != 0 ||
        parse_u64(fields[1], timestamp) != 0 ||
        parse_message_type(fields[2], &message->type) != 0 ||
        parse_u64(fields[3], &message->order_id) != 0 ||
        parse_u64(fields[4], &message->new_order_id) != 0 ||
        parse_u32(fields[6], &message->shares) != 0 ||
        parse_u32(fields[7], &message->price) != 0 ||
        parse_u64(fields[9], expected_hash) != 0) {
        return -1;
    }
    if (strcmp(fields[5], "-") != 0) {
        if (fields[5][0] == '\0' || fields[5][1] != '\0') {
            return -1;
        }
        message->side = fields[5][0];
    }
    if (strcmp(fields[8], "-") != 0) {
        snprintf(message->stock, sizeof(message->stock), "%.*s",
                 ITCHY_TICKER_LEN, fields[8]);
    }
    (void)timestamp;
    return 0;
}

int replay_check_file(const char *path, ReplayResult *result)
{
    FILE *input;
    OrderBook book;
    char line[512];
    uint64_t expected_sequence = 0;

    if (result == NULL) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    input = fopen(path, "r");
    if (input == NULL) {
        result->io_error = errno == 0 ? EIO : errno;
        return -1;
    }
    if (orderbook_init(&book) != 0) {
        fclose(input);
        result->io_error = ENOMEM;
        return -1;
    }

    if (fgets(line, sizeof(line), input) == NULL) {
        result->malformed_lines++;
        result->io_error = EINVAL;
        orderbook_free(&book);
        fclose(input);
        return -1;
    }
    if (strcmp(line, "seq,timestamp_ns,type,order_id,new_order_id,side,shares,price,stock,state_hash\n") != 0) {
        result->malformed_lines++;
    }

    while (fgets(line, sizeof(line), input) != NULL) {
        OrderBookMessage message;
        uint64_t sequence;
        uint64_t timestamp;
        uint64_t expected_hash;
        uint64_t actual_hash;
        int rc;

        if (parse_event_line(line, &sequence, &timestamp, &message, &expected_hash) != 0) {
            result->malformed_lines++;
            continue;
        }
        if (sequence != expected_sequence) {
            result->sequence_gaps++;
            expected_sequence = sequence;
        }
        expected_sequence++;

        rc = orderbook_apply(&book, &message, NULL);
        if (rc < 0 || orderbook_state_hash(&book, &actual_hash) != 0) {
            result->io_error = ENOMEM;
            break;
        }
        if (rc == 0 || actual_hash != expected_hash) {
            result->hash_mismatches++;
        }
        result->final_hash = actual_hash;
        result->events_checked++;
        (void)timestamp;
    }

    if (ferror(input) && result->io_error == 0) {
        result->io_error = EIO;
    }
    orderbook_free(&book);
    fclose(input);
    return result->io_error == 0 && result->malformed_lines == 0 &&
           result->sequence_gaps == 0 && result->hash_mismatches == 0 ? 0 : -1;
}
