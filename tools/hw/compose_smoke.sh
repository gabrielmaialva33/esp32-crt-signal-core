#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyACM0}"
RUN_NAME="${RUN_NAME:-compose_smoke}"
BUILD_DIR="${BUILD_DIR:-build/compose-smoke}"
BASE_SDKCONFIG="${BASE_SDKCONFIG:-sdkconfig}"
SDKCONFIG_TMP="${SDKCONFIG_TMP:-/tmp/esp32-crt-compose-smoke-sdkconfig}"
MONITOR_LOG="${MONITOR_LOG:-/tmp/esp32-crt-compose-smoke-monitor.log}"
MONITOR_SECONDS="${MONITOR_SECONDS:-22}"
MIN_WINDOWS="${MIN_WINDOWS:-2}"
MAX_COMPOSE_FUSED="${MAX_COMPOSE_FUSED:-90000}"
COMPOSE_STRESS="${COMPOSE_STRESS:-0}"
VIDEO_STANDARD="${VIDEO_STANDARD:-NTSC}"
EXPECT_ACTIVE_SPRITES="${EXPECT_ACTIVE_SPRITES:-}"
EXPECT_SPRITE_PEAK="${EXPECT_SPRITE_PEAK:-}"
EXPECT_PPU_PENDING="${EXPECT_PPU_PENDING:-}"
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

    sed -i -E '/^(# )?CONFIG_CRT_RENDER_MODE_(COMPOSE|RGB332_FB|RGB332_COMPOSE|STIMULUS)(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    sed -i -E '/^(# )?CONFIG_CRT_COMPOSE_STRESS_DEMO(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    sed -i -E '/^(# )?CONFIG_CRT_VIDEO_STANDARD_(NTSC|PAL|PAL_M)(=y| is not set)$/d' \
        "$SDKCONFIG_TMP"
    {
        printf 'CONFIG_CRT_RENDER_MODE_COMPOSE=y\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_FB is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_RGB332_COMPOSE is not set\n'
        printf '# CONFIG_CRT_RENDER_MODE_STIMULUS is not set\n'
        case "$VIDEO_STANDARD" in
            NTSC)
                printf 'CONFIG_CRT_VIDEO_STANDARD_NTSC=y\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL is not set\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL_M is not set\n'
                ;;
            PAL)
                printf '# CONFIG_CRT_VIDEO_STANDARD_NTSC is not set\n'
                printf 'CONFIG_CRT_VIDEO_STANDARD_PAL=y\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL_M is not set\n'
                ;;
            PAL_M)
                printf '# CONFIG_CRT_VIDEO_STANDARD_NTSC is not set\n'
                printf '# CONFIG_CRT_VIDEO_STANDARD_PAL is not set\n'
                printf 'CONFIG_CRT_VIDEO_STANDARD_PAL_M=y\n'
                ;;
            *)
                fail "unsupported VIDEO_STANDARD=${VIDEO_STANDARD}"
                ;;
        esac
        if [[ "$COMPOSE_STRESS" == "1" ]]; then
            printf 'CONFIG_CRT_COMPOSE_STRESS_DEMO=y\n'
        else
            printf '# CONFIG_CRT_COMPOSE_STRESS_DEMO is not set\n'
        fi
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

    mapfile -t budget_lines < <(grep -a 'compose_budget:' "$MONITOR_LOG" || true)
    ((${#budget_lines[@]} >= MIN_WINDOWS)) ||
        fail "expected at least ${MIN_WINDOWS} compose_budget windows, got ${#budget_lines[@]}"

    local line underruns sprite_overflow materialized fused max_layers sprites sprite_peak sprite_cap ppu_pending
    local i=0
    for line in "${budget_lines[@]}"; do
        i=$((i + 1))
        underruns="$(extract_u32 underruns "$line")"
        sprite_overflow="$(extract_u32 sprite_overflow "$line")"
        materialized="$(extract_u32 compose_materialized "$line")"
        fused="$(extract_u32 compose_fused "$line")"
        max_layers="$(extract_u32 compose_max_layers "$line")"
        sprites="$(extract_u32 sprites "$line")"
        sprite_peak="$(sed -n 's/.*sprite_peak=\([0-9][0-9]*\)\/[0-9][0-9]*.*/\1/p' <<<"$line")"
        sprite_cap="$(sed -n 's/.*sprite_peak=[0-9][0-9]*\/\([0-9][0-9]*\).*/\1/p' <<<"$line")"
        ppu_pending="$(sed -n 's/.*ppu_pending=\([0-9][0-9]*\)\/[0-9][0-9]*.*/\1/p' <<<"$line")"

        [[ -n "$underruns" && -n "$sprite_overflow" && -n "$materialized" && -n "$fused" && -n "$max_layers" ]] ||
            fail "window ${i}: could not parse compose_budget counters"
        ((underruns == 0)) || fail "window ${i}: underruns=${underruns}"
        ((sprite_overflow == 0)) || fail "window ${i}: sprite_overflow=${sprite_overflow}"
        ((materialized == 0)) || fail "window ${i}: compose_materialized=${materialized}"
        ((fused > 0)) || fail "window ${i}: compose_fused=${fused}"
        ((fused <= MAX_COMPOSE_FUSED)) ||
            fail "window ${i}: compose_fused=${fused} exceeds ${MAX_COMPOSE_FUSED}; stats may be accumulating"
        ((max_layers > 0)) || fail "window ${i}: compose_max_layers=${max_layers}"
        if [[ -n "$EXPECT_ACTIVE_SPRITES" ]]; then
            [[ -n "$sprites" ]] || fail "window ${i}: could not parse sprites"
            ((sprites == EXPECT_ACTIVE_SPRITES)) ||
                fail "window ${i}: sprites=${sprites}, expected ${EXPECT_ACTIVE_SPRITES}"
        fi
        if [[ -n "$EXPECT_SPRITE_PEAK" ]]; then
            [[ -n "$sprite_peak" && -n "$sprite_cap" ]] ||
                fail "window ${i}: could not parse sprite_peak"
            ((sprite_peak == EXPECT_SPRITE_PEAK)) ||
                fail "window ${i}: sprite_peak=${sprite_peak}, expected ${EXPECT_SPRITE_PEAK}"
            ((sprite_cap == EXPECT_SPRITE_PEAK)) ||
                fail "window ${i}: sprite_peak cap=${sprite_cap}, expected ${EXPECT_SPRITE_PEAK}"
        fi
        if [[ -n "$EXPECT_PPU_PENDING" ]]; then
            [[ -n "$ppu_pending" ]] || fail "window ${i}: could not parse ppu_pending"
            ((ppu_pending == EXPECT_PPU_PENDING)) ||
                fail "window ${i}: ppu_pending=${ppu_pending}, expected ${EXPECT_PPU_PENDING}"
        fi

        printf '%s: window %u fused=%u materialized=%u max_layers=%u underruns=%u sprite_overflow=%u sprite_peak=%s/%s ppu_pending=%s\n' \
            "$RUN_NAME" "$i" "$fused" "$materialized" "$max_layers" "$underruns" "$sprite_overflow" \
            "${sprite_peak:-?}" "${sprite_cap:-?}" "${ppu_pending:-?}"
    done
}

prepare_sdkconfig
run_idf -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG_TMP" -p "$PORT" build flash
capture_monitor
validate_monitor
printf '%s: OK port=%s windows=%u log=%s\n' "$RUN_NAME" "$PORT" "${#budget_lines[@]}" "$MONITOR_LOG"
