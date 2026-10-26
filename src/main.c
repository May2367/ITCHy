#include "benchmark.h"
#include "parser.h"
#include "types.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>


typedef struct {
    FILE *output;
    uint64_t interval_ns;
    uint64_t next_emit;
} CsvEmitter;

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

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <input.gz> [--ticker SYMBOL] [--output FILE] [--interval-ns N] [--benchmark]\n",
            program);
}

static int parse_args(int argc, char **argv, Config *config)
{
    memset(config, 0, sizeof(*config));
    config->update_interval_ns = 1000000000ull;
    config->run_mode = RUN_MODE_NORMAL;

    if (argc < 2) {
        return -1;
    }

    config->input_path = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ticker") == 0 && i + 1 < argc) {
            strncpy(config->ticker, argv[++i], ITCHY_TICKER_LEN);
            config->ticker[ITCHY_TICKER_LEN] = '\0';
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config->output_path = argv[++i];
        } else if (strcmp(argv[i], "--interval-ns") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            config->update_interval_ns = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                return -1;
            }
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            config->run_mode = RUN_MODE_BENCHMARK;
        } else {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    Config config;
    FILE *output = stdout;
    gzFile input = NULL;
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
    } else {
        ParserRunOptions options = {0};
        CsvEmitter emitter = {output, config.update_interval_ns, 0};
        ParseResult result;

        input = gzopen(config.input_path, "rb");
        if (input == NULL) {
            perror(config.input_path);
            status = 1;
            goto cleanup;
        }

        fprintf(output, "timestamp_ns,best_bid,bid_shares,best_ask,ask_shares,spread\n");
        options.book_callback = emit_spread;
        options.book_user_data = &emitter;
        result = parser_run_stream(input, &config, &options);
        if (gzclose(input) != Z_OK && result.io_error == 0) {
            result.io_error = EIO;
        }
        input = NULL;

        fprintf(stderr, "messages_read=%llu applied=%llu ignored=%llu unknown=%llu malformed=%llu\n",
                (unsigned long long)result.messages_read,
                (unsigned long long)result.messages_applied,
                (unsigned long long)result.messages_ignored,
                (unsigned long long)result.unknown_messages,
                (unsigned long long)result.malformed_messages);
        if (result.io_error != 0) {
            fprintf(stderr, "parse_error=%d\n", result.io_error);
            status = 1;
        }
    }

cleanup:
    if (input != NULL) {
        gzclose(input);
    }

    if (output != stdout) {
        fclose(output);
    }

    return status;
}
