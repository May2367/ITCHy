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

SRCS := src/main.c src/parser.c src/orderbook.c src/utils.c src/spsc_ring.c src/publisher.c src/replay.c src/benchmark.c
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

TEST_TARGET := $(BUILD_DIR)/test_itchy
TEST_SRCS := tests/test_itchy.c src/parser.c src/orderbook.c src/utils.c src/spsc_ring.c src/publisher.c src/replay.c
TEST_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_SRCS))

SAN_TARGET := $(SAN_DIR)/test_itchy
SAN_OBJS := $(patsubst %.c,$(SAN_DIR)/%.o,$(TEST_SRCS))
SAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -std=c11
SAN_LDFLAGS := -fsanitize=address,undefined

FUZZ_CC ?= clang
FUZZ_TARGET := $(BUILD_DIR)/fuzz_parser
FUZZ_SRCS := tests/fuzz_parser.c src/parser.c src/orderbook.c src/utils.c
FUZZ_CFLAGS := -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -Wall -Wextra -std=c11

TMP_FIXTURES := tests/tmp_parser_fixture.gz \
	tests/tmp_parser_standard.gz \
	tests/tmp_parser_tvagg_edges.gz \
	tests/tmp_parser_ticker_filter.gz \
	tests/tmp_parser_bad_len.gz \
	tests/tmp_parser_truncated.gz \
	tests/tmp_events.csv \
	tests/tmp_event_log.csv \
	tests/tmp_event_log_bad.csv

.PHONY: all build test bench test-sanitize fuzz-parser fuzz-parser-smoke clean

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

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)
	@if ./$(TARGET) /tmp/itchy_missing_ticker.gz --output /dev/null >/tmp/itchy_missing_ticker.out 2>/tmp/itchy_missing_ticker.err; then \
		echo "missing ticker unexpectedly succeeded"; \
		exit 1; \
	else \
		status=$$?; \
		if [ $$status -ne 2 ]; then \
			echo "missing ticker exited $$status, expected 2"; \
			exit 1; \
		fi; \
	fi

bench: build
	./$(TARGET) synthetic --benchmark

test-sanitize: $(SAN_TARGET)
	./$(SAN_TARGET)

fuzz-parser: $(FUZZ_TARGET)

$(FUZZ_TARGET): $(FUZZ_SRCS)
	mkdir -p $(dir $@)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -o $@ $(FUZZ_SRCS) $(LDLIBS)

fuzz-parser-smoke: $(FUZZ_TARGET)
	./$(FUZZ_TARGET) -runs=100 -max_len=2048

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TMP_FIXTURES)
