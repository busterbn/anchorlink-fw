#!/usr/bin/env bash
# Subscribe to all MQTT broker traffic and log it, writing a new file per day.
#
# Mirrors scripts/onomondo-capture.sh: runs mosquitto_sub against the broker and
# rotates to a fresh log file at local midnight. mosquitto_sub timestamps each
# message itself via -F. Driven by systemd (see scripts/mqtt-capture.service);
# the loop also recovers if mosquitto_sub exits early.
set -euo pipefail

REPO_DIR="/home/bn/anchorlink-fw"
CAPTURE_DIR="$REPO_DIR/captures"

# Load MQTT_USER and MQTT_PASSWORD (and optional MQTT_HOST/MQTT_PORT/MQTT_CAFILE).
set -a
# shellcheck disable=SC1091
source "$REPO_DIR/.env"
set +a

# Broker connection. Defaults match the existing `just mqtt-sub` (localhost, TLS).
MQTT_HOST="${MQTT_HOST:-localhost}"
MQTT_PORT="${MQTT_PORT:-8883}"
# Server CA for TLS; falls back to the CA shipped in the repo's ca/ dir.
CAFILE="${MQTT_CAFILE:-$(ls "$REPO_DIR"/ca/*_ca.pem 2>/dev/null | head -1)}"

mkdir -p "$CAPTURE_DIR"

CAP_PID=""
cleanup() {
	if [[ -n "$CAP_PID" ]] && kill -0 "$CAP_PID" 2>/dev/null; then
		kill -TERM "$CAP_PID" 2>/dev/null || true
		wait "$CAP_PID" 2>/dev/null || true
	fi
	exit 0
}
trap cleanup TERM INT

while true; do
	day="$(date +%F)"
	file="$CAPTURE_DIR/mqtt-$day.log"

	# Seconds until the next local midnight, so we rotate exactly on the day change.
	secs=$(( $(date -d 'tomorrow 00:00:00' +%s) - $(date +%s) ))

	echo "Logging to $file (rotating in ${secs}s)"
	# -F '%I  %t  %p' => ISO-8601 timestamp, topic, payload per message.
	mosquitto_sub \
		-h "$MQTT_HOST" -p "$MQTT_PORT" \
		--cafile "$CAFILE" --insecure \
		-u "$MQTT_USER" -P "$MQTT_PASSWORD" \
		-t '#' -q 1 \
		-F '%I  %t  %p' >>"$file" &
	CAP_PID=$!

	# Stop the subscription at midnight to start a new day's file.
	( sleep "$secs"; kill -TERM "$CAP_PID" 2>/dev/null || true ) &
	timer_pid=$!

	wait "$CAP_PID" 2>/dev/null || true
	CAP_PID=""
	kill -TERM "$timer_pid" 2>/dev/null || true
	wait "$timer_pid" 2>/dev/null || true

	# Brief backoff so an immediate mosquitto_sub failure can't busy-loop.
	sleep 5
done
