#!/usr/bin/env bash
set -euo pipefail

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
BASE_SDKCONFIG="${BASE_SDKCONFIG:-sdkconfig}"
BUILD_ROOT="${BUILD_ROOT:-build/render-modes}"
SDKCONFIG_ROOT="${SDKCONFIG_ROOT:-/tmp/esp32-crt-render-modes}"

MODES=(
    COMPOSE
    RGB332_FB
    RGB332_COMPOSE
    CALIBRATION
    STIMULUS
)

run_idf() {
    bash -c '. "$1" >/tmp/idf-export.log 2>&1 && shift && idf.py "$@"' _ "$IDF_EXPORT" "$@"
}

mode_slug() {
    tr '[:upper:]_' '[:lower:]-' <<<"$1"
}

prepare_sdkconfig() {
    local mode="$1"
    local sdkconfig_tmp="$2"

    if [[ -f "$BASE_SDKCONFIG" ]]; then
        cp "$BASE_SDKCONFIG" "$sdkconfig_tmp"
    else
        : >"$sdkconfig_tmp"
    fi

    sed -i -E '/^(# )?CONFIG_CRT_RENDER_MODE_(COMPOSE|RGB332_FB|RGB332_COMPOSE|CALIBRATION|STIMULUS)(=y| is not set)$/d' \
        "$sdkconfig_tmp"
    sed -i -E '/^(# )?CONFIG_CRT_ENABLE_COLOR(=y| is not set)$/d' "$sdkconfig_tmp"

    printf 'CONFIG_CRT_ENABLE_COLOR=y\n' >>"$sdkconfig_tmp"
    for candidate in "${MODES[@]}"; do
        if [[ "$candidate" == "$mode" ]]; then
            printf 'CONFIG_CRT_RENDER_MODE_%s=y\n' "$candidate" >>"$sdkconfig_tmp"
        else
            printf '# CONFIG_CRT_RENDER_MODE_%s is not set\n' "$candidate" >>"$sdkconfig_tmp"
        fi
    done
}

mkdir -p "$BUILD_ROOT" "$SDKCONFIG_ROOT"

for mode in "${MODES[@]}"; do
    slug="$(mode_slug "$mode")"
    sdkconfig_tmp="$SDKCONFIG_ROOT/$slug.sdkconfig"
    build_dir="$BUILD_ROOT/$slug"

    prepare_sdkconfig "$mode" "$sdkconfig_tmp"
    printf 'render-matrix: building %s -> %s\n' "$mode" "$build_dir"
    run_idf -B "$build_dir" -DSDKCONFIG="$sdkconfig_tmp" build
done

printf 'render-matrix: OK modes=%u build_root=%s\n' "${#MODES[@]}" "$BUILD_ROOT"
