#define _POSIX_C_SOURCE 200809L

#include "benchmark.h"

#include "orderbook.h"
#include "publisher.h"
#include "spsc_ring.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ITCHY_GIT_COMMIT
#define ITCHY_GIT_COMMIT "unknown"
#endif

#ifndef ITCHY_BUILD_CFLAGS
#define ITCHY_BUILD_CFLAGS "unknown"
#endif

typedef enum {
    BENCH_ADD,
    BENCH_CANCEL,
    BENCH_DELETE,
    BENCH_REPLACE,
    BENCH_STOP
} BenchOp;

typedef struct {
    BenchOp op;
    uint64_t order_id;
    uint64_t new_order_id;
    char side;
    uint32_t shares;
    uint32_t price;
} BenchMsg;

typedef struct {
    uint64_t id;
    uint32_t shares;
} ActiveOrder;

typedef struct {
    uint64_t *values;
    size_t count;
    size_t capacity;
    int oom;
} LatencySeries;

typedef struct {
    double elapsed_seconds;
    double messages_per_second;
    uint64_t latency_min_ns;
    uint64_t latency_mean_ns;
    uint64_t latency_p50_ns;
    uint64_t latency_p99_ns;
    uint64_t latency_max_ns;
} BenchStats;

typedef struct {
    uint64_t messages;
    uint64_t matching_messages;
    unsigned runs;
    unsigned warmup_runs;
    unsigned batch_size;
    unsigned ring_capacity;
    uint64_t seed;
    BenchStats single;
    BenchStats threaded;
    BenchStats published;
    BenchStats matching;
    EventPublisherStats publisher_stats;
} BenchmarkResult;

typedef struct {
    SpscRing *ring;
    const BenchMsg *msgs;
    size_t count;
} ProducerArgs;

typedef struct {
    SpscRing *ring;
    size_t batch_size;
    LatencySeries *latencies;
    int status;
} ConsumerArgs;

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static uint64_t elapsed_ns(struct timespec start, struct timespec end)
{
    return (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ull +
           (uint64_t)(end.tv_nsec - start.tv_nsec);
}

static uint64_t env_u64(const char *name, uint64_t default_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || *value == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    return (uint64_t)parsed;
}

static uint64_t next_rand(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static int latency_append(LatencySeries *series, uint64_t value)
{
    if (series == NULL) {
        return 0;
    }
    if (series->oom) {
        return -1;
    }
    if (series->count == series->capacity) {
        size_t next_capacity = series->capacity == 0 ? 4096u : series->capacity * 2u;
        uint64_t *next = realloc(series->values, next_capacity * sizeof(*next));
        if (next == NULL) {
            series->oom = 1;
            return -1;
        }
        series->values = next;
        series->capacity = next_capacity;
    }
    series->values[series->count++] = value;
    return 0;
}

static void remove_active(ActiveOrder *active, size_t *active_count, size_t index)
{
    (*active_count)--;
    active[index] = active[*active_count];
}

static int generate_messages(BenchMsg *msgs, size_t count, uint64_t seed)
{
    ActiveOrder *active = calloc(count + 1, sizeof(*active));
    size_t active_count = 0;
    uint64_t next_id = 1;
    uint64_t rng = seed;

    if (active == NULL) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        uint64_t r = next_rand(&rng);
        BenchMsg *msg = &msgs[i];

        memset(msg, 0, sizeof(*msg));
        if (active_count == 0 || (r % 10) < 6) {
            msg->op = BENCH_ADD;
            msg->order_id = next_id++;
            msg->side = (r & 1u) ? 'B' : 'S';
            msg->shares = 10u + (uint32_t)(next_rand(&rng) % 500u);
            msg->price = msg->side == 'B'
                ? 100000u + (uint32_t)(next_rand(&rng) % 400u) * 10u
                : 101000u + (uint32_t)(next_rand(&rng) % 400u) * 10u;
            active[active_count].id = msg->order_id;
            active[active_count].shares = msg->shares;
            active_count++;
        } else {
            size_t index = (size_t)(next_rand(&rng) % active_count);
            ActiveOrder *order = &active[index];

            msg->order_id = order->id;
            switch (r % 4) {
            case 0:
                msg->op = BENCH_CANCEL;
                msg->shares = 1u + (uint32_t)(next_rand(&rng) % order->shares);
                if (msg->shares >= order->shares) {
                    remove_active(active, &active_count, index);
                } else {
                    order->shares -= msg->shares;
                }
                break;
            case 1:
                msg->op = BENCH_DELETE;
                remove_active(active, &active_count, index);
                break;
            default:
                msg->op = BENCH_REPLACE;
                msg->new_order_id = next_id++;
                msg->shares = 10u + (uint32_t)(next_rand(&rng) % 500u);
                msg->price = 100000u + (uint32_t)(next_rand(&rng) % 1400u) * 10u;
                order->id = msg->new_order_id;
                order->shares = msg->shares;
                break;
            }
        }
    }

    free(active);
    return 0;
}

static void generate_matching_messages(BenchMsg *msgs, size_t count, uint64_t seed)
{
    uint64_t rng = seed;

    for (size_t i = 0; i < count; i++) {
        uint64_t r = next_rand(&rng);
        BenchMsg *msg = &msgs[i];

        memset(msg, 0, sizeof(*msg));
        msg->op = BENCH_ADD;
        msg->order_id = i + 1u;
        msg->side = (r & 1u) ? 'B' : 'S';
        msg->shares = 10u + (uint32_t)(next_rand(&rng) % 500u);
        if ((r % 10u) < 7u) {
            msg->price = msg->side == 'B'
                ? 100100u + (uint32_t)(next_rand(&rng) % 50u) * 10u
                : 99900u - (uint32_t)(next_rand(&rng) % 50u) * 10u;
        } else {
            msg->price = msg->side == 'B'
                ? 99500u + (uint32_t)(next_rand(&rng) % 50u) * 10u
                : 100500u + (uint32_t)(next_rand(&rng) % 50u) * 10u;
        }
    }
}

static int apply_msg(OrderBook *book, const BenchMsg *msg)
{
    OrderBookMessage apply = {0};

    apply.order_id = msg->order_id;
    apply.new_order_id = msg->new_order_id;
    apply.side = msg->side;
    apply.shares = msg->shares;
    apply.price = msg->price;
    memcpy(apply.stock, "AMAT", sizeof("AMAT"));

    switch (msg->op) {
    case BENCH_ADD:
        apply.type = ORDERBOOK_MSG_ADD;
        break;
    case BENCH_CANCEL:
        apply.type = ORDERBOOK_MSG_CANCEL;
        break;
    case BENCH_DELETE:
        apply.type = ORDERBOOK_MSG_DELETE;
        break;
    case BENCH_REPLACE:
        apply.type = ORDERBOOK_MSG_REPLACE;
        break;
    case BENCH_STOP:
        return 0;
    }
    return orderbook_apply(book, &apply, NULL);
}

static int run_single_pass(const BenchMsg *msgs, size_t count, size_t batch_size,
                           LatencySeries *latencies, double *elapsed_out)
{
    OrderBook book;
    struct timespec pass_start;
    struct timespec pass_end;

    if (orderbook_init(&book) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &pass_start);
    for (size_t i = 0; i < count;) {
        size_t end = i + batch_size;
        struct timespec batch_start;
        struct timespec batch_end;
        uint64_t per_message_ns;

        if (end > count) {
            end = count;
        }

        clock_gettime(CLOCK_MONOTONIC, &batch_start);
        for (size_t j = i; j < end; j++) {
            int rc = apply_msg(&book, &msgs[j]);
            if (rc < 0) {
                orderbook_free(&book);
                return -1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &batch_end);

        per_message_ns = elapsed_ns(batch_start, batch_end) / (uint64_t)(end - i);
        if (latency_append(latencies, per_message_ns) != 0) {
            orderbook_free(&book);
            return -1;
        }
        i = end;
    }
    clock_gettime(CLOCK_MONOTONIC, &pass_end);

    *elapsed_out = elapsed_seconds(pass_start, pass_end);
    orderbook_free(&book);
    return 0;
}

static int run_published_pass(const BenchMsg *msgs, size_t count, size_t batch_size,
                              size_t ring_capacity, LatencySeries *latencies,
                              double *elapsed_out, EventPublisherStats *publisher_stats)
{
    OrderBook book;
    EventPublisher publisher;
    struct timespec pass_start;
    struct timespec pass_end;
    int status = 0;

    if (orderbook_init(&book) != 0) {
        return -1;
    }
    if (event_publisher_start_file(&publisher, "/dev/null", ring_capacity) != 0) {
        orderbook_free(&book);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &pass_start);
    for (size_t i = 0; i < count;) {
        size_t end = i + batch_size;
        struct timespec batch_start;
        struct timespec batch_end;
        uint64_t per_message_ns;

        if (end > count) {
            end = count;
        }

        clock_gettime(CLOCK_MONOTONIC, &batch_start);
        for (size_t j = i; j < end; j++) {
            int rc = apply_msg(&book, &msgs[j]);
            if (rc < 0) {
                status = -1;
                break;
            }
            event_publisher_publish(&publisher, (uint64_t)j,
                                    orderbook_best_bid(&book),
                                    orderbook_best_ask(&book));
        }
        clock_gettime(CLOCK_MONOTONIC, &batch_end);
        if (status != 0) {
            break;
        }

        per_message_ns = elapsed_ns(batch_start, batch_end) / (uint64_t)(end - i);
        if (latency_append(latencies, per_message_ns) != 0) {
            status = -1;
            break;
        }
        i = end;
    }
    clock_gettime(CLOCK_MONOTONIC, &pass_end);

    if (event_publisher_stop(&publisher) != 0) {
        status = -1;
    }
    if (publisher_stats != NULL) {
        event_publisher_stats(&publisher, publisher_stats);
    }
    *elapsed_out = elapsed_seconds(pass_start, pass_end);
    orderbook_free(&book);
    return status;
}

static int run_matching_pass(const BenchMsg *msgs, size_t count, size_t batch_size,
                             LatencySeries *latencies, double *elapsed_out)
{
    OrderBook book;
    struct timespec pass_start;
    struct timespec pass_end;

    if (orderbook_init(&book) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &pass_start);
    for (size_t i = 0; i < count;) {
        size_t end = i + batch_size;
        struct timespec batch_start;
        struct timespec batch_end;
        uint64_t per_message_ns;

        if (end > count) {
            end = count;
        }

        clock_gettime(CLOCK_MONOTONIC, &batch_start);
        for (size_t j = i; j < end; j++) {
            MatchReport report;
            int rc = orderbook_match_limit(&book, msgs[j].order_id, msgs[j].side,
                                           msgs[j].shares, "AMAT", msgs[j].price,
                                           &report);
            if (rc < 0) {
                orderbook_free(&book);
                return -1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &batch_end);

        per_message_ns = elapsed_ns(batch_start, batch_end) / (uint64_t)(end - i);
        if (latency_append(latencies, per_message_ns) != 0) {
            orderbook_free(&book);
            return -1;
        }
        i = end;
    }
    clock_gettime(CLOCK_MONOTONIC, &pass_end);

    *elapsed_out = elapsed_seconds(pass_start, pass_end);
    orderbook_free(&book);
    return 0;
}

static void *producer_main(void *arg)
{
    ProducerArgs *producer = arg;
    BenchMsg stop = {.op = BENCH_STOP};

    for (size_t i = 0; i < producer->count; i++) {
        while (!spsc_ring_push(producer->ring, &producer->msgs[i])) {
            sched_yield();
        }
    }
    while (!spsc_ring_push(producer->ring, &stop)) {
        sched_yield();
    }
    return NULL;
}

static void *consumer_main(void *arg)
{
    ConsumerArgs *consumer = arg;
    OrderBook book;
    int done = 0;

    if (orderbook_init(&book) != 0) {
        consumer->status = -1;
        return NULL;
    }

    while (!done) {
        size_t applied = 0;
        struct timespec batch_start;
        struct timespec batch_end;

        clock_gettime(CLOCK_MONOTONIC, &batch_start);
        while (applied < consumer->batch_size && !done) {
            BenchMsg msg;
            if (!spsc_ring_pop(consumer->ring, &msg)) {
                if (applied == 0) {
                    clock_gettime(CLOCK_MONOTONIC, &batch_start);
                }
                sched_yield();
                continue;
            }
            if (msg.op == BENCH_STOP) {
                done = 1;
                break;
            }
            if (apply_msg(&book, &msg) < 0) {
                consumer->status = -1;
                done = 1;
                break;
            }
            applied++;
        }

        if (applied > 0) {
            uint64_t per_message_ns;
            clock_gettime(CLOCK_MONOTONIC, &batch_end);
            per_message_ns = elapsed_ns(batch_start, batch_end) / (uint64_t)applied;
            if (latency_append(consumer->latencies, per_message_ns) != 0) {
                consumer->status = -1;
                break;
            }
        }
    }

    orderbook_free(&book);
    return NULL;
}

static int run_threaded_pass(const BenchMsg *msgs, size_t count, size_t batch_size,
                             size_t ring_capacity, LatencySeries *latencies,
                             double *elapsed_out)
{
    SpscRing ring;
    ProducerArgs producer = {0};
    ConsumerArgs consumer = {0};
    pthread_t producer_thread;
    pthread_t consumer_thread;
    struct timespec pass_start;
    struct timespec pass_end;
    int status = 0;

    if (spsc_ring_init(&ring, ring_capacity, sizeof(*msgs)) != 0) {
        return -1;
    }

    producer.ring = &ring;
    producer.msgs = msgs;
    producer.count = count;
    consumer.ring = &ring;
    consumer.batch_size = batch_size;
    consumer.latencies = latencies;

    clock_gettime(CLOCK_MONOTONIC, &pass_start);
    if (pthread_create(&consumer_thread, NULL, consumer_main, &consumer) != 0) {
        spsc_ring_free(&ring);
        return -1;
    }
    if (pthread_create(&producer_thread, NULL, producer_main, &producer) != 0) {
        status = -1;
    }

    if (status == 0) {
        pthread_join(producer_thread, NULL);
    }
    pthread_join(consumer_thread, NULL);
    clock_gettime(CLOCK_MONOTONIC, &pass_end);

    if (consumer.status != 0) {
        status = -1;
    }
    *elapsed_out = elapsed_seconds(pass_start, pass_end);
    spsc_ring_free(&ring);
    return status;
}

static void compute_latency_stats(BenchStats *stats, LatencySeries *series)
{
    long double sum = 0.0L;
    size_t p50_index;
    size_t p99_index;

    if (series->count == 0) {
        return;
    }

    qsort(series->values, series->count, sizeof(*series->values), compare_u64);
    for (size_t i = 0; i < series->count; i++) {
        sum += (long double)series->values[i];
    }

    p50_index = series->count / 2u;
    p99_index = (series->count * 99u + 99u) / 100u;
    if (p99_index == 0) {
        p99_index = 1;
    }
    p99_index--;
    if (p99_index >= series->count) {
        p99_index = series->count - 1;
    }

    stats->latency_min_ns = series->values[0];
    stats->latency_mean_ns = (uint64_t)(sum / (long double)series->count);
    stats->latency_p50_ns = series->values[p50_index];
    stats->latency_p99_ns = series->values[p99_index];
    stats->latency_max_ns = series->values[series->count - 1];
}

static int run_matching_measured_passes(const BenchMsg *msgs, BenchmarkResult *result,
                                       BenchStats *stats)
{
    LatencySeries latencies = {0};
    double warmup_elapsed;
    int status = 0;

    for (unsigned i = 0; i < result->warmup_runs; i++) {
        if (run_matching_pass(msgs, (size_t)result->matching_messages, result->batch_size,
                              NULL, &warmup_elapsed) != 0) {
            return 1;
        }
    }

    for (unsigned i = 0; i < result->runs; i++) {
        double elapsed = 0.0;
        if (run_matching_pass(msgs, (size_t)result->matching_messages, result->batch_size,
                              &latencies, &elapsed) != 0) {
            status = 1;
            break;
        }
        stats->elapsed_seconds += elapsed;
    }

    if (status == 0) {
        stats->messages_per_second = stats->elapsed_seconds > 0.0
            ? ((double)result->matching_messages * (double)result->runs) / stats->elapsed_seconds
            : 0.0;
        compute_latency_stats(stats, &latencies);
    }

    free(latencies.values);
    return status;
}

static void print_cpu(FILE *output)
{
    FILE *cpu = fopen("/proc/cpuinfo", "r");
    char line[256];

    if (cpu == NULL) {
        fprintf(output, "cpu=unknown\n");
        return;
    }

    while (fgets(line, sizeof(line), cpu) != NULL) {
        if (strncmp(line, "model name", 10) == 0) {
            char *value = strchr(line, ':');
            if (value != NULL) {
                value++;
                while (*value == ' ' || *value == '\t') {
                    value++;
                }
                value[strcspn(value, "\n")] = '\0';
                fprintf(output, "cpu=%s\n", value);
                fclose(cpu);
                return;
            }
        }
    }

    fclose(cpu);
    fprintf(output, "cpu=unknown\n");
}

static void print_stats(FILE *output, const char *name, const BenchStats *stats)
{
    fprintf(output, "[%s]\n", name);
    fprintf(output, "elapsed_seconds=%.6f\n", stats->elapsed_seconds);
    fprintf(output, "messages_per_second=%.2f\n", stats->messages_per_second);
    fprintf(output, "apply_latency_min_ns=%llu\n", (unsigned long long)stats->latency_min_ns);
    fprintf(output, "apply_latency_mean_ns=%llu\n", (unsigned long long)stats->latency_mean_ns);
    fprintf(output, "apply_latency_p50_ns=%llu\n", (unsigned long long)stats->latency_p50_ns);
    fprintf(output, "apply_latency_p99_ns=%llu\n", (unsigned long long)stats->latency_p99_ns);
    fprintf(output, "apply_latency_max_ns=%llu\n\n", (unsigned long long)stats->latency_max_ns);
}

static void print_delta(FILE *output, const char *name, const BenchStats *base,
                        const BenchStats *other)
{
    double throughput_ratio = base->messages_per_second > 0.0
        ? other->messages_per_second / base->messages_per_second
        : 0.0;

    fprintf(output, "[%s_vs_single_thread]\n", name);
    fprintf(output, "throughput_ratio=%.4f\n", throughput_ratio);
    fprintf(output, "mean_latency_delta_ns=%lld\n",
            (long long)other->latency_mean_ns - (long long)base->latency_mean_ns);
    fprintf(output, "p50_latency_delta_ns=%lld\n",
            (long long)other->latency_p50_ns - (long long)base->latency_p50_ns);
    fprintf(output, "p99_latency_delta_ns=%lld\n\n",
            (long long)other->latency_p99_ns - (long long)base->latency_p99_ns);
}

static int run_measured_passes(const BenchMsg *msgs, BenchmarkResult *result,
                               int threaded, BenchStats *stats)
{
    LatencySeries latencies = {0};
    double warmup_elapsed;
    int status = 0;

    for (unsigned i = 0; i < result->warmup_runs; i++) {
        int rc = threaded
            ? run_threaded_pass(msgs, (size_t)result->messages, result->batch_size,
                                result->ring_capacity, NULL, &warmup_elapsed)
            : run_single_pass(msgs, (size_t)result->messages, result->batch_size,
                              NULL, &warmup_elapsed);
        if (rc != 0) {
            return 1;
        }
    }

    for (unsigned i = 0; i < result->runs; i++) {
        double elapsed = 0.0;
        int rc = threaded
            ? run_threaded_pass(msgs, (size_t)result->messages, result->batch_size,
                                result->ring_capacity, &latencies, &elapsed)
            : run_single_pass(msgs, (size_t)result->messages, result->batch_size,
                              &latencies, &elapsed);
        if (rc != 0) {
            status = 1;
            break;
        }
        stats->elapsed_seconds += elapsed;
    }

    if (status == 0) {
        stats->messages_per_second = stats->elapsed_seconds > 0.0
            ? ((double)result->messages * (double)result->runs) / stats->elapsed_seconds
            : 0.0;
        compute_latency_stats(stats, &latencies);
    }

    free(latencies.values);
    return status;
}

static int run_published_measured_passes(const BenchMsg *msgs, BenchmarkResult *result,
                                         BenchStats *stats)
{
    LatencySeries latencies = {0};
    EventPublisherStats publisher_totals = {0};
    double warmup_elapsed;
    int status = 0;

    for (unsigned i = 0; i < result->warmup_runs; i++) {
        if (run_published_pass(msgs, (size_t)result->messages, result->batch_size,
                               result->ring_capacity, NULL, &warmup_elapsed, NULL) != 0) {
            return 1;
        }
    }

    for (unsigned i = 0; i < result->runs; i++) {
        EventPublisherStats pass_stats;
        double elapsed = 0.0;
        if (run_published_pass(msgs, (size_t)result->messages, result->batch_size,
                               result->ring_capacity, &latencies, &elapsed,
                               &pass_stats) != 0) {
            status = 1;
            break;
        }
        stats->elapsed_seconds += elapsed;
        publisher_totals.enqueued += pass_stats.enqueued;
        publisher_totals.dropped += pass_stats.dropped;
        publisher_totals.written += pass_stats.written;
        publisher_totals.next_sequence += pass_stats.next_sequence;
        publisher_totals.write_error |= pass_stats.write_error;
    }

    if (status == 0) {
        stats->messages_per_second = stats->elapsed_seconds > 0.0
            ? ((double)result->messages * (double)result->runs) / stats->elapsed_seconds
            : 0.0;
        compute_latency_stats(stats, &latencies);
        result->publisher_stats = publisher_totals;
    }

    free(latencies.values);
    return status;
}

int benchmark_run(const Config *config, FILE *output)
{
    BenchmarkResult result = {0};
    BenchMsg *msgs;
    BenchMsg *matching_msgs;
    int status = 0;

    result.messages = env_u64("ITCHY_BENCH_MESSAGES", 200000ull);
    result.matching_messages = env_u64("ITCHY_MATCH_BENCH_MESSAGES", result.messages);
    result.runs = (unsigned)env_u64("ITCHY_BENCH_RUNS", 5ull);
    result.warmup_runs = (unsigned)env_u64("ITCHY_BENCH_WARMUP_RUNS", 1ull);
    result.batch_size = (unsigned)env_u64("ITCHY_BENCH_BATCH", 64ull);
    result.ring_capacity = (unsigned)env_u64("ITCHY_BENCH_RING", 4096ull);
    result.seed = env_u64("ITCHY_BENCH_SEED", 1ull);

    if (result.messages == 0 || result.runs == 0 || result.batch_size == 0 ||
        result.matching_messages == 0 || result.ring_capacity < 2) {
        fprintf(output, "invalid benchmark settings\n");
        return 1;
    }

    msgs = calloc((size_t)result.messages, sizeof(*msgs));
    matching_msgs = calloc((size_t)result.matching_messages, sizeof(*matching_msgs));
    if (msgs == NULL || matching_msgs == NULL ||
        generate_messages(msgs, (size_t)result.messages, result.seed) != 0) {
        free(msgs);
        free(matching_msgs);
        return 1;
    }
    generate_matching_messages(matching_msgs, (size_t)result.matching_messages, result.seed ^ 0x9e3779b97f4a7c15ull);

    status |= run_measured_passes(msgs, &result, 0, &result.single);
    status |= run_measured_passes(msgs, &result, 1, &result.threaded);
    status |= run_published_measured_passes(msgs, &result, &result.published);
    status |= run_matching_measured_passes(matching_msgs, &result, &result.matching);

    fprintf(output, "ITCHy benchmark\n");
    fprintf(output, "mode=synthetic_apply_compare\n");
    fprintf(output, "input_arg=%s\n", config->input_path == NULL ? "<none>" : config->input_path);
    fprintf(output, "messages_per_run=%llu\n", (unsigned long long)result.messages);
    fprintf(output, "matching_messages_per_run=%llu\n", (unsigned long long)result.matching_messages);
    fprintf(output, "runs=%u\n", result.runs);
    fprintf(output, "warmup_runs=%u\n", result.warmup_runs);
    fprintf(output, "batch_size=%u\n", result.batch_size);
    fprintf(output, "ring_capacity=%u\n", result.ring_capacity);
    fprintf(output, "seed=%llu\n", (unsigned long long)result.seed);
    print_cpu(output);
    fprintf(output, "compiler=%s\n", __VERSION__);
    fprintf(output, "cflags=%s\n", ITCHY_BUILD_CFLAGS);
    fprintf(output, "commit=%s\n\n", ITCHY_GIT_COMMIT);
    print_stats(output, "single_thread", &result.single);
    print_stats(output, "spsc_threaded", &result.threaded);
    print_stats(output, "publisher_file_sink", &result.published);
    fprintf(output, "publisher_enqueued=%llu\n", (unsigned long long)result.publisher_stats.enqueued);
    fprintf(output, "publisher_dropped=%llu\n", (unsigned long long)result.publisher_stats.dropped);
    fprintf(output, "publisher_written=%llu\n", (unsigned long long)result.publisher_stats.written);
    fprintf(output, "publisher_drop_rate_pct=%.4f\n\n",
            result.publisher_stats.next_sequence > 0
                ? 100.0 * (double)result.publisher_stats.dropped / (double)result.publisher_stats.next_sequence
                : 0.0);
    print_delta(output, "spsc_threaded", &result.single, &result.threaded);
    print_delta(output, "publisher_file_sink", &result.single, &result.published);
    print_stats(output, "matching_engine", &result.matching);

    free(matching_msgs);
    free(msgs);
    return status;
}
