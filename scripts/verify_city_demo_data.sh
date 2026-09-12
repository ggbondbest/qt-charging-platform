#!/usr/bin/env bash

set -euo pipefail

script_directory="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(CDPATH= cd -- "${script_directory}/.." && pwd)"
if ! command -v sqlite3 >/dev/null 2>&1; then
    printf 'City demo verification requires sqlite3.\n' >&2
    exit 1
fi

city_test_directory="$(mktemp -d "${TMPDIR:-/tmp}/charging-city-data.XXXXXX")"
city_test_database="${city_test_directory}/catalog.sqlite"
cleanup() {
    rm -f -- "${city_test_database}" "${city_test_database}-wal" "${city_test_database}-shm"
    rmdir -- "${city_test_directory}"
}
trap cleanup EXIT INT TERM

for sql_file in schema.sql seed.sql city_demo_seed.sql city_demo_seed.sql; do
    sqlite3 -batch -bail "${city_test_database}" < "${repository_root}/database/${sql_file}" > /dev/null
done

counts="$(sqlite3 -batch -bail "${city_test_database}" \
    "SELECT (SELECT COUNT(*) FROM stations) || '|' || (SELECT COUNT(*) FROM chargers);")"
if [[ "${counts}" != "25|75" ]]; then
    printf 'Unexpected city demo totals: %s (expected 25|75).\n' "${counts}" >&2
    exit 1
fi

for city in 大连市 沈阳市 北京市 上海市 深圳市; do
    counts="$(sqlite3 -batch -bail "${city_test_database}" \
        "SELECT COUNT(DISTINCT s.id) || '|' || COUNT(c.id)
         FROM stations s JOIN chargers c ON c.station_id=s.id
         WHERE s.address LIKE '${city}%';")"
    if [[ "${counts}" != "5|15" ]]; then
        printf 'Unexpected counts for %s: %s (expected 5|15).\n' "${city}" "${counts}" >&2
        exit 1
    fi
done

mixed_station_count="$(sqlite3 -batch -bail "${city_test_database}" \
    "SELECT COUNT(*) FROM (SELECT station_id FROM chargers GROUP BY station_id
     HAVING COUNT(*)=3 AND SUM(type='FAST')=2 AND SUM(type='SLOW')=1);")"
[[ "${mixed_station_count}" == "25" ]]

# Existing operator edits and business balance survive another seeded startup.
sqlite3 -batch -bail "${city_test_database}" \
    "UPDATE users SET balance_cents=54321 WHERE id=1;
     UPDATE stations SET name='保留管理员编辑',price_cents_per_kwh=199,status='INACTIVE'
     WHERE code='STA-CITY-SY-001';
     UPDATE chargers SET status='FAULT',total_charge_count=42
     WHERE code='CHG-CITY-SY-001-A1';" > /dev/null
for sql_file in seed.sql city_demo_seed.sql; do
    sqlite3 -batch -bail "${city_test_database}" < "${repository_root}/database/${sql_file}" > /dev/null
done
preserved="$(sqlite3 -batch -bail "${city_test_database}" \
    "SELECT (SELECT balance_cents FROM users WHERE id=1) || '|' ||
      (SELECT name || '|' || price_cents_per_kwh || '|' || status FROM stations WHERE code='STA-CITY-SY-001') || '|' ||
      (SELECT status || '|' || total_charge_count FROM chargers WHERE code='CHG-CITY-SY-001-A1');")"
[[ "${preserved}" == "54321|保留管理员编辑|199|INACTIVE|FAULT|42" ]]

integrity="$(sqlite3 -batch -bail "${city_test_database}" 'PRAGMA integrity_check;')"
[[ "${integrity}" == "ok" ]]
foreign_keys="$(sqlite3 -batch -bail "${city_test_database}" 'PRAGMA foreign_keys=ON; PRAGMA foreign_key_check;')"
[[ -z "${foreign_keys}" ]]
printf 'City demo data verified: 5 cities, 25 demonstration stations, 75 chargers; additive and idempotent.\n'
