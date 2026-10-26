CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || printf unknown)
CPPFLAGS ?= -Iinclude/itchy -DITCHY_GIT_COMMIT='"$(GIT_COMMIT)"' -DITCHY_BUILD_CFLAGS='"$(CFLAGS)"'
LDFLAGS ?=
LDLIBS ?= -lz -pthread

TARGET := itchy
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
SAN_DIR := $(BUILD_DIR)/sanitize

SRCS := src/main.c src/parser.c src/orderbook.c src/heap.c src/utils.c src/spsc_ring.c src/publisher.c src/replay.c src/benchmark.c
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

TEST_TARGET := $(BUILD_DIR)/test_itchy
TEST_SRCS := tests/test_itchy.c src/parser.c src/orderbook.c src/heap.c src/utils.c src/spsc_ring.c src/publisher.c src/replay.c
TEST_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_SRCS))

SAN_TARGET := $(SAN_DIR)/test_itchy
SAN_OBJS := $(patsubst %.c,$(SAN_DIR)/%.o,$(TEST_SRCS))
SAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -std=c11
SAN_LDFLAGS := -fsanitize=address,undefined

TMP_FIXTURES := tests/tmp_parser_fixture.gz \
	tests/tmp_parser_standard.gz \
	tests/tmp_parser_tvagg_edges.gz \
	tests/tmp_parser_bad_len.gz \
	tests/tmp_parser_truncated.gz \
	tests/tmp_events.csv \
	tests/tmp_event_log.csv \
	tests/tmp_event_log_bad.csv

.PHONY: all build test bench test-sanitize clean

all: build

build: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SAN_TARGET): $(SAN_OBJS)
	$(CC) $(SAN_LDFLAGS) -o $@ $(SAN_OBJS) $(LDLIBS)

$(SAN_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: build
	./$(TARGET) synthetic --benchmark

test-sanitize: $(SAN_TARGET)
	./$(SAN_TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TMP_FIXTURES)
