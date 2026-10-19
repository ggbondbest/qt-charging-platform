"""Read-only current Qt business snapshots, independent of historical analytics.

Only the server-configured SQLite file is opened. Each request uses one read
transaction; no users, account balances, contact details or business writes are
exposed. Amounts stay in integer cents and energy in integer Wh.
"""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import sqlite3
import time
from typing import Generic, Literal, TypeVar
import uuid

from fastapi import APIRouter, FastAPI, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict


PREFIX = "/api/v1/qt-live"
MAX_SAFE_INTEGER = 2**53 - 1
CHARGER_STATES = ("AVAILABLE", "RESERVED", "CHARGING", "FAULT", "OFFLINE")
ORDER_STATES = ("RESERVED", "CHARGING", "WAITING_PAYMENT", "COMPLETED", "CANCELLED")
SHANGHAI = timezone(timedelta(hours=8), "Asia/Shanghai")
OrderStatus = Literal["RESERVED", "CHARGING", "WAITING_PAYMENT", "COMPLETED", "CANCELLED"]
REQUIRED_COLUMNS = {
    "stations": {"id", "code", "name", "city", "district", "status", "price_cents_per_kwh"},
    "chargers": {"id", "station_id", "code", "status", "updated_at"},
    "users": {"id"},
    "orders": {"id", "charger_id", "status", "amount_cents", "energy_wh",
               "created_at", "started_at", "stopped_at", "paid_at"},
    "charger_exceptions": {"id", "charger_id", "code", "severity", "status", "occurred_at"},
}


class QtLiveError(Exception):
    """Safe public errors for this separate, non-published data domain."""

    def __init__(self, code: str, message: str, status: int = 503):
        super().__init__(message)
        self.code, self.message, self.status = code, message, status


class QtModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class QtSource(QtModel):
    dataSource: Literal["qt-sqlite"] = "qt-sqlite"
    asOf: str
    simulated: Literal[True] = True


class QtChargerCounts(QtModel):
    AVAILABLE: int
    RESERVED: int
    CHARGING: int
    FAULT: int
    OFFLINE: int


class QtOrderCounts(QtModel):
    RESERVED: int
    CHARGING: int
    WAITING_PAYMENT: int
    COMPLETED: int
    CANCELLED: int


class QtSummary(QtModel):
    totalUsers: int
    totalStations: int
    activeStations: int
    totalChargers: int
    chargerStatusCounts: QtChargerCounts
    totalOrders: int
    orderStatusCounts: QtOrderCounts
    chargingOrders: int
    completedOrders: int
    paidAmountCents: int
    completedEnergyWh: int
    activeOrders: int
    onlineChargers: int
    onlineRatio: float
    todayOrders: int
    todayCompletedOrders: int
    todayRevenueCents: int
    monthRevenueCents: int
    todayCompletedEnergyWh: int


class QtStation(QtModel):
    qtStationId: int
    qtStationCode: str
    name: str
    city: str | None
    district: str | None
    status: Literal["ACTIVE", "INACTIVE"]
    priceCentsPerKwh: int
    totalChargers: int
    chargerStatusCounts: QtChargerCounts


class QtOrder(QtModel):
    qtOrderId: int
    qtStationId: int
    qtStationCode: str
    stationName: str
    chargerCode: str
    status: OrderStatus
    amountCents: int
    energyWh: int
    createdAt: str
    startedAt: str | None
    stoppedAt: str | None
    paidAt: str | None


class QtHealth(QtSource):
    status: Literal["ready"] = "ready"
    readOnly: Literal[True] = True


class QtSummarySnapshot(QtSource):
    summary: QtSummary


class QtLimits(QtModel):
    stations: int
    orders: int


class QtTruncated(QtModel):
    stations: bool
    orders: bool


class QtRevenuePoint(QtModel):
    date: str
    revenueCents: int
    completedOrderCount: int
    completedEnergyWh: int


class QtActiveException(QtModel):
    qtExceptionId: int
    code: Literal["SIMULATED_FAULT", "SIMULATED_OFFLINE"]
    severity: Literal["WARNING", "CRITICAL"]
    status: Literal["ACTIVE", "ACKNOWLEDGED", "RECOVERING"]
    safeSummary: str
    occurredAt: str


class QtAbnormalCharger(QtModel):
    qtChargerId: int
    chargerCode: str
    qtStationId: int
    qtStationCode: str
    stationName: str
    status: Literal["AVAILABLE", "RESERVED", "CHARGING", "FAULT", "OFFLINE"]
    updatedAt: str
    activeException: QtActiveException | None


class QtAbnormalChargers(QtModel):
    total: int
    items: list[QtAbnormalCharger]


class QtOverview(QtModel):
    timeZone: Literal["Asia/Shanghai"] = "Asia/Shanghai"
    today: str
    trendDays: Literal[7, 30]
    revenueTrend: list[QtRevenuePoint]
    abnormalChargers: QtAbnormalChargers


class QtSnapshot(QtSummarySnapshot):
    overview: QtOverview
    stations: list[QtStation]
    orders: list[QtOrder]
    limits: QtLimits
    truncated: QtTruncated


class QtStationPage(QtSource):
    stations: list[QtStation]
    total: int
    limit: int
    offset: int


class QtOrderPage(QtSource):
    orders: list[QtOrder]
    total: int
    limit: int
    offset: int


class QtMeta(QtModel):
    requestId: str
    dataSource: Literal["qt-sqlite"] = "qt-sqlite"
    scope: Literal["CURRENT_QT_BUSINESS"] = "CURRENT_QT_BUSINESS"
    mergedWithHistory: Literal[False] = False
    simulated: Literal[True] = True


T = TypeVar("T")


class QtEnvelope(QtModel, Generic[T]):
    code: str
    message: str
    data: T | None
    meta: QtMeta


def _number(value):
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= MAX_SAFE_INTEGER:
        raise QtLiveError("QT_DATA_INVALID", "Qt业务数据数值无效或超出安全显示范围")
    return value


def _utc(value):
    if value is None:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise ValueError("Timezone is required")
        return parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    except (AttributeError, TypeError, ValueError, OverflowError):
        raise QtLiveError("QT_DATA_INVALID", "Qt业务记录时间格式无效，请检查服务端数据库") from None


def _source(as_of):
    return {"dataSource": "qt-sqlite", "asOf": as_of, "simulated": True}


def _business_day(value):
    normalized = _utc(value)
    if normalized is None:
        raise QtLiveError("QT_DATA_INVALID", "Qt业务记录缺少统计时间")
    return datetime.fromisoformat(normalized.replace("Z", "+00:00")).astimezone(SHANGHAI).date().isoformat()


class QtLiveAdapter:
    def __init__(self, database_path=None, *, timeout_seconds=3.0):
        # Read configuration once. HTTP callers can never choose a filesystem path.
        self.database_path = database_path if database_path is not None else os.getenv("QT_SQLITE_PATH")
        self.timeout_seconds = timeout_seconds

    @contextmanager
    def _read(self):
        if not self.database_path:
            raise QtLiveError("QT_DATABASE_NOT_CONFIGURED", "尚未配置Qt业务数据库，请设置QT_SQLITE_PATH")
        connection = None
        try:
            path = Path(self.database_path).expanduser()
            if not path.is_absolute() or not path.is_file():
                raise QtLiveError("QT_DATABASE_UNAVAILABLE", "Qt业务数据库不可用，请核对服务端数据库绝对路径和读取权限")
            uri = path.resolve().as_uri() + "?mode=ro"
            connection = sqlite3.connect(uri, uri=True, timeout=self.timeout_seconds, isolation_level=None)
            connection.row_factory = sqlite3.Row
            connection.create_function("qt_business_day", 1, _business_day, deterministic=True)
            connection.execute("PRAGMA query_only = ON")
            connection.execute("PRAGMA trusted_schema = OFF")
            deadline = time.monotonic() + 5.0
            connection.set_progress_handler(lambda: int(time.monotonic() > deadline), 10000)
            connection.execute("BEGIN")
            self._validate_schema(connection)
            # The schema read pins the SQLite snapshot; all following reads share it.
            as_of = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
            yield connection, as_of
        except sqlite3.Error as exc:
            code = getattr(exc, "sqlite_errorcode", 0) & 0xFF
            if code in (sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED):
                raise QtLiveError("QT_DATABASE_BUSY", "Qt业务数据库繁忙，请稍后刷新") from None
            if code == sqlite3.SQLITE_INTERRUPT:
                raise QtLiveError("QT_DATABASE_BUSY", "Qt业务查询超时，请稍后刷新") from None
            if "integer overflow" in str(exc).lower():
                raise QtLiveError("QT_DATA_INVALID", "Qt业务汇总数值超出安全显示范围") from None
            if "user-defined function raised exception" in str(exc).lower():
                raise QtLiveError("QT_DATA_INVALID", "Qt业务记录时间格式无效，请检查服务端数据库") from None
            raise QtLiveError("QT_DATABASE_UNAVAILABLE", "Qt业务数据库无法读取，请检查文件和服务状态") from None
        except (OSError, ValueError):
            raise QtLiveError("QT_DATABASE_UNAVAILABLE", "Qt业务数据库无法读取，请核对配置和权限") from None
        finally:
            if connection is not None:
                # Roll back the read transaction and release its locks on every path.
                try:
                    connection.rollback()
                finally:
                    connection.close()

    @staticmethod
    def _validate_schema(connection):
        tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
        for table, required in REQUIRED_COLUMNS.items():
            if table not in tables:
                raise QtLiveError("QT_SCHEMA_MISMATCH", "Qt业务数据库结构不匹配，请使用当前Qt服务端数据库")
            # Table names are fixed constants, never request inputs.
            columns = {row[1] for row in connection.execute(f"PRAGMA table_info({table})")}
            if not required <= columns:
                raise QtLiveError("QT_SCHEMA_MISMATCH", "Qt业务数据库结构不匹配，请升级Qt服务端数据库")

    @staticmethod
    def _counts(connection, table, states):
        counts = dict.fromkeys(states, 0)
        for row in connection.execute(f"SELECT status, COUNT(*) AS n FROM {table} GROUP BY status"):
            if row["status"] not in counts:
                raise QtLiveError("QT_DATA_INVALID", "Qt业务状态无法识别，请检查服务端数据库")
            counts[row["status"]] = _number(row["n"])
        return counts

    @staticmethod
    def _paid_days(connection):
        rows = connection.execute("""SELECT qt_business_day(paid_at) AS day,
            COUNT(*) AS count, COALESCE(SUM(amount_cents),0) AS amount,
            COALESCE(SUM(energy_wh),0) AS energy FROM orders
            WHERE status='COMPLETED' GROUP BY qt_business_day(paid_at)""")
        return {row["day"]: dict(date=row["day"], revenueCents=_number(row["amount"]),
                                completedOrderCount=_number(row["count"]), completedEnergyWh=_number(row["energy"]))
                for row in rows}

    def _summary(self, connection, as_of, paid_days):
        stations = self._counts(connection, "stations", ("ACTIVE", "INACTIVE"))
        chargers = self._counts(connection, "chargers", CHARGER_STATES)
        orders = self._counts(connection, "orders", ORDER_STATES)
        today = _business_day(as_of)
        paid_today = paid_days.get(today, {})
        total_chargers = _number(sum(chargers.values()))
        online = total_chargers - chargers["OFFLINE"]
        today_orders = connection.execute("SELECT COUNT(*) FROM orders WHERE qt_business_day(created_at)=?", (today,)).fetchone()[0]
        return dict(totalUsers=_number(connection.execute("SELECT COUNT(*) FROM users").fetchone()[0]),
                    totalStations=_number(sum(stations.values())), activeStations=stations["ACTIVE"],
                    totalChargers=_number(sum(chargers.values())), chargerStatusCounts=chargers,
                    totalOrders=_number(sum(orders.values())), orderStatusCounts=orders,
                    chargingOrders=orders["CHARGING"], completedOrders=orders["COMPLETED"],
                    paidAmountCents=_number(sum(day["revenueCents"] for day in paid_days.values())),
                    completedEnergyWh=_number(sum(day["completedEnergyWh"] for day in paid_days.values())),
                    activeOrders=_number(sum(orders[state] for state in ("RESERVED", "CHARGING", "WAITING_PAYMENT"))),
                    onlineChargers=online, onlineRatio=online / total_chargers if total_chargers else 0.0,
                    todayOrders=_number(today_orders), todayCompletedOrders=paid_today.get("completedOrderCount", 0),
                    todayRevenueCents=paid_today.get("revenueCents", 0),
                    monthRevenueCents=_number(sum(day["revenueCents"] for date, day in paid_days.items() if date[:7] == today[:7])),
                    todayCompletedEnergyWh=paid_today.get("completedEnergyWh", 0))

    @staticmethod
    def _overview(connection, as_of, paid_days, trend_days):
        today = datetime.fromisoformat(as_of.replace("Z", "+00:00")).astimezone(SHANGHAI).date()
        trend = []
        for offset in range(trend_days - 1, -1, -1):
            date = (today - timedelta(days=offset)).isoformat()
            trend.append(paid_days.get(date, dict(date=date, revenueCents=0, completedOrderCount=0, completedEnergyWh=0)))
        abnormal_filter = """c.status IN ('FAULT','OFFLINE') OR EXISTS (
            SELECT 1 FROM charger_exceptions x WHERE x.charger_id=c.id
            AND x.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING'))"""
        total = _number(connection.execute("SELECT COUNT(*) FROM chargers c WHERE " + abnormal_filter).fetchone()[0])
        rows = connection.execute("""SELECT c.id, c.code, c.status, c.updated_at,
            s.id AS station_id, s.code AS station_code, s.name AS station_name,
            e.id AS exception_id, e.code AS exception_code, e.severity, e.status AS exception_status, e.occurred_at
            FROM chargers c JOIN stations s ON s.id=c.station_id
            LEFT JOIN charger_exceptions e ON e.id=(SELECT x.id FROM charger_exceptions x WHERE x.charger_id=c.id
                AND x.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING') ORDER BY julianday(x.occurred_at) DESC,x.id DESC LIMIT 1)
            WHERE """ + abnormal_filter + " ORDER BY julianday(c.updated_at) DESC,c.id DESC LIMIT 5")
        items = []
        for row in rows:
            event = None
            if row["exception_id"] is not None:
                # Use controlled labels, never arbitrary operator text, identities or recovery notes.
                summaries = {"SIMULATED_FAULT": "模拟电桩故障", "SIMULATED_OFFLINE": "模拟电桩离线"}
                if row["exception_code"] not in summaries or row["severity"] not in ("WARNING", "CRITICAL"):
                    raise QtLiveError("QT_DATA_INVALID", "Qt异常事件分类无法识别")
                event = dict(qtExceptionId=_number(row["exception_id"]), code=row["exception_code"],
                             severity=row["severity"], status=row["exception_status"],
                             safeSummary=summaries[row["exception_code"]], occurredAt=_utc(row["occurred_at"]))
            items.append(dict(qtChargerId=_number(row["id"]), chargerCode=row["code"],
                qtStationId=_number(row["station_id"]), qtStationCode=row["station_code"], stationName=row["station_name"],
                status=row["status"], updatedAt=_utc(row["updated_at"]), activeException=event))
        return dict(timeZone="Asia/Shanghai", today=today.isoformat(), trendDays=trend_days,
                    revenueTrend=trend, abnormalChargers=dict(total=total, items=items))

    @staticmethod
    def _stations(connection, limit, offset=0):
        rows = connection.execute("""WITH selected AS (
            SELECT id, code, name, city, district, status, price_cents_per_kwh
            FROM stations ORDER BY id LIMIT ? OFFSET ?
        ) SELECT s.*, c.status AS charger_status, COUNT(c.id) AS charger_count
          FROM selected s LEFT JOIN chargers c ON c.station_id = s.id
          GROUP BY s.id, c.status ORDER BY s.id""", (limit, offset))
        items = {}
        for row in rows:
            identity = _number(row["id"])
            if row["status"] not in ("ACTIVE", "INACTIVE"):
                raise QtLiveError("QT_DATA_INVALID", "Qt业务状态无法识别，请检查服务端数据库")
            if identity not in items:
                items[identity] = dict(qtStationId=identity, qtStationCode=row["code"], name=row["name"],
                    city=row["city"], district=row["district"], status=row["status"],
                    priceCentsPerKwh=_number(row["price_cents_per_kwh"]), totalChargers=0,
                    chargerStatusCounts=dict.fromkeys(CHARGER_STATES, 0))
            status, count = row["charger_status"], _number(row["charger_count"])
            if status is None and count == 0:
                continue
            if status not in CHARGER_STATES:
                raise QtLiveError("QT_DATA_INVALID", "Qt业务状态无法识别，请检查服务端数据库")
            items[identity]["chargerStatusCounts"][status] = count
            items[identity]["totalChargers"] += count
        return list(items.values())

    @staticmethod
    def _orders(connection, limit, offset=0, status=None):
        rows = connection.execute("""SELECT o.id, s.id AS station_id, s.code AS station_code,
            s.name AS station_name, c.code AS charger_code, o.status, o.amount_cents,
            o.energy_wh, o.created_at, o.started_at, o.stopped_at, o.paid_at
            FROM orders o JOIN chargers c ON c.id = o.charger_id JOIN stations s ON s.id = c.station_id
            WHERE (? IS NULL OR o.status = ?) ORDER BY o.id DESC LIMIT ? OFFSET ?""",
            (status, status, limit, offset))
        items = []
        for row in rows:
            if row["status"] not in ORDER_STATES:
                raise QtLiveError("QT_DATA_INVALID", "Qt业务状态无法识别，请检查服务端数据库")
            items.append(dict(qtOrderId=_number(row["id"]), qtStationId=_number(row["station_id"]),
                qtStationCode=row["station_code"], stationName=row["station_name"], chargerCode=row["charger_code"],
                status=row["status"], amountCents=_number(row["amount_cents"]), energyWh=_number(row["energy_wh"]),
                createdAt=_utc(row["created_at"]), startedAt=_utc(row["started_at"]),
                stoppedAt=_utc(row["stopped_at"]), paidAt=_utc(row["paid_at"])))
        return items

    @staticmethod
    def _paging(limit, offset, maximum):
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= maximum:
            raise QtLiveError("INVALID_ARGUMENT", "查询条数超出允许范围", 422)
        if isinstance(offset, bool) or not isinstance(offset, int) or not 0 <= offset <= 1000000:
            raise QtLiveError("INVALID_ARGUMENT", "查询偏移超出允许范围", 422)

    def health(self):
        with self._read() as (_, as_of):
            return {**_source(as_of), "status": "ready", "readOnly": True}

    def summary(self):
        with self._read() as (connection, as_of):
            return {**_source(as_of), "summary": self._summary(connection, as_of, self._paid_days(connection))}

    def snapshot(self, *, station_limit=500, order_limit=20, trend_days=7):
        self._paging(station_limit, 0, 500)
        self._paging(order_limit, 0, 100)
        if type(trend_days) is not int or trend_days not in (7, 30):
            raise QtLiveError("INVALID_ARGUMENT", "营收趋势仅支持近7天或30天", 422)
        with self._read() as (connection, as_of):
            paid_days = self._paid_days(connection)
            summary = self._summary(connection, as_of, paid_days)
            return {**_source(as_of), "summary": summary,
                    "overview": self._overview(connection, as_of, paid_days, trend_days),
                    "stations": self._stations(connection, station_limit),
                    "orders": self._orders(connection, order_limit),
                    "limits": {"stations": station_limit, "orders": order_limit},
                    "truncated": {"stations": summary["totalStations"] > station_limit,
                                  "orders": summary["totalOrders"] > order_limit}}

    def stations(self, *, limit=100, offset=0):
        self._paging(limit, offset, 500)
        with self._read() as (connection, as_of):
            total = _number(connection.execute("SELECT COUNT(*) FROM stations").fetchone()[0])
            return {**_source(as_of), "stations": self._stations(connection, limit, offset),
                    "total": total, "limit": limit, "offset": offset}

    def orders(self, *, limit=20, offset=0, status=None):
        self._paging(limit, offset, 100)
        if status is not None and status not in ORDER_STATES:
            raise QtLiveError("INVALID_ARGUMENT", "订单状态无效", 422)
        with self._read() as (connection, as_of):
            total = _number(connection.execute("SELECT COUNT(*) FROM orders WHERE (? IS NULL OR status = ?)",
                                               (status, status)).fetchone()[0])
            return {**_source(as_of), "orders": self._orders(connection, limit, offset, status),
                    "total": total, "limit": limit, "offset": offset}


def qt_envelope(request, data=None, *, code="OK", message="成功"):
    return dict(code=code, message=message, data=data,
                meta=dict(requestId=getattr(request.state, "request_id", uuid.uuid4().hex),
                          dataSource="qt-sqlite", scope="CURRENT_QT_BUSINESS",
                          mergedWithHistory=False, simulated=True))


def register_qt_live(application: FastAPI, database_path=None):
    """Register before the '/' mount. Historical providers are never called."""
    adapter = QtLiveAdapter(database_path)
    application.state.qt_live = adapter
    router = APIRouter(prefix=PREFIX, tags=["Qt current simulated business"])
    errors = {503: {"model": QtEnvelope[dict], "description": "Qt SQLite unavailable; no fabricated zeros"},
              422: {"model": QtEnvelope[dict], "description": "Invalid pagination or status"}}

    @router.get("/health", response_model=QtEnvelope[QtHealth], responses=errors)
    def health(request: Request):
        return qt_envelope(request, adapter.health())

    @router.get("/snapshot", response_model=QtEnvelope[QtSnapshot], responses=errors)
    def snapshot(request: Request, stationLimit: int = Query(500, ge=1, le=500),
                 orderLimit: int = Query(20, ge=1, le=100), trendDays: int = Query(7, ge=7, le=30)):
        return qt_envelope(request, adapter.snapshot(station_limit=stationLimit, order_limit=orderLimit, trend_days=trendDays))

    @router.get("/summary", response_model=QtEnvelope[QtSummarySnapshot], responses=errors)
    def summary(request: Request):
        return qt_envelope(request, adapter.summary())

    @router.get("/stations", response_model=QtEnvelope[QtStationPage], responses=errors)
    def stations(request: Request, limit: int = Query(100, ge=1, le=500),
                 offset: int = Query(0, ge=0, le=1000000)):
        return qt_envelope(request, adapter.stations(limit=limit, offset=offset))

    @router.get("/orders", response_model=QtEnvelope[QtOrderPage], responses=errors)
    def orders(request: Request, limit: int = Query(20, ge=1, le=100),
               offset: int = Query(0, ge=0, le=1000000), status: OrderStatus | None = None):
        return qt_envelope(request, adapter.orders(limit=limit, offset=offset, status=status))

    @application.exception_handler(QtLiveError)
    async def qt_error(request, exc):
        return JSONResponse(status_code=exc.status,
                            content=qt_envelope(request, code=exc.code, message=exc.message))

    previous_validation = application.exception_handlers.get(RequestValidationError)

    @application.exception_handler(RequestValidationError)
    async def invalid_request(request, exc):
        if request.url.path.startswith(PREFIX + "/"):
            return JSONResponse(status_code=422,
                content=qt_envelope(request, code="INVALID_ARGUMENT", message="Qt业务查询参数格式或取值不正确"))
        if previous_validation is not None:
            return await previous_validation(request, exc)
        return JSONResponse(status_code=422,
                            content=qt_envelope(request, code="INVALID_ARGUMENT", message="请求参数格式或取值不正确"))

    @application.middleware("http")
    async def qt_request_metadata(request, call_next):
        if not request.url.path.startswith(PREFIX + "/"):
            return await call_next(request)
        request.state.request_id = uuid.uuid4().hex
        try:
            response = await call_next(request)
        except Exception:
            response = JSONResponse(status_code=500,
                content=qt_envelope(request, code="INTERNAL_ERROR", message="Qt业务查询失败，请提供请求编号以便排查"))
        response.headers["X-Request-ID"] = request.state.request_id
        response.headers["Cache-Control"] = "no-store"
        return response

    application.include_router(router)
    return adapter
