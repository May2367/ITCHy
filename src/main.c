#define _POSIX_C_SOURCE 200809L

#include "benchmark.h"
#include "parser.h"
#include "publisher.h"
#include "replay.h"
#include "types.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>


typedef struct {
    FILE *output;
    uint64_t interval_ns;
    uint64_t next_emit;
} CsvEmitter;

typedef struct {
    CsvEmitter emitter;
    EventPublisher *publisher;
    EventLogWriter *event_log;
} BookCallbacks;

typedef struct {
    uint64_t interval;
    uint64_t messages;
    uint64_t applied;
    struct timespec start;
} ParseProgress;

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void emit_spread(uint64_t timestamp, const Order *bid, const Order *ask,
                        void *user_data)
{
    CsvEmitter *emitter = user_data;
    char bid_price[32];
    char ask_price[32];
    char spread[32];

    if (emitter == NULL || emitter->output == NULL || bid == NULL || ask == NULL) {
        return;
    }
    if (emitter->interval_ns != 0 && timestamp < emitter->next_emit) {
        return;
    }

    format_price(bid_price, sizeof(bid_price), bid->price);
    format_price(ask_price, sizeof(ask_price), ask->price);
    if (ask->price >= bid->price) {
        format_price(spread, sizeof(spread), ask->price - bid->price);
    } else {
        snprintf(spread, sizeof(spread), "-");
    }

    fprintf(emitter->output, "%llu,%s,%llu,%s,%llu,%s\n",
            (unsigned long long)timestamp,
            bid_price, (unsigned long long)bid->shares,
            ask_price, (unsigned long long)ask->shares,
            spread);
    emitter->next_emit = timestamp + emitter->interval_ns;
}

static void on_book_update(uint64_t timestamp, const Order *bid, const Order *ask,
                           void *user_data)
{
    BookCallbacks *callbacks = user_data;

    if (callbacks == NULL) {
        return;
    }
    emit_spread(timestamp, bid, ask, &callbacks->emitter);
    event_publisher_publish(callbacks->publisher, timestamp, bid, ask);
}

static void on_applied_message(uint64_t timestamp, const OrderBookMessage *message,
                               uint64_t state_hash, void *user_data)
{
    BookCallbacks *callbacks = user_data;

    if (callbacks == NULL || callbacks->event_log == NULL) {
        return;
    }
    event_log_writer_write(callbacks->event_log, timestamp, message, state_hash);
}

static void on_parse_message(uint64_t timestamp, int applied, void *user_data)
{
    ParseProgress *progress = user_data;

    (void)timestamp;
    if (progress == NULL || progress->interval == 0) {
        return;
    }
    progress->messages++;
    if (applied) {
        progress->applied++;
    }
    if (progress->messages % progress->interval == 0) {
        struct timespec now;
        double elapsed;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = elapsed_seconds(progress->start, now);
        fprintf(stderr, "parse_progress messages_read=%llu applied=%llu elapsed_seconds=%.6f messages_per_second=%.2f\n",
                (unsigned long long)progress->messages,
                (unsigned long long)progress->applied,
                elapsed,
                elapsed > 0.0 ? (double)progress->messages / elapsed : 0.0);
    }
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <input.gz> --ticker SYMBOL [--output FILE] [--interval-ns N] [--publish FILE] [--publish-ring N] [--event-log FILE] [--max-messages N] [--progress-interval N]\n"
            "       %s <input> --benchmark\n"
            "       %s <event-log.csv> --replay\n",
            program, program, program);
}

static int parse_args(int argc, char **argv, Config *config)
{
    memset(config, 0, sizeof(*config));
    config->update_interval_ns = 1000000000ull;
    config->publish_ring_capacity = 262144;
    config->run_mode = RUN_MODE_NORMAL;

    if (argc < 2) {
        return -1;
    }

    config->input_path = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ticker") == 0 && i + 1 < argc) {
            config_set_ticker(config, argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config->output_path = argv[++i];
        } else if (strcmp(argv[i], "--interval-ns") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            config->update_interval_ns = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                return -1;
            }
        } else if (strcmp(argv[i], "--publish") == 0 && i + 1 < argc) {
            config->publish_path = argv[++i];
        } else if (strcmp(argv[i], "--event-log") == 0 && i + 1 < argc) {
            config->event_log_path = argv[++i];
        } else if (strcmp(argv[i], "--publish-ring") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            config->publish_ring_capacity = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || config->publish_ring_capacity < 2) {
                return -1;
            }
        } else if (strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            config->max_messages = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                return -1;
            }
        } else if (strcmp(argv[i], "--progress-interval") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            config->progress_interval = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                return -1;
            }
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            config->run_mode = RUN_MODE_BENCHMARK;
        } else if (strcmp(argv[i], "--replay") == 0) {
            config->run_mode = RUN_MODE_REPLAY;
        } else {
            return -1;
        }
    }

    if (config->run_mode == RUN_MODE_NORMAL && config->ticker[0] == '\0') {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    Config config;
    FILE *output = stdout;
    gzFile input = NULL;
    EventPublisher publisher;
    EventLogWriter event_log;
    int publisher_started = 0;
    int event_log_started = 0;
    int status = 0;

    if (parse_args(argc, argv, &config) != 0) {
        usage(argv[0]);
        return 2;
    }

    if (config.output_path != NULL) {
        output = fopen(config.output_path, "w");
        if (output == NULL) {
            perror(config.output_path);
            return 1;
        }
    }

    if (config.run_mode == RUN_MODE_BENCHMARK) {
        status = benchmark_run(&config, output);
    } else if (config.run_mode == RUN_MODE_REPLAY) {
        ReplayResult replay;
        if (replay_check_file(config.input_path, &replay) != 0) {
            status = 1;
        }
        fprintf(output, "events_checked=%llu\n",
                (unsigned long long)replay.events_checked);
        fprintf(output, "sequence_gaps=%llu\n",
                (unsigned long long)replay.sequence_gaps);
        fprintf(output, "hash_mismatches=%llu\n",
                (unsigned long long)replay.hash_mismatches);
        fprintf(output, "malformed_lines=%llu\n",
                (unsigned long long)replay.malformed_lines);
        fprintf(output, "final_hash=%llu\n",
                (unsigned long long)replay.final_hash);
        if (replay.io_error != 0) {
            fprintf(output, "io_error=%d\n", replay.io_error);
        }
    } else {
        ParserRunOptions options = {0};
        BookCallbacks callbacks = {{output, config.update_interval_ns, 0}, NULL, NULL};
        ParseResult result;
        ParseProgress progress = {config.progress_interval, 0, 0, {0, 0}};
        struct timespec parse_start;
        struct timespec parse_end;
        double parse_elapsed;

        if (config.event_log_path != NULL) {
            if (event_log_writer_open(&event_log, config.event_log_path) != 0) {
                perror(config.event_log_path);
                status = 1;
                goto cleanup;
            }
            event_log_started = 1;
            callbacks.event_log = &event_log;
        }

        if (config.publish_path != NULL) {
            if (event_publisher_start_file(&publisher, config.publish_path,
                                      (size_t)config.publish_ring_capacity) != 0) {
                perror(config.publish_path);
                status = 1;
                goto cleanup;
            }
            publisher_started = 1;
            callbacks.publisher = &publisher;
        }

        input = gzopen(config.input_path, "rb");
        if (input == NULL) {
            perror(config.input_path);
            status = 1;
            goto cleanup;
        }

        fprintf(output, "timestamp_ns,best_bid,bid_shares,best_ask,ask_shares,spread\n");
        options.max_messages = config.max_messages;
        options.book_callback = on_book_update;
        options.book_user_data = &callbacks;
        if (callbacks.event_log != NULL) {
            options.applied_message_callback = on_applied_message;
            options.applied_message_user_data = &callbacks;
        }
        if (config.progress_interval > 0) {
            options.message_callback = on_parse_message;
            options.message_user_data = &progress;
        }
        clock_gettime(CLOCK_MONOTONIC, &parse_start);
        progress.start = parse_start;
        result = parser_run_stream(input, &config, &options);
        clock_gettime(CLOCK_MONOTONIC, &parse_end);
        parse_elapsed = elapsed_seconds(parse_start, parse_end);
        if (gzclose(input) != Z_OK && result.io_error == 0) {
            result.io_error = EIO;
        }
        input = NULL;

        if (event_log_started) {
            if (event_log_writer_close(&event_log) != 0 && result.io_error == 0) {
                result.io_error = EIO;
            }
            event_log_started = 0;
        }

        if (publisher_started) {
            EventPublisherStats stats;
            if (event_publisher_stop(&publisher) != 0 && result.io_error == 0) {
                result.io_error = EIO;
            }
            publisher_started = 0;
            event_publisher_stats(&publisher, &stats);
            fprintf(stderr, "published=%llu dropped=%llu written=%llu\n",
                    (unsigned long long)stats.enqueued,
                    (unsigned long long)stats.dropped,
                    (unsigned long long)stats.written);
        }

        fprintf(stderr, "messages_read=%llu applied=%llu ignored=%llu unknown=%llu malformed=%llu elapsed_seconds=%.6f messages_per_second=%.2f\n",
                (unsigned long long)result.messages_read,
                (unsigned long long)result.messages_applied,
                (unsigned long long)result.messages_ignored,
                (unsigned long long)result.unknown_messages,
                (unsigned long long)result.malformed_messages,
                parse_elapsed,
                parse_elapsed > 0.0 ? (double)result.messages_read / parse_elapsed : 0.0);
        if (result.io_error != 0) {
            fprintf(stderr, "parse_error=%d\n", result.io_error);
            status = 1;
        }
    }

cleanup:
    if (event_log_started) {
        if (event_log_writer_close(&event_log) != 0) {
            status = 1;
        }
    }

    if (publisher_started) {
        if (event_publisher_stop(&publisher) != 0) {
            status = 1;
        }
    }

    if (input != NULL) {
        gzclose(input);
    }

    if (output != stdout) {
        fclose(output);
    }

    return status;
}
