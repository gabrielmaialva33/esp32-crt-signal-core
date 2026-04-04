# Measurement Brutalism Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first authored signal-art suite for the ESP32 CRT Signal Core as a luma-only showcase with `perform` and `hold_probe` modes over the existing `crt_fb + scanline hook` path.

**Architecture:** Add a small `signal_suite` component that owns suite state, pattern generation, framebuffer rendering, and serial control. Keep milestone 1 additive: `crt_core` continues owning sync/burst/line scheduling, while `main/app_main.c` swaps the calibration-pattern boot path for suite initialization, framebuffer registration, and serial-driven mode/preset control. To keep probe states deterministic, serial input must only stage pending commands; the frame hook applies those staged changes and triggers framebuffer re-rendering exactly at frame boundaries.

**Tech Stack:** C11, ESP-IDF 5.4, existing `crt_core`/`crt_fb` hook ABI, host-compiled `assert.h` tests, UART/serial control via ESP-IDF console/UART APIs

---

## File Map

| File | Responsibility |
|------|----------------|
| `components/signal_suite/CMakeLists.txt` | Build registration for the suite component |
| `components/signal_suite/include/signal_suite.h` | Public API: init, frame tick, mode/movement/preset selection, state query |
| `components/signal_suite/include/signal_suite_patterns.h` | Pattern and preset declarations for staircase/ramp/transition movements |
| `components/signal_suite/include/signal_suite_serial.h` | Narrow serial command surface for mode/movement/preset control |
| `components/signal_suite/signal_suite.c` | Runtime state machine, framebuffer ownership, movement scheduler, frame hook |
| `components/signal_suite/signal_suite_patterns.c` | Deterministic row/preset generation for lock/staircase/ramp/transition stress |
| `components/signal_suite/signal_suite_serial.c` | Serial command parser and machine-readable state-line formatter |
| `main/CMakeLists.txt` | Add `signal_suite` to the main component dependency list |
| `main/app_main.c` | Replace calibration bootstrap with suite bootstrap and serial polling task |
| `tests/signal_suite_pattern_test.c` | Host tests for deterministic presets and row generation |
| `tests/signal_suite_state_test.c` | Host tests for mode/movement transitions and frame scheduling |
| `tests/signal_suite_serial_test.c` | Host tests for command parsing and state-line formatting |
| `tools/analysis/signal_suite_manifest.json` | Compact movement/preset table consumed by host tools |
| `tools/analysis/export_signal_suite_manifest.py` | Emit host-readable movement/preset manifest from firmware-facing names |
| `tools/analysis/report_transfer_curve.py` | Generate transfer-curve report from captured electrical samples |
| `tools/analysis/report_transition_metrics.py` | Generate overshoot/settling report from captured electrical samples |
| `README.md` | Quick-start and suite operation notes |
| `docs/superpowers/specs/2026-04-04-measurement-brutalism-suite-design.md` | Approved spec for reference only; do not modify during implementation unless requirements change |

---

### Task 1: Scaffold `signal_suite` Component and Public Types

**Files:**
- Create: `components/signal_suite/CMakeLists.txt`
- Create: `components/signal_suite/include/signal_suite.h`
- Create: `components/signal_suite/include/signal_suite_patterns.h`
- Create: `components/signal_suite/include/signal_suite_serial.h`
- Create: `components/signal_suite/signal_suite.c`
- Create: `components/signal_suite/signal_suite_patterns.c`
- Create: `components/signal_suite/signal_suite_serial.c`
- Modify: `main/CMakeLists.txt`
- Test: `tests/signal_suite_state_test.c`

- [ ] **Step 1: Write the failing state/API test**

```c
#include <assert.h>
#include <string.h>

#include "signal_suite.h"

int main(void)
{
    signal_suite_state_t state;
    signal_suite_config_t cfg = {
        .logical_width = 256,
        .logical_height = 240,
        .perform_dwell_frames = 30,
    };

    assert(signal_suite_init(NULL, &cfg) == ESP_ERR_INVALID_ARG);
    assert(signal_suite_init(&state, &cfg) == ESP_OK);
    assert(state.mode == SIGNAL_SUITE_MODE_PERFORM);
    assert(state.movement == SIGNAL_SUITE_MOVEMENT_LOCK);
    assert(strcmp(signal_suite_mode_name(state.mode), "perform") == 0);
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c \
    -o /tmp/signal_suite_state_test
```

Expected: compile failure because the `signal_suite` headers and implementation do not exist yet.

- [ ] **Step 3: Create the component skeleton**

`components/signal_suite/CMakeLists.txt`
```cmake
idf_component_register(
    SRCS
        "signal_suite.c"
        "signal_suite_patterns.c"
        "signal_suite_serial.c"
    INCLUDE_DIRS "include"
    REQUIRES crt_fb crt_core
)
```

`components/signal_suite/include/signal_suite.h`
```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "crt_fb.h"

typedef enum {
    SIGNAL_SUITE_MODE_PERFORM = 0,
    SIGNAL_SUITE_MODE_HOLD_PROBE = 1,
} signal_suite_mode_t;

typedef enum {
    SIGNAL_SUITE_MOVEMENT_LOCK = 0,
    SIGNAL_SUITE_MOVEMENT_STAIRCASE = 1,
    SIGNAL_SUITE_MOVEMENT_RAMP = 2,
    SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS = 3,
} signal_suite_movement_t;

typedef struct {
    uint16_t logical_width;
    uint16_t logical_height;
    uint16_t perform_dwell_frames;
    crt_video_standard_t video_standard;
} signal_suite_config_t;

typedef struct {
    signal_suite_mode_t mode;
    signal_suite_movement_t movement;
    uint16_t preset_index;
    uint32_t frame_number;
    bool color_enabled;
    uint16_t logical_width;
    uint16_t logical_height;
    uint16_t perform_dwell_frames;
    crt_video_standard_t video_standard;
    bool pending_valid;
    signal_suite_mode_t pending_mode;
    signal_suite_movement_t pending_movement;
    uint16_t pending_preset_index;
    crt_fb_surface_t *surface;
} signal_suite_state_t;

esp_err_t signal_suite_init(signal_suite_state_t *state, const signal_suite_config_t *config);
const char *signal_suite_mode_name(signal_suite_mode_t mode);
const char *signal_suite_movement_name(signal_suite_movement_t movement);
signal_suite_movement_t signal_suite_next_movement(signal_suite_movement_t movement);
```

`components/signal_suite/signal_suite.c`
```c
#include "signal_suite.h"

#include <string.h>

esp_err_t signal_suite_init(signal_suite_state_t *state, const signal_suite_config_t *config)
{
    if (state == NULL || config == NULL) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    state->mode = SIGNAL_SUITE_MODE_PERFORM;
    state->movement = SIGNAL_SUITE_MOVEMENT_LOCK;
    state->logical_width = config->logical_width;
    state->logical_height = config->logical_height;
    state->perform_dwell_frames = config->perform_dwell_frames;
    state->video_standard = config->video_standard;
    return ESP_OK;
}

const char *signal_suite_mode_name(signal_suite_mode_t mode)
{
    return mode == SIGNAL_SUITE_MODE_HOLD_PROBE ? "hold_probe" : "perform";
}

const char *signal_suite_movement_name(signal_suite_movement_t movement)
{
    switch (movement) {
    case SIGNAL_SUITE_MOVEMENT_LOCK: return "lock";
    case SIGNAL_SUITE_MOVEMENT_STAIRCASE: return "staircase";
    case SIGNAL_SUITE_MOVEMENT_RAMP: return "ramp";
    case SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS: return "transition_stress";
    default: return "unknown";
    }
}
```

Add stub declarations in `signal_suite_patterns.h` and `signal_suite_serial.h` sufficient for compilation.

Update `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "app_main.c"
                       REQUIRES crt_core crt_demo crt_diag crt_fb crt_hal crt_timing signal_suite
                       INCLUDE_DIRS ".")
```

- [ ] **Step 4: Run the test to verify it passes**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c \
    -o /tmp/signal_suite_state_test && /tmp/signal_suite_state_test
```

Expected: exits `0`.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite tests/signal_suite_state_test.c
git commit -m "feat(signal_suite): scaffold suite component and public types"
```

---

### Task 2: Implement Deterministic Pattern and Preset Generation

**Files:**
- Modify: `components/signal_suite/include/signal_suite_patterns.h`
- Modify: `components/signal_suite/signal_suite_patterns.c`
- Test: `tests/signal_suite_pattern_test.c`

- [ ] **Step 1: Write the failing pattern test**

```c
#include <assert.h>
#include <stdint.h>

#include "signal_suite_patterns.h"

int main(void)
{
    uint8_t row[16];

    signal_suite_pattern_fill_staircase_row(row, 16, 4);
    assert(row[0] == 0);
    assert(row[3] == 0);
    assert(row[4] > row[3]);
    assert(row[15] == 255);

    signal_suite_pattern_fill_transition_row(row, 16, 255, 0, 8);
    assert(row[7] == 255);
    assert(row[8] == 0);
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include \
    tests/signal_suite_pattern_test.c components/signal_suite/signal_suite_patterns.c \
    -o /tmp/signal_suite_pattern_test
```

Expected: compile failure because the pattern functions are not implemented.

- [ ] **Step 3: Implement the minimal deterministic generators**

`components/signal_suite/include/signal_suite_patterns.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>

void signal_suite_pattern_fill_lock_row(uint8_t *row, size_t width, uint8_t level);
void signal_suite_pattern_fill_staircase_row(uint8_t *row, size_t width, uint8_t steps);
void signal_suite_pattern_fill_ramp_row(uint8_t *row, size_t width, uint8_t lo, uint8_t hi);
void signal_suite_pattern_fill_transition_row(uint8_t *row, size_t width, uint8_t left, uint8_t right, uint16_t edge_x);
```

`components/signal_suite/signal_suite_patterns.c`
```c
#include "signal_suite_patterns.h"

void signal_suite_pattern_fill_lock_row(uint8_t *row, size_t width, uint8_t level)
{
    for (size_t i = 0; i < width; ++i) row[i] = level;
}

void signal_suite_pattern_fill_staircase_row(uint8_t *row, size_t width, uint8_t steps)
{
    size_t step_width = width / steps;
    for (size_t x = 0; x < width; ++x) {
        uint8_t bucket = (uint8_t)(x / step_width);
        if (bucket >= steps) bucket = steps - 1;
        row[x] = (uint8_t)((255U * bucket) / (steps - 1U));
    }
}

void signal_suite_pattern_fill_ramp_row(uint8_t *row, size_t width, uint8_t lo, uint8_t hi)
{
    for (size_t x = 0; x < width; ++x) {
        row[x] = (uint8_t)(lo + ((uint32_t)x * (hi - lo)) / (width - 1U));
    }
}

void signal_suite_pattern_fill_transition_row(uint8_t *row, size_t width, uint8_t left, uint8_t right, uint16_t edge_x)
{
    for (size_t x = 0; x < width; ++x) row[x] = (x < edge_x) ? left : right;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include \
    tests/signal_suite_pattern_test.c components/signal_suite/signal_suite_patterns.c \
    -o /tmp/signal_suite_pattern_test && /tmp/signal_suite_pattern_test
```

Expected: exits `0`.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite/include/signal_suite_patterns.h components/signal_suite/signal_suite_patterns.c tests/signal_suite_pattern_test.c
git commit -m "feat(signal_suite): add deterministic pattern generators"
```

---

### Task 3: Implement Suite State Machine and `perform` Scheduling

**Files:**
- Modify: `components/signal_suite/include/signal_suite.h`
- Modify: `components/signal_suite/signal_suite.c`
- Test: `tests/signal_suite_state_test.c`

- [ ] **Step 1: Extend the failing state test**

Add to `tests/signal_suite_state_test.c`:

```c
    assert(signal_suite_set_mode(&state, SIGNAL_SUITE_MODE_PERFORM) == ESP_OK);
    assert(signal_suite_set_movement(&state, SIGNAL_SUITE_MOVEMENT_STAIRCASE) == ESP_OK);
    assert(signal_suite_step_frame(&state) == ESP_OK);
    assert(state.frame_number == 1);

    state.mode = SIGNAL_SUITE_MODE_PERFORM;
    state.movement = SIGNAL_SUITE_MOVEMENT_LOCK;
    state.frame_number = 29;
    assert(signal_suite_step_frame(&state) == ESP_OK);
    assert(state.movement == SIGNAL_SUITE_MOVEMENT_STAIRCASE);
```

- [ ] **Step 2: Run the test to verify it fails**

Run the same command from Task 1, Step 4.

Expected: compile failure for missing setters / frame-step functions or assertion failure because movement scheduling is not implemented.

- [ ] **Step 3: Implement the minimal state machine**

Add to `components/signal_suite/include/signal_suite.h`:

```c
esp_err_t signal_suite_set_mode(signal_suite_state_t *state, signal_suite_mode_t mode);
esp_err_t signal_suite_set_movement(signal_suite_state_t *state, signal_suite_movement_t movement);
esp_err_t signal_suite_set_preset(signal_suite_state_t *state, uint16_t preset_index);
esp_err_t signal_suite_step_frame(signal_suite_state_t *state);
```

Implement in `components/signal_suite/signal_suite.c`:

```c
signal_suite_movement_t signal_suite_next_movement(signal_suite_movement_t movement)
{
    switch (movement) {
    case SIGNAL_SUITE_MOVEMENT_LOCK: return SIGNAL_SUITE_MOVEMENT_STAIRCASE;
    case SIGNAL_SUITE_MOVEMENT_STAIRCASE: return SIGNAL_SUITE_MOVEMENT_RAMP;
    case SIGNAL_SUITE_MOVEMENT_RAMP: return SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS;
    default: return SIGNAL_SUITE_MOVEMENT_LOCK;
    }
}

esp_err_t signal_suite_set_mode(signal_suite_state_t *state, signal_suite_mode_t mode)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    state->mode = mode;
    return ESP_OK;
}

esp_err_t signal_suite_set_movement(signal_suite_state_t *state, signal_suite_movement_t movement)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    state->movement = movement;
    state->preset_index = 0;
    return ESP_OK;
}

esp_err_t signal_suite_set_preset(signal_suite_state_t *state, uint16_t preset_index)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    state->preset_index = preset_index;
    return ESP_OK;
}

esp_err_t signal_suite_step_frame(signal_suite_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    state->frame_number++;
    if (state->mode == SIGNAL_SUITE_MODE_PERFORM &&
        state->perform_dwell_frames > 0 &&
        (state->frame_number % state->perform_dwell_frames) == 0U) {
        state->movement = signal_suite_next_movement(state->movement);
        state->preset_index = 0;
    }
    return ESP_OK;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run the Task 1 state-test command again.

Expected: exits `0`.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite/include/signal_suite.h components/signal_suite/signal_suite.c tests/signal_suite_state_test.c
git commit -m "feat(signal_suite): add perform scheduling state machine"
```

---

### Task 4: Render Movements into `crt_fb` and Register Frame/Scanline Hooks

**Files:**
- Modify: `components/signal_suite/include/signal_suite.h`
- Modify: `components/signal_suite/signal_suite.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/app_main.c`
- Test: `tests/signal_suite_state_test.c`

- [ ] **Step 1: Add a failing render/integration test**

Append to `tests/signal_suite_state_test.c`:

```c
    crt_fb_surface_t fb = {
        .width = 16,
        .height = 4,
        .format = CRT_FB_FORMAT_INDEXED8,
    };
    uint8_t buffer[64] = {0};
    uint16_t palette[256] = {0};
    fb.buffer = buffer;
    fb.buffer_size = sizeof(buffer);
    fb.palette = palette;
    fb.palette_size = 256;

    state.surface = &fb;
    assert(signal_suite_render_frame(&state) == ESP_OK);
    assert(buffer[0] == 0);
```

- [ ] **Step 2: Run the test to verify it fails**

Run the Task 1 state-test command again.

Expected: compile failure for missing `signal_suite_render_frame()` or runtime failure because rendering is not implemented.

- [ ] **Step 3: Implement framebuffer rendering and app integration**

Add to `components/signal_suite/include/signal_suite.h`:

```c
esp_err_t signal_suite_render_frame(signal_suite_state_t *state);
esp_err_t signal_suite_apply_pending(signal_suite_state_t *state);
void signal_suite_frame_hook(uint32_t frame_number, void *user_data);
```

Implement in `components/signal_suite/signal_suite.c`:

```c
esp_err_t signal_suite_render_frame(signal_suite_state_t *state)
{
    uint8_t *row;
    if (state == NULL || state->surface == NULL || state->surface->buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t y = 0; y < state->surface->height; ++y) {
        row = crt_fb_row(state->surface, y);
        switch (state->movement) {
        case SIGNAL_SUITE_MOVEMENT_LOCK:
            signal_suite_pattern_fill_lock_row(row, state->surface->width, 0);
            break;
        case SIGNAL_SUITE_MOVEMENT_STAIRCASE:
            signal_suite_pattern_fill_staircase_row(row, state->surface->width, 8);
            break;
        case SIGNAL_SUITE_MOVEMENT_RAMP:
            signal_suite_pattern_fill_ramp_row(row, state->surface->width, 0, 255);
            break;
        case SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS:
            signal_suite_pattern_fill_transition_row(row, state->surface->width, 255, 0, state->surface->width / 2U);
            break;
        }
    }
    return ESP_OK;
}

esp_err_t signal_suite_apply_pending(signal_suite_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    if (!state->pending_valid) return ESP_OK;
    state->mode = state->pending_mode;
    state->movement = state->pending_movement;
    state->preset_index = state->pending_preset_index;
    state->pending_valid = false;
    return signal_suite_render_frame(state);
}

void signal_suite_frame_hook(uint32_t frame_number, void *user_data)
{
    signal_suite_state_t *state = (signal_suite_state_t *)user_data;
    if (state == NULL) return;
    /* Apply staged serial changes only at frame boundaries. */
    signal_suite_apply_pending(state);
    state->frame_number = frame_number;
    if (signal_suite_step_frame(state) == ESP_OK) {
        signal_suite_render_frame(state);
    }
}
```

Modify `main/CMakeLists.txt` to add `signal_suite` to `REQUIRES`.

Modify `main/app_main.c` to:

- create `signal_suite_state_t s_suite`
- allocate `crt_fb_surface_t`
- initialize grayscale palette
- call `signal_suite_init(&s_suite, &cfg)`
- set `s_suite.surface = &s_fb`
- call `signal_suite_render_frame(&s_suite)` before `crt_core_start()`
- register:

```c
crt_register_frame_hook(signal_suite_frame_hook, &s_suite);
crt_register_scanline_hook(crt_fb_scanline_hook, &s_fb);
```

- [ ] **Step 4: Run host test and firmware build**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c components/crt_fb/crt_fb.c \
    -o /tmp/signal_suite_state_test && /tmp/signal_suite_state_test
```

Then run:

```bash
idf.py build
```

Expected: host test exits `0`; firmware build succeeds.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite main/CMakeLists.txt main/app_main.c tests/signal_suite_state_test.c
git commit -m "feat(signal_suite): render suite through framebuffer hooks"
```

---

### Task 5: Add `hold_probe` Presets and Machine-Readable State Reporting

**Files:**
- Modify: `components/signal_suite/include/signal_suite.h`
- Modify: `components/signal_suite/include/signal_suite_serial.h`
- Modify: `components/signal_suite/signal_suite.c`
- Modify: `components/signal_suite/signal_suite_serial.c`
- Test: `tests/signal_suite_serial_test.c`

- [ ] **Step 1: Write the failing serial/state-format test**

```c
#include <assert.h>
#include <string.h>

#include "signal_suite.h"
#include "signal_suite_serial.h"

int main(void)
{
    signal_suite_state_t state = {
        .mode = SIGNAL_SUITE_MODE_HOLD_PROBE,
        .movement = SIGNAL_SUITE_MOVEMENT_STAIRCASE,
        .preset_index = 1,
        .frame_number = 42,
        .color_enabled = false,
        .video_standard = CRT_VIDEO_STANDARD_PAL,
    };
    char line[128];

    assert(signal_suite_format_state_line(&state, line, sizeof(line)) > 0);
    assert(strstr(line, "mode=hold_probe") != NULL);
    assert(strstr(line, "movement=staircase") != NULL);
    assert(strstr(line, "frame=42") != NULL);
    assert(strstr(line, "standard=pal") != NULL);
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_serial_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c \
    -o /tmp/signal_suite_serial_test
```

Expected: compile failure because formatter helpers are not implemented.

- [ ] **Step 3: Implement preset names and state-line formatter**

Add to `components/signal_suite/include/signal_suite_serial.h`:

```c
#pragma once

#include <stddef.h>
#include <inttypes.h>

#include "signal_suite.h"

size_t signal_suite_format_state_line(const signal_suite_state_t *state, char *buf, size_t buf_size);
const char *signal_suite_preset_name(signal_suite_movement_t movement, uint16_t preset_index);
size_t signal_suite_format_manifest_line(signal_suite_movement_t movement, uint16_t preset_index,
                                         char *buf, size_t buf_size);
```

Implement in `components/signal_suite/signal_suite_serial.c`:

```c
#include "signal_suite_serial.h"

#include <stdio.h>

const char *signal_suite_preset_name(signal_suite_movement_t movement, uint16_t preset_index)
{
    switch (movement) {
    case SIGNAL_SUITE_MOVEMENT_LOCK: return "lock_baseline";
    case SIGNAL_SUITE_MOVEMENT_STAIRCASE: return preset_index == 1 ? "staircase_16" : "staircase_8";
    case SIGNAL_SUITE_MOVEMENT_RAMP: return "ramp_full";
    case SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS: return "transition_255_0";
    default: return "unknown";
    }
}

size_t signal_suite_format_state_line(const signal_suite_state_t *state, char *buf, size_t buf_size)
{
    return (size_t)snprintf(
        buf, buf_size,
        "SUITE_STATE mode=%s movement=%s preset=%s frame=%" PRIu32 " color=%s standard=%s",
        signal_suite_mode_name(state->mode),
        signal_suite_movement_name(state->movement),
        signal_suite_preset_name(state->movement, state->preset_index),
        state->frame_number,
        state->color_enabled ? "on" : "off",
        state->video_standard == CRT_VIDEO_STANDARD_PAL ? "pal" : "ntsc");
}

size_t signal_suite_format_manifest_line(signal_suite_movement_t movement, uint16_t preset_index,
                                         char *buf, size_t buf_size)
{
    return (size_t)snprintf(buf, buf_size,
        "SUITE_PRESET movement=%s preset=%s index=%u",
        signal_suite_movement_name(movement),
        signal_suite_preset_name(movement, preset_index),
        (unsigned)preset_index);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run the command from Step 2 with `&& /tmp/signal_suite_serial_test`.

Expected: exits `0`.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite/include/signal_suite_serial.h components/signal_suite/signal_suite_serial.c tests/signal_suite_serial_test.c
git commit -m "feat(signal_suite): add preset naming and state-line reporting"
```

---

### Task 6: Implement UART/Serial Command Control

**Files:**
- Modify: `components/signal_suite/include/signal_suite_serial.h`
- Modify: `components/signal_suite/signal_suite_serial.c`
- Modify: `main/app_main.c`
- Test: `tests/signal_suite_serial_test.c`

- [ ] **Step 1: Extend the failing serial test with parser expectations**

Add to `tests/signal_suite_serial_test.c`:

```c
    signal_suite_state_t state = {0};
    uint8_t buffer[256] = {0};
    uint16_t palette[256] = {0};
    crt_fb_surface_t fb = {
        .width = 16,
        .height = 16,
        .format = CRT_FB_FORMAT_INDEXED8,
        .buffer = buffer,
        .buffer_size = sizeof(buffer),
        .palette = palette,
        .palette_size = 256,
    };
    assert(signal_suite_init(&state, &(signal_suite_config_t){ .logical_width = 256, .logical_height = 240, .perform_dwell_frames = 30, .video_standard = CRT_VIDEO_STANDARD_NTSC }) == ESP_OK);
    state.surface = &fb;
    assert(signal_suite_apply_command(&state, "mode hold_probe") == ESP_OK);
    assert(state.pending_valid);
    assert(state.pending_mode == SIGNAL_SUITE_MODE_HOLD_PROBE);
    assert(signal_suite_apply_command(&state, "movement ramp") == ESP_OK);
    assert(state.pending_movement == SIGNAL_SUITE_MOVEMENT_RAMP);
    assert(signal_suite_apply_command(&state, "preset 1") == ESP_OK);
    assert(state.pending_preset_index == 1);
    assert(signal_suite_apply_pending(&state) == ESP_OK);
    assert(state.mode == SIGNAL_SUITE_MODE_HOLD_PROBE);
    assert(state.movement == SIGNAL_SUITE_MOVEMENT_RAMP);
    assert(state.preset_index == 1);
    assert(signal_suite_apply_command(&state, "query") == ESP_OK);
    assert(signal_suite_apply_command(&state, "perform next") == ESP_OK);
    assert(signal_suite_apply_command(&state, "perform reset") == ESP_OK);
    assert(signal_suite_apply_command(&state, "list") == ESP_OK);
```

- [ ] **Step 2: Run the test to verify it fails**

Run the command from Task 5, Step 2 again.

Expected: compile failure for missing command parser or assertion failure because command handling is not implemented.

- [ ] **Step 3: Implement the narrow command parser and app polling**

Add to `components/signal_suite/include/signal_suite_serial.h`:

```c
esp_err_t signal_suite_apply_command(signal_suite_state_t *state, const char *line);
```

Implement in `components/signal_suite/signal_suite_serial.c`:

```c
#include <string.h>

esp_err_t signal_suite_apply_command(signal_suite_state_t *state, const char *line)
{
    if (state == NULL || line == NULL) return ESP_ERR_INVALID_ARG;
    signal_suite_mode_t staged_mode =
        state->pending_valid ? state->pending_mode : state->mode;
    signal_suite_movement_t staged_movement =
        state->pending_valid ? state->pending_movement : state->movement;
    uint16_t staged_preset =
        state->pending_valid ? state->pending_preset_index : state->preset_index;

    if (strcmp(line, "mode perform") == 0) {
        state->pending_valid = true;
        state->pending_mode = SIGNAL_SUITE_MODE_PERFORM;
        state->pending_movement = staged_movement;
        state->pending_preset_index = staged_preset;
        return ESP_OK;
    }
    if (strcmp(line, "mode hold_probe") == 0) {
        state->pending_valid = true;
        state->pending_mode = SIGNAL_SUITE_MODE_HOLD_PROBE;
        state->pending_movement = staged_movement;
        state->pending_preset_index = staged_preset;
        return ESP_OK;
    }
    if (strcmp(line, "movement lock") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = SIGNAL_SUITE_MOVEMENT_LOCK; state->pending_preset_index = 0; return ESP_OK; }
    if (strcmp(line, "movement staircase") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = SIGNAL_SUITE_MOVEMENT_STAIRCASE; state->pending_preset_index = 0; return ESP_OK; }
    if (strcmp(line, "movement ramp") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = SIGNAL_SUITE_MOVEMENT_RAMP; state->pending_preset_index = 0; return ESP_OK; }
    if (strcmp(line, "movement transition_stress") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = SIGNAL_SUITE_MOVEMENT_TRANSITION_STRESS; state->pending_preset_index = 0; return ESP_OK; }
    if (strcmp(line, "preset 0") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = staged_movement; state->pending_preset_index = 0; return ESP_OK; }
    if (strcmp(line, "preset 1") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = staged_movement; state->pending_preset_index = 1; return ESP_OK; }
    if (strcmp(line, "preset 2") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = staged_movement; state->pending_preset_index = 2; return ESP_OK; }
    if (strcmp(line, "preset 3") == 0) { state->pending_valid = true; state->pending_mode = staged_mode; state->pending_movement = staged_movement; state->pending_preset_index = 3; return ESP_OK; }
    if (strcmp(line, "query") == 0) return ESP_OK;
    if (strcmp(line, "perform next") == 0) {
        state->pending_valid = true;
        state->pending_mode = staged_mode;
        state->pending_movement = signal_suite_next_movement(state->movement);
        state->pending_preset_index = 0;
        return ESP_OK;
    }
    if (strcmp(line, "perform reset") == 0) {
        state->pending_valid = true;
        state->pending_mode = SIGNAL_SUITE_MODE_PERFORM;
        state->pending_movement = SIGNAL_SUITE_MOVEMENT_LOCK;
        state->pending_preset_index = 0;
        return ESP_OK;
    }
    if (strcmp(line, "list") == 0) return ESP_OK;
    return ESP_ERR_NOT_SUPPORTED;
}
```

Modify `main/app_main.c` to add a low-priority serial-control task. The minimal implementation can:

- read lines from UART/console
- call `signal_suite_apply_command(&s_suite, line)` only to stage the desired change
- for `query`, print the current `SUITE_STATE ...` line
- for `list`, print all `SUITE_PRESET ...` lines by iterating the compact movement/preset table
- never call `signal_suite_render_frame()` directly from the serial task; the frame hook owns render commits

Keep the task fully outside the hot path.

- [ ] **Step 4: Run the test and firmware build**

Run the Task 5 serial-test command with `&& /tmp/signal_suite_serial_test`.

Then run:

```bash
idf.py build
```

Expected: host test exits `0`; firmware build succeeds.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite main/app_main.c tests/signal_suite_serial_test.c
git commit -m "feat(signal_suite): add serial command control"
```

---

### Task 7: Add Probe Presets for Tonal and Transition Workflows

**Files:**
- Modify: `components/signal_suite/signal_suite.c`
- Modify: `components/signal_suite/signal_suite_patterns.c`
- Modify: `components/signal_suite/signal_suite_serial.c`
- Test: `tests/signal_suite_pattern_test.c`
- Test: `tests/signal_suite_state_test.c`

- [ ] **Step 1: Add failing assertions for multiple presets**

Extend `tests/signal_suite_pattern_test.c` and `tests/signal_suite_state_test.c` so they verify:

- staircase has both `staircase_8` and `staircase_16`
- ramp supports at least `ramp_full`, `ramp_window_low`, `ramp_window_mid`, `ramp_window_high`
- transition stress supports at least `transition_255_0` and `transition_192_0`

Example assertion:

```c
assert(strcmp(signal_suite_preset_name(SIGNAL_SUITE_MOVEMENT_RAMP, 2), "ramp_window_mid") == 0);
```

- [ ] **Step 2: Run both tests to verify they fail**

Run separately:

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_pattern_test.c components/signal_suite/signal_suite_patterns.c \
    -o /tmp/signal_suite_pattern_test && /tmp/signal_suite_pattern_test
```

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c components/crt_fb/crt_fb.c \
    -o /tmp/signal_suite_state_test && /tmp/signal_suite_state_test
```

Expected: at least one test fails because preset coverage is incomplete.

- [ ] **Step 3: Implement the additional preset families**

Implement in `components/signal_suite/signal_suite.c`:

- movement-specific render branches keyed by `preset_index`
- narrow ramp windows by filling the row with low baseline outside a measured window
- alternate transition amplitudes for `255->0` and `192->0`

Implement in `components/signal_suite/signal_suite_serial.c`:

- stable preset-name mapping for each movement
- optional `preset next` helper if it simplifies live probing

Keep preset count intentionally small:

- `lock`: 1 preset
- `staircase`: 2 presets
- `ramp`: 4 presets
- `transition_stress`: 2 presets

- [ ] **Step 4: Run both tests and firmware build**

Run separately:

```bash
gcc -I tests/stubs -I components/signal_suite/include \
    tests/signal_suite_pattern_test.c components/signal_suite/signal_suite_patterns.c \
    -o /tmp/signal_suite_pattern_test && /tmp/signal_suite_pattern_test
```

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c components/crt_fb/crt_fb.c \
    -o /tmp/signal_suite_state_test && /tmp/signal_suite_state_test
```

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_serial_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c \
    -o /tmp/signal_suite_serial_test && /tmp/signal_suite_serial_test
```

Then:

```bash
idf.py build
```

Expected: host tests exit `0`; firmware build succeeds.

- [ ] **Step 5: Commit**

```bash
git add components/signal_suite tests/signal_suite_pattern_test.c tests/signal_suite_state_test.c tests/signal_suite_serial_test.c
git commit -m "feat(signal_suite): add hold-probe preset families"
```

---

### Task 8: Document Operation and Verify End-to-End

**Files:**
- Modify: `README.md`
- Modify: `main/app_main.c` (only if startup logging is still unclear)
- Create: `tools/analysis/export_signal_suite_manifest.py`
- Create: `tools/analysis/signal_suite_manifest.json`
- Create: `tools/analysis/report_transfer_curve.py`
- Create: `tools/analysis/report_transition_metrics.py`

- [ ] **Step 1: Add README usage notes**

Add a short section documenting:

- what the Measurement Brutalism Suite is
- that milestone 1 is luma-only
- how to flash and run it
- available serial commands
- expected `SUITE_STATE ...` output format
- expected `SUITE_PRESET ...` manifest lines

- [ ] **Step 2: Add host metadata and reporting helpers**

Create `tools/analysis/export_signal_suite_manifest.py` to emit a compact JSON manifest matching the firmware-facing movement/preset table.

Minimum output shape:

```json
{
  "suite": "measurement_brutalism",
  "movements": [
    { "name": "lock", "presets": ["lock_baseline"] },
    { "name": "staircase", "presets": ["staircase_8", "staircase_16"] }
  ]
}
```

Create `tools/analysis/report_transfer_curve.py` to:

- ingest captured electrical sample CSV
- ingest the suite manifest plus run metadata JSON
- emit a transfer-curve plot and summary JSON

Create `tools/analysis/report_transition_metrics.py` to:

- ingest transition-capture CSV
- ingest run metadata JSON
- measure overshoot, undershoot, and settling width
- emit plots and summary JSON

Store one generated manifest at `tools/analysis/signal_suite_manifest.json`.

Define the minimum run-metadata artifact as `captures/<run_id>.json`.

Required keys:

```json
{
  "run_id": "2026-04-04T22-15-30Z-staircase16",
  "timestamp": "2026-04-04T22:15:30Z",
  "standard": "ntsc",
  "mode": "hold_probe",
  "movement": "staircase",
  "preset": "staircase_16",
  "color": "off",
  "scope_vertical_scale": "200mV/div",
  "scope_time_scale": "10us/div",
  "capture_hw": "scope+adc"
}
```

Require both report scripts to accept `--manifest` and `--run-metadata`.

- [ ] **Step 3: Build and run the full verification set**

Suggested README snippet:

```markdown
## Measurement Brutalism Suite

The default runtime now boots into the first authored signal-art suite.

Serial commands:
- `mode perform`
- `mode hold_probe`
- `movement lock`
- `movement staircase`
- `movement ramp`
- `movement transition_stress`
- `preset 0`
- `preset 1`
- `preset 2`
- `preset 3`
- `query`
- `list`
- `perform next`
- `perform reset`
```

Run:

```bash
gcc -I tests/stubs -I components/signal_suite/include \
    tests/signal_suite_pattern_test.c components/signal_suite/signal_suite_patterns.c \
    -o /tmp/signal_suite_pattern_test && /tmp/signal_suite_pattern_test
```

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_state_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c components/crt_fb/crt_fb.c \
    -o /tmp/signal_suite_state_test && /tmp/signal_suite_state_test
```

```bash
gcc -I tests/stubs -I components/signal_suite/include -I components/crt_fb/include -I components/crt_core/include -I components/crt_timing/include \
    tests/signal_suite_serial_test.c components/signal_suite/signal_suite.c \
    components/signal_suite/signal_suite_patterns.c components/signal_suite/signal_suite_serial.c \
    -o /tmp/signal_suite_serial_test && /tmp/signal_suite_serial_test
```

Then:

```bash
idf.py build
```

If hardware is attached:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Then generate the first host artifacts:

```bash
python3 tools/analysis/export_signal_suite_manifest.py > tools/analysis/signal_suite_manifest.json
python3 tools/analysis/report_transfer_curve.py captures/transfer_curve.csv --manifest tools/analysis/signal_suite_manifest.json --run-metadata captures/transfer_curve.json
python3 tools/analysis/report_transition_metrics.py captures/transition_255_0.csv --manifest tools/analysis/signal_suite_manifest.json --run-metadata captures/transition_255_0.json
```

Expected:

- host tests pass
- firmware builds
- device boots into `perform`
- `mode hold_probe` and movement/preset commands change the framebuffer output
- serial log prints machine-readable `SUITE_STATE ...` lines
- `list` prints the compact preset table
- host analysis scripts emit at least one transfer-curve report and one transition-metrics report

- [ ] **Step 4: Commit**

```bash
git add README.md tools/analysis/export_signal_suite_manifest.py tools/analysis/signal_suite_manifest.json tools/analysis/report_transfer_curve.py tools/analysis/report_transition_metrics.py
git commit -m "docs: document suite operation and analysis workflow"
```
