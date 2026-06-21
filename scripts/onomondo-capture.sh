#!/usr/bin/env bash
# Continuously capture all Onomondo SIM traffic, writing a new pcap file per day.
#
# Runs onomondo-live with a date-stamped filename and rotates to a fresh file at
# local midnight. Driven by systemd (see scripts/onomondo-capture.service); the
# loop also recovers if onomondo-live exits early.
set -euo pipefail

REPO_DIR="/home/bn/anchorlink-fw"
CAPTURE_DIR="$REPO_DIR/captures"

# Load ONOMONDO_API_KEY and SIM_ID.
set -a
# shellcheck disable=SC1091
source "$REPO_DIR/.env"
set +a

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
	file="$CAPTURE_DIR/onomondo-$day.pcap"
	# Avoid clobbering an existing day file (e.g. after a mid-day restart).
	if [[ -s "$file" ]]; then
		file="$CAPTURE_DIR/onomondo-$day.$(date +%H%M%S).pcap"
	fi

	# Seconds until the next local midnight, so we rotate exactly on the day change.
	secs=$(( $(date -d 'tomorrow 00:00:00' +%s) - $(date +%s) ))

	echo "Capturing to $file (rotating in ${secs}s)"
	onomondo-live --key="$ONOMONDO_API_KEY" --sim="$SIM_ID" --filename="$file" &
	CAP_PID=$!

	# Stop the capture at midnight to start a new day's file.
	( sleep "$secs"; kill -TERM "$CAP_PID" 2>/dev/null || true ) &
	timer_pid=$!

	wait "$CAP_PID" 2>/dev/null || true
	CAP_PID=""
	kill -TERM "$timer_pid" 2>/dev/null || true
	wait "$timer_pid" 2>/dev/null || true

	# Brief backoff so an immediate onomondo-live failure can't busy-loop.
	sleep 5
done
