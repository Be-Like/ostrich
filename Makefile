CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

BUILD   := build
SRC     := src
TESTS   := tests
INCLUDE := include

.PHONY: all clean test

all: $(BUILD)/ostrich

$(BUILD)/ostrich: $(SRC)/main.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/smoke_test: $(TESTS)/smoke_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/arena_test: $(TESTS)/arena_test.c $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

$(BUILD)/lexicon_test: $(TESTS)/lexicon_test.c $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

test: all $(BUILD)/smoke_test $(BUILD)/arena_test $(BUILD)/lexicon_test
	./$(BUILD)/smoke_test
	./$(BUILD)/arena_test
	./$(BUILD)/lexicon_test

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
