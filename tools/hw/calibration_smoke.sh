#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyACM0}"
RUN_NAME="${RUN_NAME:-calibration_pal_m}"
BUILD_DIR="${BUILD_DIR:-build/calibration-pal-m}"
BASE_SDKCONFIG="${BASE_SDKCONFIG:-sdkconfig}"
SDKCONFIG_TMP="${SDKCONFIG_TMP:-/tmp/esp32-crt-calibration-pal-m-sdkconfig}"
MONITOR_LOG="${MONITOR_LOG:-/tmp/esp32-crt-calibration-pal-m-monitor.log}"
MONITOR_SECONDS="${MONITOR_SECONDS:-14}"
MIN_DIAG_LINES="${MIN_DIAG_LINES:-1}"
VIDEO_STANDARD="${VIDEO_STANDARD:-PAL_M}"
EXPECT_STANDARD_NAME="${EXPECT_STANDARD_NAME:-}"
EXPECT_STATUS="${EXPECT_STATUS:-validated}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"

fail() {
    printf '%s: FAIL: %s\n' "$RUN_NAME" "$*" >&2
    printf '%s: monitor log: %s\n' "$RUN_NAME" "$MONITOR_LOG" >&2
    exit 1
}

extract_u32() {
    local key="$1"
    local line="$2"
    sed -n "s/.*${key}=\([0-9][0-9]*\).*/\1/p" <<<"$line"
}

run_idf() {
    bash -c '. "$1" >/tmp/idf-export.log 2>&1 && shift && idf.py "$@"' _ "$IDF_EXPORT" "$@"
}

prepare_sdkconfig() {
    if [[ -f "$BASE_SDKCONFIG" ]]; then
        cp "$BASE_SDKCONFIG" "$SDKCONFIG_TMP"
    else
        : >"$SDKCONFIG_TMP"
    fi

    sed -i -E '/^(# )?CONFIG_CRT_RENDER_MODE_(COMPOSE|RGB332_FB|RGB332_COMPOSE|CALIBRATION|STIMULUS)(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    sed -i -E '/^(# )?CONFIG_CRT_VIDEO_STANDARD_(NTSC|PAL|PAL_M|PAL_N)(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    sed -i -E '/^(# )?CONFIG_CRT_ENABLE_COLOR(=y| is not set)$/d' "$SDKCONFIG_TMP"

    {
        printf 'CONFIG_CRT_ENABLE_COLOR=y\n'
        printf '# CONFIG_CRT_RENDER_MODE_COMPOSE is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_FB is not set\n'
        printf 'CONFIG_CRT_RENDER_MODE_CALIBRATION=y\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_COMPOSE is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_STIMULUS is not set\n'

        case "$VIDEO_STANDARD" in
            NTSC)
                EXPECT_STANDARD_NAME="${EXPECT_STANDARD_NAME:-NTSC-M}"
                printf 'CONFIG_CRT_VIDEO_STANDARD_NTSC=y\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL is not set\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL_M is not set\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL_N is not set\n'
                ;;
            PAL_M)
                EXPECT_STANDARD_NAME="${EXPECT_STANDARD_NAME:-PAL-M}"
                printf '# CONFIG_CRT_VIDEO_STANDARD_NTSC is not set\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL is not set\n'
                printf 'CONFIG_CRT_VIDEO_STANDARD_PAL_M=y\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL_N is not set\n'
                ;;
            *)
                fail "unsupported VIDEO_STANDARD=${VIDEO_STANDARD}; calibration smoke supports NTSC or PAL_M"
                ;;
        esac
    } >>"$SDKCONFIG_TMP"
}

capture_monitor() {
    local monitor_cmd
    monitor_cmd="bash -c '. \"$IDF_EXPORT\" >/tmp/idf-export.log 2>&1 && idf.py -B \"$BUILD_DIR\" -DSDKCONFIG=\"$SDKCONFIG_TMP\" -p \"$PORT\" monitor'"
    rm -f "$MONITOR_LOG"
    timeout "${MONITOR_SECONDS}s" script -qfc "$monitor_cmd" "$MONITOR_LOG" >/dev/null 2>&1 || true
}

validate_monitor() {
    if grep -aEq 'task_wdt|Task watchdog|Guru Meditation|panic' "$MONITOR_LOG"; then
        fail "monitor reported watchdog or panic"
    fi

    local boot_line
    boot_line="$(grep -a 'ESP32 CRT signal core started:' "$MONITOR_LOG" | tail -n 1 || true)"
    [[ -n "$boot_line" ]] || fail "missing CRT startup log"
    grep -aFq "standard=${EXPECT_STANDARD_NAME}" <<<"$boot_line" ||
        fail "startup standard mismatch: ${boot_line}"
    grep -aFq "pattern=calibration" <<<"$boot_line" ||
        fail "startup pattern mismatch: ${boot_line}"
    grep -aFq "status=${EXPECT_STATUS}" <<<"$boot_line" ||
        fail "startup status mismatch: ${boot_line}"

    mapfile -t diag_lines < <(grep -a 'runtime_meta: .*underruns=' "$MONITOR_LOG" || true)
    ((${#diag_lines[@]} >= MIN_DIAG_LINES)) ||
        fail "expected at least ${MIN_DIAG_LINES} diag lines, got ${#diag_lines[@]}"

    local line underruns
    local i=0
    for line in "${diag_lines[@]}"; do
        i=$((i + 1))
        underruns="$(extract_u32 underruns "$line")"
        [[ -n "$underruns" ]] || fail "diag ${i}: could not parse underruns"
        ((underruns == 0)) || fail "diag ${i}: underruns=${underruns}"
        printf '%s: diag %u underruns=%u\n' "$RUN_NAME" "$i" "$underruns"
    done
}

prepare_sdkconfig
run_idf -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG_TMP" -p "$PORT" build flash
capture_monitor
validate_monitor
printf '%s: OK port=%s standard=%s log=%s\n' "$RUN_NAME" "$PORT" "$EXPECT_STANDARD_NAME" "$MONITOR_LOG"
