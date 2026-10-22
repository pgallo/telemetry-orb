#!/usr/bin/env bash
# Starts hub, two stations and a producer, lets them run for DEMO_SECONDS
# (default 5), then shuts everything down with SIGTERM in reverse order and
# prints each process's output.
#
#   scripts/demo.sh            run the demo
#   scripts/demo.sh --check    also assert on the output (used by ctest)
#   scripts/demo.sh --naming   resolve the hub through a local Naming Service
#
# Environment: TELEMETRY_BIN (directory with the executables, default ./build),
#              DEMO_SECONDS, DEMO_BEDS, DEMO_RATE_HZ.
set -euo pipefail

check=0
naming=0
for arg in "$@"; do
  case "$arg" in
    --check) check=1 ;;
    --naming) naming=1 ;;
    *) echo "usage: $0 [--check] [--naming]" >&2; exit 2 ;;
  esac
done

bin="${TELEMETRY_BIN:-$(pwd)/build}"
seconds="${DEMO_SECONDS:-5}"
beds="${DEMO_BEDS:-6}"
rate="${DEMO_RATE_HZ:-4}"
for exe in telemetry-hub telemetry-producer telemetry-station; do
  [[ -x "$bin/$exe" ]] || { echo "missing $bin/$exe (build first or set TELEMETRY_BIN)" >&2; exit 2; }
done

work="$(mktemp -d)"
pids=()
shutdown() {
  # Reverse start order: clients unsubscribe and unregister while the hub is up.
  local i
  for (( i=${#pids[@]}-1; i>=0; i-- )); do
    kill -TERM "${pids[$i]}" 2>/dev/null || true
    wait "${pids[$i]}" 2>/dev/null || true
  done
  pids=()
}
trap 'shutdown; rm -rf "$work"' EXIT

ior="$work/hub.ior"
locate=(--ior-file "$ior")
hub_extra=()
if (( naming )); then
  port=$(( 20000 + RANDOM % 10000 ))
  mkdir -p "$work/names"
  omniNames -start "$port" -always -logdir "$work/names" > "$work/names.log" 2>&1 &
  pids+=($!)
  sleep 0.5
  export ORBInitRef="NameService=corbaname::127.0.0.1:$port"
  locate=(--naming telemetry/Hub.object)
  hub_extra=(--naming telemetry/Hub.object)
fi

# Bind to loopback on an ephemeral port; the IOR file carries the address.
endpoint=(-ORBendPoint giop:tcp:127.0.0.1:)

"$bin/telemetry-hub" --ior-file "$ior" --stats-ms 1000 "${hub_extra[@]}" "${endpoint[@]}" \
  > "$work/hub.log" 2>&1 &
pids+=($!)
for _ in $(seq 1 100); do [[ -s "$ior" ]] && break; sleep 0.05; done
[[ -s "$ior" ]] || { echo "hub did not start" >&2; cat "$work/hub.log" >&2; exit 1; }

"$bin/telemetry-station" "${locate[@]}" --name station-all --refresh-ms 1000 "${endpoint[@]}" \
  > "$work/station-all.log" 2>&1 &
pids+=($!)
# A deliberately constrained station: four beds, two parameters, a 12-sample
# queue and at most two pushes per second. 32 samples/s match its filter but
# it accepts only 24/s, so the hub drops the oldest for it and nobody else.
"$bin/telemetry-station" "${locate[@]}" --name station-spo2 --beds ICU-01,ICU-02,ICU-03,ICU-04 \
  --params SPO2,HR --max-queue 12 --max-batch 12 --min-interval-ms 500 --refresh-ms 2500 \
  "${endpoint[@]}" \
  > "$work/station-spo2.log" 2>&1 &
pids+=($!)
sleep 0.3
"$bin/telemetry-producer" "${locate[@]}" --beds "$beds" --rate-hz "$rate" \
  > "$work/producer.log" 2>&1 &
pids+=($!)

sleep "$seconds"
shutdown

for f in producer station-spo2 station-all hub; do
  echo "===== $f ====="
  cat "$work/$f.log"
done

if (( check )); then
  fail=0
  expect() {  # expect <log> <fixed string>
    if ! grep -qF -- "$2" "$work/$1.log"; then
      echo "demo check: '$2' not found in $1.log" >&2
      fail=1
    fi
  }
  expect hub "IOR written to"
  expect hub "station-all beds=* params=*"
  expect hub "station-spo2 beds=ICU-01,ICU-02,ICU-03,ICU-04 params=SPO2,HR"
  expect hub "unsubscribe #"
  expect producer "$beds beds registered"
  expect producer "0 failed publishes"
  expect station-all "subscribed as #"
  expect station-all "ICU-0$beds"
  expect station-all "from $beds beds"
  expect station-spo2 "from 4 beds"
  # The constrained station must have received data and seen hub-side drops.
  if ! grep -qE "received [1-9][0-9]* samples" "$work/station-all.log"; then
    echo "demo check: station-all received nothing" >&2; fail=1
  fi
  if ! grep -qE "dropped at hub=[1-9]" "$work/station-spo2.log"; then
    echo "demo check: expected overflow drops for station-spo2" >&2; fail=1
  fi
  if grep -q "evicted" "$work/hub.log"; then
    echo "demo check: unexpected eviction" >&2; fail=1
  fi
  (( fail == 0 )) && echo "demo check: OK"
  exit "$fail"
fi
