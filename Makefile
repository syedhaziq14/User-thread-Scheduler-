CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -std=c11
INCLUDES = -I./include

SRC_DIR = src
OBJ_DIR = obj
TEST_DIR = tests
BENCH_DIR = benchmarks

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c, $(TEST_DIR)/%, $(TEST_SRCS))

BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.c)
BENCH_BINS = $(patsubst $(BENCH_DIR)/%.c, $(BENCH_DIR)/%, $(BENCH_SRCS))

.PHONY: all build test bench clean

all: build test bench

build: $(OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

test: $(TEST_BINS)

$(TEST_DIR)/%: $(TEST_DIR)/%.c $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm

bench: $(BENCH_BINS)

$(BENCH_DIR)/%: $(BENCH_DIR)/%.c $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TEST_BINS) $(BENCH_BINS)
	rm -f trace.json
