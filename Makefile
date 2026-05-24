# ESP32 CRT Signal Core — convenience targets
# Requires ESP-IDF sourced in shell: . $IDF_PATH/export.sh

PORT ?= /dev/ttyUSB0
HW_PORT ?= /dev/ttyACM0
BAUD ?= 115200

# ── Build ────────────────────────────────────────────────────────────

.PHONY: build flash monitor clean menuconfig fullclean hw-compose-smoke hw-compose-stress hw-compose-pal-stress

build:
	idf.py build

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) -b $(BAUD) monitor

fm: flash monitor  ## Flash + monitor (most common)
	@true

hw-compose-smoke:  ## Build, flash, and validate COMPOSE diagnostics on ESP32 hardware
	@EXPECT_ACTIVE_SPRITES=3 EXPECT_PPU_PENDING=3 PORT="$(HW_PORT)" tools/hw/compose_smoke.sh

hw-compose-stress:  ## Build, flash, and validate worst-case COMPOSE diagnostics on ESP32 hardware
	@RUN_NAME="compose_stress" COMPOSE_STRESS=1 BUILD_DIR="build/compose-stress" \
		SDKCONFIG_TMP="/tmp/esp32-crt-compose-stress-sdkconfig" \
		MONITOR_LOG="/tmp/esp32-crt-compose-stress-monitor.log" \
		MONITOR_SECONDS=55 MIN_WINDOWS=8 \
		EXPECT_ACTIVE_SPRITES=8 EXPECT_SPRITE_PEAK=8 EXPECT_PPU_PENDING=8 \
		PORT="$(HW_PORT)" tools/hw/compose_smoke.sh

hw-compose-pal-stress:  ## Build, flash, and validate PAL worst-case COMPOSE diagnostics on ESP32 hardware
	@RUN_NAME="compose_pal_stress" VIDEO_STANDARD="PAL" COMPOSE_STRESS=1 \
		BUILD_DIR="build/compose-pal-stress" \
		SDKCONFIG_TMP="/tmp/esp32-crt-compose-pal-stress-sdkconfig" \
		MONITOR_LOG="/tmp/esp32-crt-compose-pal-stress-monitor.log" \
		MONITOR_SECONDS=55 MIN_WINDOWS=8 \
		EXPECT_ACTIVE_SPRITES=8 EXPECT_SPRITE_PEAK=8 EXPECT_PPU_PENDING=8 \
		PORT="$(HW_PORT)" tools/hw/compose_smoke.sh

clean:
	idf.py fullclean

menuconfig:
	idf.py menuconfig

# ── Host Tests ───────────────────────────────────────────────────────

TEST_CC     ?= gcc
TEST_CFLAGS ?= -Wall -Wextra -Wno-unused-function -std=c11 -g
TEST_INC    := -I tests/stubs \
               -I components/crt_core/include \
               -I components/crt_timing/include \
               -I components/crt_demo/include \
               -I components/crt_fb/include \
               -I components/crt_compose/include \
               -I components/crt_stimulus/include \
               -I components/crt_tile/include \
               -I components/crt_ppu/include \
               -I components/crt_hal/include
TEST_OUT    := /tmp
LINT_SOURCES := components/crt_core/crt_waveform.c \
                components/crt_core/crt_line_policy.c \
                components/crt_core/crt_composite_palette.c \
                components/crt_hal/crt_hal_clock.c \
                components/crt_timing/crt_timing.c \
                components/crt_demo/crt_demo_pattern.c \
                components/crt_fb/crt_fb.c \
                components/crt_compose/crt_compose.c \
                components/crt_compose/crt_compose_layers.c \
                components/crt_compose/crt_sprite.c \
                components/crt_tile/crt_tile.c \
                components/crt_ppu/crt_ppu.c \
                components/crt_stimulus/crt_stimulus.c

.PHONY: test test-core test-render test-burst test-policy test-timing test-demo test-hal-clock \
        test-composite-palette test-scanline-abi test-scanline-header test-fb test-compose \
        test-stimulus test-tile test-ppu

test: test-core test-render  ## Run all host tests
	@echo "\n✓ All tests passed"

test-core: test-burst test-policy test-timing test-demo test-hal-clock test-composite-palette test-scanline-abi test-scanline-header  ## Run core host tests
	@echo "\n✓ Core tests passed"

test-render: test-fb test-compose test-stimulus test-tile test-ppu  ## Run render adapter host tests
	@echo "\n✓ Render tests passed"

test-burst:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/burst_waveform_test.c components/crt_core/crt_waveform.c \
		-lm -o $(TEST_OUT)/burst_test && $(TEST_OUT)/burst_test

test-policy:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/line_policy_test.c components/crt_core/crt_line_policy.c \
		-o $(TEST_OUT)/policy_test && $(TEST_OUT)/policy_test

test-timing:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_timing_profile_test.c components/crt_timing/crt_timing.c \
		-o $(TEST_OUT)/timing_test && $(TEST_OUT)/timing_test

test-demo:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_demo_pattern_test.c components/crt_demo/crt_demo_pattern.c \
		components/crt_core/crt_waveform.c -lm \
		-o $(TEST_OUT)/demo_test && $(TEST_OUT)/demo_test

test-hal-clock:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_hal_clock_test.c components/crt_hal/crt_hal_clock.c \
		-o $(TEST_OUT)/crt_hal_clock_test && $(TEST_OUT)/crt_hal_clock_test

test-composite-palette:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_composite_palette_test.c components/crt_core/crt_composite_palette.c \
		-o $(TEST_OUT)/crt_composite_palette_test && $(TEST_OUT)/crt_composite_palette_test

test-scanline-abi:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_scanline_abi_test.c \
		-o $(TEST_OUT)/scanline_abi_test && $(TEST_OUT)/scanline_abi_test

test-scanline-header:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_scanline_header_test.c \
		-o $(TEST_OUT)/scanline_header_test && $(TEST_OUT)/scanline_header_test

test-fb:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_fb_test.c components/crt_fb/crt_fb.c components/crt_core/crt_composite_palette.c \
		-o $(TEST_OUT)/crt_fb_test && $(TEST_OUT)/crt_fb_test

test-compose:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_compose_test.c components/crt_compose/crt_compose.c \
		components/crt_compose/crt_compose_layers.c components/crt_compose/crt_sprite.c \
		components/crt_tile/crt_tile.c components/crt_core/crt_composite_palette.c \
		-o $(TEST_OUT)/crt_compose_test && $(TEST_OUT)/crt_compose_test

test-stimulus:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_stimulus_test.c components/crt_stimulus/crt_stimulus.c \
		-o $(TEST_OUT)/crt_stimulus_test && $(TEST_OUT)/crt_stimulus_test

test-tile:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_tile_test.c components/crt_tile/crt_tile.c \
		-o $(TEST_OUT)/crt_tile_test && $(TEST_OUT)/crt_tile_test

test-ppu:
	@$(TEST_CC) $(TEST_CFLAGS) $(TEST_INC) \
		tests/crt_ppu_test.c components/crt_ppu/crt_ppu.c components/crt_tile/crt_tile.c \
		components/crt_compose/crt_compose.c components/crt_compose/crt_sprite.c \
		components/crt_core/crt_composite_palette.c \
		-o $(TEST_OUT)/crt_ppu_test && $(TEST_OUT)/crt_ppu_test

# ── Lint / Format ────────────────────────────────────────────────────

PY_SOURCES := tools/

.PHONY: format format-c format-py lint lint-c lint-py

format: format-c format-py  ## Format C + Python sources

CLANG_FORMAT ?= $(shell command -v clang-format-18 \
                          || ls /usr/lib/llvm18/bin/clang-format 2>/dev/null \
                          || command -v clang-format)
# tile_demo.h is static data; clang-format-18's heuristic misreads it as
# Objective-C. Excluded from formatting; CI also tolerates that warning.
C_FORMAT_SOURCES := $(shell find components main -name '*.c' -o -name '*.h' \
                              | grep -v 'main/tile_demo.h')

format-c:  ## clang-format on components/ + main/ (prefers clang-format-18 to match CI)
	@$(CLANG_FORMAT) -i $(C_FORMAT_SOURCES)
	@echo "✓ C formatted (using $(CLANG_FORMAT))"

format-py:  ## ruff format + import sort on tools/
	@uv run --with ruff ruff format $(PY_SOURCES)
	@uv run --with ruff ruff check --fix --select I $(PY_SOURCES)
	@echo "✓ Python formatted"

lint: lint-c lint-py  ## Lint C + Python sources

lint-c:  ## clang-tidy + clang-format-18 dry-run check
	@clang-tidy $(LINT_SOURCES) --quiet -- $(TEST_CFLAGS) $(TEST_INC)
	@$(CLANG_FORMAT) --dry-run -Werror $(C_FORMAT_SOURCES)
	@echo "✓ C lint done"

lint-py:  ## ruff check on tools/
	@uv run --with ruff ruff check $(PY_SOURCES)
	@echo "✓ Python lint done"

# ── Help ─────────────────────────────────────────────────────────────

.PHONY: help
help:  ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?##' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}'
