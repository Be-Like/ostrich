CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

BUILD   := build
SRC     := src
TESTS   := tests

.PHONY: all clean test

all: $(BUILD)/ostrich

$(BUILD)/ostrich: $(SRC)/main.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/smoke_test: $(TESTS)/smoke_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

test: all $(BUILD)/smoke_test
	./$(BUILD)/smoke_test

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
