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

CREATE INDEX IF NOT EXISTS idx_stations_status
    ON stations(status);
CREATE INDEX IF NOT EXISTS idx_chargers_station_status
    ON chargers(station_id, status);
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
    ON orders(status, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_recharge_records_user_created_at
    ON recharge_records(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_operation_logs_admin_created_at
    ON operation_logs(admin_id, created_at DESC);

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

PRAGMA user_version = 1;

COMMIT;
