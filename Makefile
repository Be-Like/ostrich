CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

BUILD   := build
SRC     := src

.PHONY: all clean test

all: $(BUILD)/ostrich

$(BUILD)/ostrich: $(SRC)/main.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
