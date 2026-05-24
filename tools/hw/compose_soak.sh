#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyACM0}"
SOAK_SECONDS="${SOAK_SECONDS:-90}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"

min_windows_for_duration() {
    local seconds="$1"
    local windows=$((seconds / 5 - 2))
    if ((windows < 4)); then
        windows=4
    fi
    printf '%u\n' "$windows"
}

run_soak() {
    local standard="$1"
    local standard_lc
    standard_lc="$(tr '[:upper:]' '[:lower:]' <<<"$standard")"

    RUN_NAME="compose_${standard_lc}_soak" \
        VIDEO_STANDARD="$standard" \
        COMPOSE_STRESS=1 \
        BUILD_DIR="build/compose-soak-${standard_lc}" \
        SDKCONFIG_TMP="/tmp/esp32-crt-compose-soak-${standard_lc}-sdkconfig" \
        MONITOR_LOG="/tmp/esp32-crt-compose-soak-${standard_lc}-monitor.log" \
        MONITOR_SECONDS="$SOAK_SECONDS" \
        MIN_WINDOWS="$(min_windows_for_duration "$SOAK_SECONDS")" \
        EXPECT_ACTIVE_SPRITES=8 \
        EXPECT_SPRITE_PEAK=8 \
        EXPECT_PPU_PENDING=8 \
        PORT="$PORT" \
        IDF_EXPORT="$IDF_EXPORT" \
        tools/hw/compose_smoke.sh
}

run_soak NTSC
run_soak PAL
run_soak PAL_M
run_soak PAL_N
printf 'compose_soak: OK port=%s seconds_per_standard=%u\n' "$PORT" "$SOAK_SECONDS"
