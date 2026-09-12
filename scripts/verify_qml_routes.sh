#!/usr/bin/env bash
# UI-only resource/route smoke. This is NOT a TCP, billing or SQLite acceptance test.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash scripts/verify_qml_routes.sh /path/to/charging-client [screenshot-directory]" >&2
    echo "Optional environment: CHARGING_SMOKE_SIZE=420x860 CHARGING_SMOKE_THEME=light|dark" >&2
    exit 2
fi
smoke_size="${CHARGING_SMOKE_SIZE:-420x860}"
smoke_theme="${CHARGING_SMOKE_THEME:-light}"
if [[ ! "$smoke_size" =~ ^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$ ]] \
   || (( ${smoke_size%x*} > 8192 || ${smoke_size#*x} > 8192 )); then
    echo 'CHARGING_SMOKE_SIZE must be WIDTHxHEIGHT, with each dimension in 1..8192' >&2
    exit 2
fi
if [[ "$smoke_theme" != light && "$smoke_theme" != dark ]]; then
    echo 'CHARGING_SMOKE_THEME must be light or dark' >&2
    exit 2
fi
binary="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
if [[ ! -x "$binary" ]]; then
    echo "Client executable not found: $binary" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for the per-route process timeout" >&2
    exit 2
fi
output_dir="${2:-$(mktemp -d "${TMPDIR:-/tmp}/charging-qml-routes.XXXXXX")}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
export CHARGING_CHANNEL=mock
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_QUICK_BACKEND="${QT_QUICK_BACKEND:-software}"
# Do not consume real map quotas or expose an inherited API key in UI-only CI.
export TENCENT_MAP_API_KEY='' CHARGING_TENCENT_MAP_KEY='' TENCENT_MAP_JS_KEY=''
export TENCENT_MAP_SECRET_KEY='' CHARGING_TENCENT_MAP_SECRET=''
export XDG_CONFIG_HOME="$output_dir/config"
export XDG_CACHE_HOME="$output_dir/cache"

run_with_timeout() {
    python3 -c '
import subprocess, sys
try:
    sys.exit(subprocess.run(sys.argv[1:], timeout=20, check=False).returncode)
except subprocess.TimeoutExpired:
    print("QML_ROUTE_TIMEOUT: process exceeded 20 seconds", file=sys.stderr)
    sys.exit(124)
' "$@"
}

failures=0
passed=0
error_pattern='QML load failed|Root is not a Window|Component is not ready|TypeError:|ReferenceError:|Unable to assign|Cannot assign|is not a type|module .* is not installed|Binding loop detected|Cannot anchor to an item|Cannot load library|Cannot find plugin|QML_ROUTE_TIMEOUT'
for route in login station profile wallet recharge order order_detail charging charging_run \
             settlement profile_edit station_detail reservation_confirm reservation_module \
             navigation favorites notifications settings stats coupon points ratings scan queue fault_reports \
             tasks level points_mall; do
    arg='{}'
    case "$route" in
        station_detail)
            arg='{"id":"1","name":"科技园充电驿站","address":"南山区科苑南路1012号","status":"active","latitude":22.5412,"longitude":113.943,"priceCentsPerKwh":120,"distanceMeters":-1}' ;;
        reservation_confirm)
            arg='{"stationId":"1","stationName":"科技园充电驿站","chargerId":"1001","chargerCode":"SZ-KEY-01-01","chargerType":"fast","chargerPowerWatts":120000,"priceCentsPerKwh":120,"stationLatitude":22.5412,"stationLongitude":113.943,"hasStationLocation":true,"distanceMeters":-1}' ;;
        navigation)
            arg='{"stationName":"科技园充电驿站","stationLatitude":22.5412,"stationLongitude":113.943,"hasStationLocation":true}' ;;
        order_detail)
            arg='{"id":"2","orderNo":"SMOKE-ORDER-2","stationName":"界面测试电站","chargerCode":"TEST-01","status":"completed","amountCents":1250,"energyWh":10000,"durationSeconds":1800,"unitPriceCentsPerKwh":125,"createdAt":"2026-09-08T01:00:00.000Z"}' ;;
        charging_run) arg='{"id":"5"}' ;;
        settlement)
            arg='{"id":"4","orderNo":"SMOKE-ORDER-4","stationName":"界面测试电站","chargerCode":"TEST-01","status":"waiting_payment","amountCents":1250,"energyWh":10000,"durationSeconds":1800,"unitPriceCentsPerKwh":125}' ;;
        stats|coupon|points|ratings|scan)
            # These top-level pages fetch the current user's own data; no
            # fabricated order/station route argument is needed to open them.
            arg='""' ;;
    esac
    screenshot="$output_dir/$route.png"
    log="$output_dir/$route.log"
    command_args=("$binary" "--view=$route" "--arg=$arg" "--size=$smoke_size"
                  "--theme=$smoke_theme" "--screenshot=$screenshot")
    if [[ "$route" != login ]]; then
        command_args+=(--logged-in)
    fi
    if run_with_timeout "${command_args[@]}" >"$log" 2>&1; then
        exit_status=0
    else
        exit_status=$?
    fi
    if [[ "$exit_status" -ne 0 || ! -s "$screenshot" ]] || grep -Eq "$error_pattern" "$log"; then
        echo "FAIL $route (exit=$exit_status; log=$log)"
        sed -n '1,100p' "$log"
        failures=$((failures + 1))
    else
        echo "PASS $route"
        passed=$((passed + 1))
    fi
done
echo "QML UI smoke ($smoke_size, $smoke_theme): $passed passed, $failures failed. Screenshots: $output_dir"
echo "Explicit Mock channel: these results do not prove real backend/database connectivity."
[[ "$failures" -eq 0 ]]
