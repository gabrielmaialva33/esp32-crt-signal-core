#!/usr/bin/env bash
# tcbvn_capture.sh — grab one RTSP frame from the IP camera and save it
# under tools/captures/. Optional pattern label tags the filename.
#
# Usage:
#   ./tools/tcbvn_capture.sh [pattern_label]
# Reads creds from ~/.config/tcbvn/creds.env (chmod 600).

set -euo pipefail

CREDS=${TCBVN_CREDS:-$HOME/.config/tcbvn/creds.env}
[[ -r "$CREDS" ]] || { echo "missing $CREDS" >&2; exit 1; }
# shellcheck disable=SC1090
. "$CREDS"

LABEL=${1:-frame}
TS=$(date +%Y%m%d_%H%M%S)
OUT=tools/captures/${LABEL}_${TS}.jpg
URL="rtsp://${RTSP_USER}:${RTSP_PASS}@${RTSP_HOST}:${RTSP_PORT}${RTSP_PATH_SD}"

# Yoosee requires UDP transport; settle the stream a bit before grabbing.
ffmpeg -hide_banner -loglevel error -y \
       -rtsp_transport udp -i "$URL" \
       -ss 1.5 -frames:v 1 -q:v 2 \
       "$OUT"

echo "captured: $OUT"
ls -la "$OUT"
