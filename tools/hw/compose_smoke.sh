#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyACM0}"
BUILD_DIR="${BUILD_DIR:-build/compose-smoke}"
BASE_SDKCONFIG="${BASE_SDKCONFIG:-sdkconfig}"
SDKCONFIG_TMP="${SDKCONFIG_TMP:-/tmp/esp32-crt-compose-smoke-sdkconfig}"
MONITOR_LOG="${MONITOR_LOG:-/tmp/esp32-crt-compose-smoke-monitor.log}"
MONITOR_SECONDS="${MONITOR_SECONDS:-22}"
MIN_WINDOWS="${MIN_WINDOWS:-2}"
MAX_COMPOSE_FUSED="${MAX_COMPOSE_FUSED:-90000}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"

fail() {
    printf 'compose_smoke: FAIL: %s\n' "$*" >&2
    printf 'compose_smoke: monitor log: %s\n' "$MONITOR_LOG" >&2
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

    sed -i -E '/^(# )?CONFIG_CRT_RENDER_MODE_(COMPOSE|RGB332_FB|RGB332_COMPOSE|STIMULUS)(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    {
        printf 'CONFIG_CRT_RENDER_MODE_COMPOSE=y\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_FB is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_COMPOSE is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_STIMULUS is not set\n'
    } >>"$SDKCONFIG_TMP"
}

capture_monitor() {
    local monitor_cmd
    monitor_cmd="bash -c '. \"$IDF_EXPORT\" >/tmp/idf-export.log 2>&1 && idf.py -B \"$BUILD_DIR\" -DSDKCONFIG=\"$SDKCONFIG_TMP\" -p \"$PORT\" monitor'"
    rm -f "$MONITOR_LOG"
    timeout "${MONITOR_SECONDS}s" script -qfc "$monitor_cmd" "$MONITOR_LOG" >/dev/null 2>&1 || true
}

validate_monitor() {
    mapfile -t budget_lines < <(grep -a 'compose_budget:' "$MONITOR_LOG" || true)
    ((${#budget_lines[@]} >= MIN_WINDOWS)) ||
        fail "expected at least ${MIN_WINDOWS} compose_budget windows, got ${#budget_lines[@]}"

    local line underruns sprite_overflow materialized fused max_layers
    local i=0
    for line in "${budget_lines[@]}"; do
        i=$((i + 1))
        underruns="$(extract_u32 underruns "$line")"
        sprite_overflow="$(extract_u32 sprite_overflow "$line")"
        materialized="$(extract_u32 compose_materialized "$line")"
        fused="$(extract_u32 compose_fused "$line")"
        max_layers="$(extract_u32 compose_max_layers "$line")"

        [[ -n "$underruns" && -n "$sprite_overflow" && -n "$materialized" && -n "$fused" && -n "$max_layers" ]] ||
            fail "window ${i}: could not parse compose_budget counters"
        ((underruns == 0)) || fail "window ${i}: underruns=${underruns}"
        ((sprite_overflow == 0)) || fail "window ${i}: sprite_overflow=${sprite_overflow}"
        ((materialized == 0)) || fail "window ${i}: compose_materialized=${materialized}"
        ((fused > 0)) || fail "window ${i}: compose_fused=${fused}"
        ((fused <= MAX_COMPOSE_FUSED)) ||
            fail "window ${i}: compose_fused=${fused} exceeds ${MAX_COMPOSE_FUSED}; stats may be accumulating"
        ((max_layers > 0)) || fail "window ${i}: compose_max_layers=${max_layers}"

        printf 'compose_smoke: window %u fused=%u materialized=%u max_layers=%u underruns=%u sprite_overflow=%u\n' \
            "$i" "$fused" "$materialized" "$max_layers" "$underruns" "$sprite_overflow"
    done
}

prepare_sdkconfig
run_idf -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG_TMP" -p "$PORT" build flash
capture_monitor
validate_monitor
printf 'compose_smoke: OK port=%s windows=%u log=%s\n' "$PORT" "${#budget_lines[@]}" "$MONITOR_LOG"
