.RECIPEPREFIX := >

CC := gcc
CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror -pthread -fsanitize=address -fsanitize=leak

SRC_DIR := src
BUILD_DIR := build

TARGET := $(BUILD_DIR)/proxy-http
SRCS := $(SRC_DIR)/proxy.c

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(SRCS)
>mkdir -p $(BUILD_DIR)
>$(CC) $(CFLAGS) $^ -o $@

run: $(TARGET)
>./$(TARGET)

# Запуск автоматических тестов
test: $(TARGET)
>chmod +x tests/run_tests.sh
>tests/run_tests.sh

clean:
>rm -rf $(BUILD_DIR)