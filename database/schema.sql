PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;

BEGIN IMMEDIATE;

-- qint64 values exposed as JSON numbers are capped at 2^53 - 1 so they remain
-- exact when represented by QJsonValue's IEEE-754 double storage.
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    phone TEXT NOT NULL UNIQUE
        CHECK (
            length(phone) = 11
            AND substr(phone, 1, 1) = '1'
            AND phone NOT GLOB '*[^0-9]*'
        ),
    nickname TEXT NOT NULL CHECK (length(trim(nickname)) BETWEEN 1 AND 32),
    avatar_key TEXT NOT NULL DEFAULT '',
    balance_cents INTEGER NOT NULL DEFAULT 0
        CHECK (
            typeof(balance_cents) = 'integer'
            AND balance_cents BETWEEN 0 AND 9007199254740991
        ),
    status TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'FROZEN')),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS admins (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL COLLATE NOCASE UNIQUE
        CHECK (length(trim(username)) BETWEEN 3 AND 32),
    display_name TEXT NOT NULL CHECK (length(trim(display_name)) BETWEEN 1 AND 32),
    password_algorithm TEXT NOT NULL DEFAULT 'SHA256_SALTED',
    password_salt TEXT NOT NULL,
    password_hash TEXT NOT NULL CHECK (length(password_hash) = 64),
    status TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'DISABLED')),
    last_login_at TEXT,
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS stations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL UNIQUE CHECK (length(trim(code)) BETWEEN 1 AND 32),
    name TEXT NOT NULL CHECK (length(trim(name)) BETWEEN 1 AND 64),
    address TEXT NOT NULL CHECK (length(trim(address)) BETWEEN 1 AND 255),
    -- Legacy rows remain NULL: unknown contact information is never invented.
    city TEXT CHECK (city IS NULL OR length(trim(city)) BETWEEN 1 AND 64),
    district TEXT CHECK (district IS NULL OR length(trim(district)) BETWEEN 1 AND 64),
    contact_name TEXT CHECK (contact_name IS NULL OR length(trim(contact_name)) BETWEEN 1 AND 64),
    contact_phone TEXT CHECK (contact_phone IS NULL OR
        (length(contact_phone) = 11 AND contact_phone GLOB '1[3-9]*'
         AND contact_phone NOT GLOB '*[^0-9]*')),
    latitude REAL NOT NULL CHECK (latitude BETWEEN -90.0 AND 90.0),
    longitude REAL NOT NULL CHECK (longitude BETWEEN -180.0 AND 180.0),
    price_cents_per_kwh INTEGER NOT NULL
        CHECK (
            typeof(price_cents_per_kwh) = 'integer'
            AND price_cents_per_kwh BETWEEN 0 AND 9007199254740991
        ),
    status TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'INACTIVE')),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS chargers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id INTEGER NOT NULL,
    code TEXT NOT NULL UNIQUE CHECK (length(trim(code)) BETWEEN 1 AND 32),
    type TEXT NOT NULL CHECK (type IN ('FAST', 'SLOW')),
    power_watts INTEGER NOT NULL CHECK (power_watts > 0),
    status TEXT NOT NULL DEFAULT 'AVAILABLE'
        CHECK (status IN ('AVAILABLE', 'RESERVED', 'CHARGING', 'FAULT', 'OFFLINE')),
    total_charge_count INTEGER NOT NULL DEFAULT 0 CHECK (total_charge_count >= 0),
    total_charge_seconds INTEGER NOT NULL DEFAULT 0
        CHECK (
            typeof(total_charge_seconds) = 'integer'
            AND total_charge_seconds BETWEEN 0 AND 9007199254740991
        ),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (station_id) REFERENCES stations(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS reservations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    charger_id INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'FULFILLED', 'CANCELLED', 'EXPIRED')),
    reserved_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    expires_at TEXT NOT NULL,
    ended_at TEXT,
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    CHECK (expires_at > reserved_at),
    CHECK (
        (status = 'ACTIVE' AND ended_at IS NULL)
        OR (status <> 'ACTIVE' AND ended_at IS NOT NULL)
    ),
    UNIQUE (id, user_id, charger_id),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (charger_id) REFERENCES chargers(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS orders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    order_no TEXT NOT NULL UNIQUE CHECK (length(trim(order_no)) BETWEEN 1 AND 40),
    user_id INTEGER NOT NULL,
    charger_id INTEGER NOT NULL,
    reservation_id INTEGER UNIQUE,
    status TEXT NOT NULL DEFAULT 'RESERVED'
        CHECK (status IN ('RESERVED', 'CHARGING', 'WAITING_PAYMENT', 'COMPLETED', 'CANCELLED')),
    unit_price_cents_per_kwh INTEGER NOT NULL
        CHECK (
            typeof(unit_price_cents_per_kwh) = 'integer'
            AND unit_price_cents_per_kwh BETWEEN 0 AND 9007199254740991
        ),
    energy_wh INTEGER NOT NULL DEFAULT 0
        CHECK (
            typeof(energy_wh) = 'integer'
            AND energy_wh BETWEEN 0 AND 9007199254740991
        ),
    duration_seconds INTEGER NOT NULL DEFAULT 0
        CHECK (
            typeof(duration_seconds) = 'integer'
            AND duration_seconds BETWEEN 0 AND 9007199254740991
        ),
    amount_cents INTEGER NOT NULL DEFAULT 0
        CHECK (
            typeof(amount_cents) = 'integer'
            AND amount_cents BETWEEN 0 AND 9007199254740991
        ),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    started_at TEXT,
    stopped_at TEXT,
    paid_at TEXT,
    stop_reason TEXT CHECK (stop_reason IS NULL OR stop_reason IN
        ('TARGET_AMOUNT', 'TARGET_ENERGY', 'TARGET_DURATION', 'MANUAL')),
    telemetry_captured_at TEXT,
    telemetry_power_watts INTEGER CHECK (telemetry_power_watts IS NULL OR
        (typeof(telemetry_power_watts) = 'integer' AND telemetry_power_watts > 0)),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    CHECK (status NOT IN ('CHARGING', 'WAITING_PAYMENT', 'COMPLETED') OR started_at IS NOT NULL),
    CHECK (status NOT IN ('WAITING_PAYMENT', 'COMPLETED') OR stopped_at IS NOT NULL),
    CHECK (status <> 'COMPLETED' OR paid_at IS NOT NULL),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (charger_id) REFERENCES chargers(id)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (reservation_id, user_id, charger_id)
        REFERENCES reservations(id, user_id, charger_id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

-- One immutable tariff policy per new order. No backfill for historic orders:
-- their amount alone cannot prove a breakdown or which policy was applied.
CREATE TABLE IF NOT EXISTS order_pricing_snapshots (
    order_id INTEGER PRIMARY KEY,
    version TEXT NOT NULL CHECK (version = 'energy-only-v1'),
    unit_price_cents_per_kwh INTEGER NOT NULL CHECK (
        typeof(unit_price_cents_per_kwh) = 'integer'
        AND unit_price_cents_per_kwh BETWEEN 0 AND 9007199254740991),
    captured_at TEXT NOT NULL,
    FOREIGN KEY (order_id) REFERENCES orders(id) ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS charger_exceptions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    charger_id INTEGER NOT NULL,
    code TEXT NOT NULL CHECK (code IN ('SIMULATED_FAULT', 'SIMULATED_OFFLINE')),
    severity TEXT NOT NULL CHECK (severity IN ('WARNING', 'CRITICAL')),
    safe_summary TEXT NOT NULL CHECK (length(safe_summary) BETWEEN 1 AND 256),
    status TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'ACKNOWLEDGED', 'RECOVERING', 'RECOVERED')),
    occurred_at TEXT NOT NULL,
    acknowledged_at TEXT,
    recovered_at TEXT,
    recoverable INTEGER NOT NULL DEFAULT 1 CHECK (recoverable IN (0, 1)),
    recovery_action TEXT NOT NULL DEFAULT 'SIMULATE_RESTORE'
        CHECK (recovery_action = 'SIMULATE_RESTORE'),
    recovered_by_admin_id INTEGER,
    recovery_command_id TEXT,
    recovery_message TEXT CHECK (recovery_message IS NULL OR length(recovery_message) <= 256),
    updated_at TEXT NOT NULL,
    FOREIGN KEY (charger_id) REFERENCES chargers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (recovered_by_admin_id) REFERENCES admins(id) ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE INDEX IF NOT EXISTS idx_charger_exceptions_charger_status_occurred
    ON charger_exceptions(charger_id, status, occurred_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_charger_exceptions_occurred
    ON charger_exceptions(occurred_at DESC, id DESC);

CREATE TABLE IF NOT EXISTS recharge_records (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    transaction_no TEXT NOT NULL UNIQUE
        CHECK (length(trim(transaction_no)) BETWEEN 1 AND 40),
    user_id INTEGER NOT NULL,
    amount_cents INTEGER NOT NULL
        CHECK (
            typeof(amount_cents) = 'integer'
            AND amount_cents BETWEEN 1 AND 9007199254740991
        ),
    balance_after_cents INTEGER NOT NULL
        CHECK (
            typeof(balance_after_cents) = 'integer'
            AND balance_after_cents BETWEEN 0 AND 9007199254740991
        ),
    status TEXT NOT NULL CHECK (status IN ('SUCCESS', 'FAILED')),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS operation_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    admin_id INTEGER,
    action TEXT NOT NULL CHECK (length(trim(action)) BETWEEN 1 AND 64),
    target_type TEXT NOT NULL CHECK (length(trim(target_type)) BETWEEN 1 AND 32),
    target_id TEXT NOT NULL DEFAULT '',
    details_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (admin_id) REFERENCES admins(id)
        ON UPDATE CASCADE ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS notifications (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    type TEXT NOT NULL CHECK (type IN ('CHARGING_STOPPED', 'ORDER_PAID',
        'RESERVATION_EXPIRY_REMINDER', 'QUEUE_CALLED', 'QUEUE_EXPIRED', 'REPAIR_UPDATED')),
    title TEXT NOT NULL CHECK (length(trim(title)) BETWEEN 1 AND 64),
    body TEXT NOT NULL CHECK (length(trim(body)) BETWEEN 1 AND 512),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    read_at TEXT,
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS queue_entries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    charger_id INTEGER NOT NULL REFERENCES chargers(id) ON DELETE RESTRICT,
    status TEXT NOT NULL DEFAULT 'WAITING' CHECK (status IN ('WAITING','CALLED','CONFIRMED','LEFT','EXPIRED')),
    entered_at TEXT NOT NULL,
    called_at TEXT,
    call_expires_at TEXT,
    ended_at TEXT,
    reservation_id INTEGER REFERENCES reservations(id) ON DELETE RESTRICT,
    updated_at TEXT NOT NULL,
    join_operation_id TEXT NOT NULL CHECK (length(join_operation_id) BETWEEN 1 AND 64),
    confirm_operation_id TEXT,
    leave_operation_id TEXT,
    UNIQUE(user_id, join_operation_id),
    UNIQUE(user_id, confirm_operation_id),
    UNIQUE(user_id, leave_operation_id)
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_queue_active_user ON queue_entries(user_id)
    WHERE status IN ('WAITING','CALLED');
CREATE UNIQUE INDEX IF NOT EXISTS ux_queue_called_charger ON queue_entries(charger_id)
    WHERE status = 'CALLED';
CREATE INDEX IF NOT EXISTS idx_queue_fifo ON queue_entries(charger_id,status,entered_at,id);

CREATE TABLE IF NOT EXISTS repair_reports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    charger_id INTEGER NOT NULL REFERENCES chargers(id) ON DELETE RESTRICT,
    problem_type TEXT NOT NULL CHECK (problem_type IN ('CONNECTION','SCREEN','CONNECTOR','CHARGING','OTHER')),
    description TEXT NOT NULL CHECK (length(trim(description)) BETWEEN 1 AND 200),
    status TEXT NOT NULL DEFAULT 'SUBMITTED' CHECK (status IN ('SUBMITTED','ACCEPTED','PROCESSING','RESOLVED')),
    processing_note TEXT NOT NULL DEFAULT '' CHECK (length(processing_note) <= 200),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    resolved_at TEXT
);
CREATE TABLE IF NOT EXISTS repair_timeline (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    report_id INTEGER NOT NULL REFERENCES repair_reports(id) ON DELETE RESTRICT,
    status TEXT NOT NULL CHECK (status IN ('SUBMITTED','ACCEPTED','PROCESSING','RESOLVED')),
    note TEXT NOT NULL CHECK (length(note) <= 200),
    admin_id INTEGER REFERENCES admins(id) ON DELETE SET NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS repair_operations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    actor_type TEXT NOT NULL CHECK (actor_type IN ('USER','ADMIN')),
    actor_id INTEGER NOT NULL,
    operation_id TEXT NOT NULL CHECK (length(operation_id) BETWEEN 1 AND 64),
    action TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    result_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    UNIQUE(actor_type,actor_id,operation_id)
);
CREATE INDEX IF NOT EXISTS idx_repair_user_created ON repair_reports(user_id,created_at DESC,id DESC);
CREATE INDEX IF NOT EXISTS idx_repair_charger_status ON repair_reports(charger_id,status);
CREATE INDEX IF NOT EXISTS idx_repair_status_updated ON repair_reports(status,updated_at);
CREATE INDEX IF NOT EXISTS idx_repair_timeline_report ON repair_timeline(report_id,id);

CREATE TABLE IF NOT EXISTS order_charge_targets (
    order_id INTEGER PRIMARY KEY REFERENCES orders(id) ON DELETE RESTRICT,
    target_type TEXT NOT NULL CHECK (target_type IN ('AMOUNT','ENERGY','DURATION')),
    target_value INTEGER NOT NULL CHECK (typeof(target_value) = 'integer'
        AND target_value BETWEEN 1 AND 9007199254740991),
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS coupons (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('CASH', 'DISCOUNT')),
    title TEXT NOT NULL CHECK (length(trim(title)) BETWEEN 1 AND 64),
    value_cents INTEGER NOT NULL DEFAULT 0
        CHECK (value_cents BETWEEN 0 AND 9007199254740991),
    discount_tenths INTEGER
        CHECK (discount_tenths IS NULL OR discount_tenths BETWEEN 1 AND 99),
    threshold_cents INTEGER NOT NULL DEFAULT 0
        CHECK (threshold_cents BETWEEN 0 AND 9007199254740991),
    status TEXT NOT NULL DEFAULT 'AVAILABLE'
        CHECK (status IN ('AVAILABLE', 'USED', 'EXPIRED')),
    source TEXT NOT NULL CHECK (length(trim(source)) BETWEEN 1 AND 32),
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

-- 批次C（2026-09-08）签到/积分：total = SUM(amount)，无独立余额列——
-- 单一事实源，杜绝余额与流水对不上账。reason 词表 CHECK_IN 之外
-- TODO(contract) 运营扩展（任务/活动发放）。
CREATE TABLE IF NOT EXISTS points_ledger (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    amount INTEGER NOT NULL
        CHECK (amount BETWEEN -9007199254740991 AND 9007199254740991),
    reason TEXT NOT NULL CHECK (length(trim(reason)) BETWEEN 1 AND 32),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

-- 日粒度签到幂等表：(user_id, day) 主键即唯一约束，day 为 UTC 日历日
-- "YYYY-MM-DD"（与 CHECK_IN 响应 day 字段同口径）。
CREATE TABLE IF NOT EXISTS user_checkins (
    user_id INTEGER NOT NULL,
    day TEXT NOT NULL
        CHECK (day GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    PRIMARY KEY (user_id, day),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

-- 批次E（2026-09-08）电桩评价：一单一评，order_id UNIQUE 即幂等锚（镜像服务端
-- INSERT OR IGNORE + 客户端重放不报错）。评价对象绑定订单所属桩（charger_id 快照，
-- 防后续订单改绑漂移）。comment 可空串（TEXT NOT NULL DEFAULT ''），最长 140 字。
-- TODO(contract): 是否允许改评/删评（一期不可）。
CREATE TABLE IF NOT EXISTS charger_ratings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    charger_id INTEGER NOT NULL,
    order_id INTEGER NOT NULL UNIQUE,
    rating INTEGER NOT NULL
        CHECK (rating BETWEEN 1 AND 5),
    comment TEXT NOT NULL DEFAULT ''
        CHECK (length(comment) <= 140),
    created_at TEXT NOT NULL
        DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (user_id) REFERENCES users(id)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (charger_id) REFERENCES chargers(id)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (order_id) REFERENCES orders(id)
        ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE INDEX IF NOT EXISTS idx_charger_ratings_user_created_at
    ON charger_ratings(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_stations_status
    ON stations(status);
CREATE INDEX IF NOT EXISTS idx_chargers_station_status
    ON chargers(station_id, status);
CREATE INDEX IF NOT EXISTS idx_chargers_abnormal_updated_at
    ON chargers(updated_at DESC, id DESC)
    WHERE status IN ('FAULT','OFFLINE');
CREATE INDEX IF NOT EXISTS idx_chargers_updated_at
    ON chargers(updated_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_reservations_user_status
    ON reservations(user_id, status);
CREATE INDEX IF NOT EXISTS idx_reservations_charger_status
    ON reservations(charger_id, status);
CREATE INDEX IF NOT EXISTS idx_reservations_expires_at
    ON reservations(expires_at);
CREATE INDEX IF NOT EXISTS idx_orders_user_created_at
    ON orders(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_orders_charger_status
    ON orders(charger_id, status);
CREATE INDEX IF NOT EXISTS idx_orders_status_created_at
    ON orders(status, created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_orders_created_at
    ON orders(created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_users_status_id
    ON users(status, id);
CREATE INDEX IF NOT EXISTS idx_recharge_records_user_created_at
    ON recharge_records(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_recharge_records_status_created_at
    ON recharge_records(status, created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_recharge_records_created_at
    ON recharge_records(created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_operation_logs_admin_created_at
    ON operation_logs(admin_id, created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_operation_logs_action_created_at
    ON operation_logs(action, created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_operation_logs_created_at
    ON operation_logs(created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_notifications_user_created_at
    ON notifications(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_coupons_user_status
    ON coupons(user_id, status);
CREATE INDEX IF NOT EXISTS idx_coupons_user_created_at
    ON coupons(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_points_ledger_user_created_at
    ON points_ledger(user_id, created_at DESC);

CREATE UNIQUE INDEX IF NOT EXISTS ux_reservations_active_user
    ON reservations(user_id)
    WHERE status = 'ACTIVE';
CREATE UNIQUE INDEX IF NOT EXISTS ux_reservations_active_charger
    ON reservations(charger_id)
    WHERE status = 'ACTIVE';
CREATE UNIQUE INDEX IF NOT EXISTS ux_orders_unfinished_user
    ON orders(user_id)
    WHERE status IN ('RESERVED', 'CHARGING', 'WAITING_PAYMENT');
CREATE UNIQUE INDEX IF NOT EXISTS ux_orders_active_charger
    ON orders(charger_id)
    WHERE status IN ('RESERVED', 'CHARGING');

-- v4 adds optional legacy station contact columns, persisted simulated meter
-- samples, independent exception events and explicit order tariff snapshots.
-- DatabaseConnection atomically adds missing columns on supported v1-v3 files.
PRAGMA user_version = 5;

COMMIT;
