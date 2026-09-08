#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_dir}/build-delivery}"
build_jobs="${CHARGING_BUILD_JOBS:-2}"
if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo 'CHARGING_BUILD_JOBS must be a positive integer' >&2
    exit 2
fi

cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON \
    -DCHARGING_BUILD_QML=ON -DCHARGING_BUILD_WIDGETS_CLIENT=OFF
cmake --build "$build_dir" --parallel "$build_jobs"
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    ctest --test-dir "$build_dir" --output-on-failure
bash "$project_dir/scripts/verify_database.sh"
bash "$project_dir/scripts/verify_qml_routes.sh" "$build_dir/client/charging-client"
