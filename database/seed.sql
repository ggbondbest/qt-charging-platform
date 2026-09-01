PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;

BEGIN IMMEDIATE;

-- Test-only credential required by the assignment: admin / 123456.
-- The hash is SHA-256("qtcharging-demo-salt-v1:123456").
INSERT OR IGNORE INTO admins (
    id, username, display_name, password_algorithm, password_salt,
    password_hash, status, created_at, updated_at
) VALUES (
    1,
    'admin',
    '系统管理员',
    'SHA256_SALTED',
    'qtcharging-demo-salt-v1',
    'e8bdfcb0c16995438f832177ae576861f98442ecf0cc3887feb7cee7478f6e65',
    'ACTIVE',
    '2026-09-01T00:00:00.000Z',
    '2026-09-01T00:00:00.000Z'
);

INSERT OR IGNORE INTO users (
    id, phone, nickname, avatar_key, balance_cents, status, created_at, updated_at
) VALUES (
    1,
    '13800138000',
    '用户8000',
    '',
    10000,
    'ACTIVE',
    '2026-09-01T00:00:00.000Z',
    '2026-09-01T00:00:00.000Z'
);

INSERT OR IGNORE INTO recharge_records (
    id, transaction_no, user_id, amount_cents, balance_after_cents, status, created_at
) VALUES (
    1,
    'SEED-RECHARGE-0001',
    1,
    10000,
    10000,
    'SUCCESS',
    '2026-09-01T00:00:00.000Z'
);

INSERT OR IGNORE INTO stations (
    id, code, name, address, latitude, longitude,
    price_cents_per_kwh, status, created_at, updated_at
) VALUES
    (1, 'STA-DEMO-001', '高新园区示范充电站', '大连市高新园区软件园路示范点',
     38.884700, 121.526900, 120, 'ACTIVE',
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (2, 'STA-DEMO-002', '海创中心示范充电站', '大连市高新园区黄浦路示范点',
     38.866800, 121.533200, 98, 'ACTIVE',
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (3, 'STA-DEMO-003', '生态科技城示范充电站', '大连市甘井子区生态科技城示范点',
     39.010900, 121.505500, 150, 'ACTIVE',
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z');

INSERT OR IGNORE INTO chargers (
    id, station_id, code, type, power_watts, status,
    total_charge_count, total_charge_seconds, created_at, updated_at
) VALUES
    (1, 1, 'CHG-DEMO-001-A1', 'FAST', 120000, 'AVAILABLE', 12, 34200,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (2, 1, 'CHG-DEMO-001-A2', 'FAST', 120000, 'AVAILABLE', 9, 28800,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (3, 1, 'CHG-DEMO-001-B1', 'SLOW', 7000, 'FAULT', 5, 54000,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (4, 2, 'CHG-DEMO-002-A1', 'FAST', 60000, 'AVAILABLE', 7, 21000,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (5, 2, 'CHG-DEMO-002-B1', 'SLOW', 7000, 'AVAILABLE', 3, 32400,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (6, 3, 'CHG-DEMO-003-A1', 'FAST', 180000, 'AVAILABLE', 15, 39600,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z'),
    (7, 3, 'CHG-DEMO-003-A2', 'FAST', 180000, 'OFFLINE', 11, 30600,
     '2026-09-01T00:00:00.000Z', '2026-09-01T00:00:00.000Z');

COMMIT;
