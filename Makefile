# SPDX-License-Identifier: GPL-3.0-only
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Iengine/include
LDLIBS ?= -lm
BUILD := build
ENGINE := engine/src/wurli_engine.c engine/src/wurli_tables.c
TEST := engine/tests/test_engine.c
RENDER := tools/render/render.c

.PHONY: all test reed bench web demo firmware clean

all: test reed

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_engine: $(ENGINE) $(TEST) | $(BUILD)
	$(CC) $(CFLAGS) $(ENGINE) $(TEST) $(LDLIBS) -o $@

$(BUILD)/reed: $(ENGINE) $(RENDER) | $(BUILD)
	$(CC) $(CFLAGS) $(ENGINE) $(RENDER) $(LDLIBS) -o $@

test: $(BUILD)/test_engine
	./$(BUILD)/test_engine

reed: $(BUILD)/reed

bench: $(BUILD)/reed
	./$(BUILD)/reed bench

demo: $(BUILD)/reed
	./$(BUILD)/reed demo --out $(BUILD)/demo.wav

web:
	./web/build.sh

firmware:
	./firmware/build.sh

clean:
	rm -rf $(BUILD)
