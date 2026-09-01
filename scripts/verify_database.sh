#!/usr/bin/env bash

set -euo pipefail

script_directory="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(CDPATH= cd -- "${script_directory}/.." && pwd)"
schema_file="${1:-${repository_root}/database/schema.sql}"
seed_file="${2:-${repository_root}/database/seed.sql}"

for sql_file in "${schema_file}" "${seed_file}"; do
    if [[ ! -f "${sql_file}" ]]; then
        printf 'Database verification failed: file not found: %s\n' "${sql_file}" >&2
        exit 1
    fi
done

if ! command -v sqlite3 >/dev/null 2>&1; then
    printf 'Database verification failed: sqlite3 is not installed.\n' >&2
    exit 1
fi

temporary_directory="$(mktemp -d "${TMPDIR:-/tmp}/charging-platform-db.XXXXXX")"
temporary_database="${temporary_directory}/charging-platform.db"
cleanup() {
    rm -rf -- "${temporary_directory}"
}
trap cleanup EXIT INT TERM

sqlite3 -batch -bail "${temporary_database}" < "${schema_file}" > /dev/null
sqlite3 -batch -bail "${temporary_database}" < "${seed_file}" > /dev/null
# Seed files are required to be safe to run more than once in demo/test setup.
sqlite3 -batch -bail "${temporary_database}" < "${seed_file}" > /dev/null

integrity_result="$(sqlite3 -batch -bail "${temporary_database}" 'PRAGMA integrity_check;')"
if [[ "${integrity_result}" != "ok" ]]; then
    printf 'Database verification failed: integrity_check returned %s\n' \
        "${integrity_result}" >&2
    exit 1
fi

foreign_key_errors="$(sqlite3 -batch -bail "${temporary_database}" \
    'PRAGMA foreign_keys = ON; PRAGMA foreign_key_check;')"
if [[ -n "${foreign_key_errors}" ]]; then
    printf 'Database verification failed: foreign key violations found:\n%s\n' \
        "${foreign_key_errors}" >&2
    exit 1
fi

table_count="$(sqlite3 -batch -bail "${temporary_database}" \
    "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%';")"
if [[ "${table_count}" -ne 8 ]]; then
    printf 'Database verification failed: expected 8 application tables, found %s.\n' \
        "${table_count}" >&2
    exit 1
fi

schema_version="$(sqlite3 -batch -bail "${temporary_database}" 'PRAGMA user_version;')"
if [[ "${schema_version}" -ne 1 ]]; then
    printf 'Database verification failed: expected schema version 1, found %s.\n' \
        "${schema_version}" >&2
    exit 1
fi

seed_counts="$(sqlite3 -batch -bail "${temporary_database}" \
    "SELECT (SELECT count(*) FROM admins) || '|' ||
            (SELECT count(*) FROM users) || '|' ||
            (SELECT count(*) FROM stations) || '|' ||
            (SELECT count(*) FROM chargers) || '|' ||
            (SELECT count(*) FROM recharge_records);")"
if [[ "${seed_counts}" != "1|1|3|7|1" ]]; then
    printf 'Database verification failed: unexpected seed counts: %s.\n' \
        "${seed_counts}" >&2
    exit 1
fi

# The largest JSON-safe integer must remain storable, while an arithmetic
# update that crosses the frozen wire-format limit must fail atomically.
sqlite3 -batch -bail "${temporary_database}" \
    "BEGIN IMMEDIATE;
     UPDATE users
        SET balance_cents = 9007199254740991
      WHERE phone = '13800138000';
     ROLLBACK;" > /dev/null

if sqlite3 -batch -bail "${temporary_database}" \
    "BEGIN IMMEDIATE;
     UPDATE users
        SET balance_cents = 9007199254740991
      WHERE phone = '13800138000';
     UPDATE users
        SET balance_cents = balance_cents + 1
      WHERE phone = '13800138000';
     COMMIT;" > /dev/null 2>&1; then
    printf 'Database verification failed: balance overflow crossed the JSON-safe limit.\n' >&2
    exit 1
fi

# A matching reservation/order identity must be accepted.
sqlite3 -batch -bail "${temporary_database}" \
    "PRAGMA foreign_keys = ON;
     BEGIN IMMEDIATE;
     INSERT INTO reservations (user_id, charger_id, expires_at)
     VALUES (1, 1, '2099-01-01T00:00:00.000Z');
     INSERT INTO orders (
         order_no, user_id, charger_id, reservation_id, unit_price_cents_per_kwh
     ) VALUES ('VERIFY-MATCHING-ORDER', 1, 1, last_insert_rowid(), 120);
     ROLLBACK;" > /dev/null

# All three referenced rows below exist; only the reservation ownership and
# charger identity differ. The composite foreign key must reject this order.
if sqlite3 -batch -bail "${temporary_database}" \
    "PRAGMA foreign_keys = ON;
     BEGIN IMMEDIATE;
     INSERT INTO users (phone, nickname)
     VALUES ('13900139000', 'constraint-test');
     INSERT INTO reservations (user_id, charger_id, expires_at)
     VALUES (1, 1, '2099-01-01T00:00:00.000Z');
     INSERT INTO orders (
         order_no, user_id, charger_id, reservation_id, unit_price_cents_per_kwh
     ) VALUES ('VERIFY-MISMATCHED-ORDER', 2, 2, last_insert_rowid(), 120);
     COMMIT;" > /dev/null 2>&1; then
    printf 'Database verification failed: order accepted a mismatched reservation owner/charger.\n' \
        >&2
    exit 1
fi

printf 'Database verification passed: version %s, %s tables, idempotent seed, integrity/FK ok.\n' \
    "${schema_version}" "${table_count}"
