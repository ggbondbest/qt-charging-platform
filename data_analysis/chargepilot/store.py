"""Transactional MySQL operational state for the explicitly simulated pilot.

The singleton replay-clock row is also the domain transaction lock. This keeps
allocation, FIFO dispatch, immutable offers and the global reward budget atomic
across HTTP workers. This deliberate serialization suits this small demo; no
state is kept in process memory and there is no alternate persistence backend.
"""

from contextlib import contextmanager
from datetime import datetime, timezone, timedelta
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import json
import math
import secrets
import threading
import uuid

from data_analysis.mysql_support import MySQLSettings, connect


class DomainError(Exception):
    def __init__(self, code, message, status=400):
        super().__init__(message)
        self.code, self.message, self.status = code, message, status


START = datetime(2026, 5, 5, tzinfo=timezone.utc).timestamp()
ACTIVE = {"EN_ROUTE", "QUEUED", "CALLED", "RESERVED", "CHARGING", "PENDING_PAYMENT"}
ALLOCATED = {"CALLED", "RESERVED", "CHARGING"}
DEFAULT_CONFIG = {
    "weights": {"availability": .30, "wait": .25, "travel": .20,
                "price": .10, "power": .10, "balance": .05},
    "rewards": {"first": 30, "second": 10},
    "minimumEnergyKwh": 1., "minimumChargingSeconds": 60,
    "dailyRewardBudget": 10000, "callTimeoutSeconds": 60,
    "reservationTimeoutSeconds": 900, "rewardEligibilitySeconds": 86400,
}


def _json(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def _decode(value):
    return json.loads(value) if isinstance(value, (str, bytes)) else value


def _iso(value):
    return datetime.fromtimestamp(value, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _timestamp(value):
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            raise ValueError()
        return parsed.timestamp()
    except (TypeError, ValueError, AttributeError, OverflowError):
        raise DomainError("INVALID_TIMESTAMP", "Timestamp must include a UTC offset") from None


def _number(value, name, low=0, high=1e9, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise DomainError("INVALID_CONFIG", f"{name} must be a finite number")
    if not low <= value <= high or (integer and int(value) != value):
        raise DomainError("INVALID_CONFIG", f"{name} is outside the allowed range")
    return int(value) if integer else float(value)


def _cents(energy, price):
    return int((Decimal(str(energy)) * Decimal(str(price)) * 100).quantize(Decimal("1"), rounding=ROUND_HALF_UP))


class ChargePilotStore:
    def __init__(self, settings):
        self.settings = settings if isinstance(settings, MySQLSettings) else MySQLSettings(**settings)
        self._observed = threading.local()
        # An operational write connection must never target the analytics schema.
        import os
        analytics = os.environ.get("ANALYTICS_MYSQL_DATABASE")
        if analytics and self.settings.database == analytics:
            raise ValueError("ChargePilot requires a separate operational MySQL database")

    def initialize(self, catalog, cities):
        statements = [
            "CREATE TABLE IF NOT EXISTS cp_state (id TINYINT PRIMARY KEY, payload JSON NOT NULL) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_cities (city_id VARCHAR(128) PRIMARY KEY, payload JSON NOT NULL) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_stations (station_id VARCHAR(128) PRIMARY KEY, city_id VARCHAR(128) NOT NULL, payload JSON NOT NULL, background_busy INT NOT NULL DEFAULT 0, background_release DOUBLE NULL, INDEX(city_id)) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_users (user_id CHAR(36) PRIMARY KEY, name VARCHAR(80) NOT NULL, token_hash CHAR(64) NOT NULL UNIQUE, points BIGINT NOT NULL DEFAULT 0, created_at DOUBLE NOT NULL, expires_wall DOUBLE NOT NULL) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_recommendations (recommendation_id CHAR(36) PRIMARY KEY, user_id CHAR(36) NOT NULL, payload JSON NOT NULL, expires_at DOUBLE NOT NULL, eligibility JSON NOT NULL, INDEX(user_id)) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_trips (trip_id CHAR(36) PRIMARY KEY, sequence BIGINT NOT NULL AUTO_INCREMENT UNIQUE, user_id CHAR(36) NOT NULL, station_id VARCHAR(128) NOT NULL, recommendation_id CHAR(36) NOT NULL UNIQUE, status VARCHAR(32) NOT NULL, payload JSON NOT NULL, INDEX(user_id,status), INDEX(station_id,status)) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_events (event_id BIGINT PRIMARY KEY AUTO_INCREMENT, trip_id CHAR(36) NOT NULL, type VARCHAR(40) NOT NULL, status VARCHAR(32) NOT NULL, created_at DOUBLE NOT NULL, details JSON NOT NULL, INDEX(trip_id,event_id)) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_payments (trip_id CHAR(36) PRIMARY KEY, user_id CHAR(36) NOT NULL, amount_cents BIGINT NOT NULL, paid_at DOUBLE NOT NULL) ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS cp_ledger (trip_id CHAR(36) PRIMARY KEY, user_id CHAR(36) NOT NULL, station_id VARCHAR(128) NOT NULL, points BIGINT NOT NULL, amount_cents BIGINT NOT NULL, created_at DOUBLE NOT NULL, business_date DATE NOT NULL, INDEX(user_id), INDEX(business_date)) ENGINE=InnoDB",
        ]
        connection = connect(self.settings, dict_rows=True)
        try:
            with connection.cursor() as cursor:
                for statement in statements:
                    cursor.execute(statement)
                cursor.execute("SELECT UNIX_TIMESTAMP(UTC_TIMESTAMP(6)) AS wall")
                wall = float(cursor.fetchone()["wall"])
                cursor.execute("SHOW COLUMNS FROM cp_users LIKE 'expires_wall'")
                if not cursor.fetchone():
                    cursor.execute("ALTER TABLE cp_users ADD COLUMN expires_wall DOUBLE NOT NULL DEFAULT 0")
                    cursor.execute("UPDATE cp_users SET expires_wall=%s WHERE expires_wall=0", (wall + 86400,))
                state = dict(time=START, wall=wall, speed=10., paused=False,
                             config=DEFAULT_CONFIG, baselineInitialized=False, queueSequence=0)
                cursor.execute("INSERT IGNORE INTO cp_state VALUES (1,%s)", (_json(state),))
                cursor.execute("SELECT payload FROM cp_state WHERE id=1 FOR UPDATE")
                cursor.fetchone()
                for city in cities:
                    cursor.execute("INSERT IGNORE INTO cp_cities VALUES (%s,%s)", (city["cityId"], _json(city)))
                for station in catalog:
                    _number(station["capacity"], "capacity", 1, 10000, True)
                    _number(station["powerKw"], "powerKw", .001, 10000)
                    _number(station["pricePerKwh"], "pricePerKwh", 0, 1000)
                    cursor.execute("SELECT background_busy FROM cp_stations WHERE station_id=%s", (station["stationId"],))
                    previous = cursor.fetchone()
                    if previous:
                        cursor.execute("SELECT COUNT(*) AS allocated FROM cp_trips WHERE station_id=%s AND status IN ('CALLED','RESERVED','CHARGING')", (station["stationId"],))
                        if station["capacity"] < previous["background_busy"] + cursor.fetchone()["allocated"]:
                            raise DomainError("CAPACITY_CONFLICT", "Catalog cannot displace existing allocations", 409)
                    cursor.execute("INSERT INTO cp_stations (station_id,city_id,payload) VALUES (%s,%s,%s) AS incoming ON DUPLICATE KEY UPDATE city_id=incoming.city_id,payload=incoming.payload",
                                   (station["stationId"], station["cityId"], _json(station)))
            connection.commit()
        except Exception:
            connection.rollback()
            raise
        finally:
            connection.close()

    @contextmanager
    def _transaction(self):
        connection = connect(self.settings, dict_rows=True)
        try:
            with connection.cursor() as cursor:
                cursor.execute("SELECT payload FROM cp_state WHERE id=1 FOR UPDATE")
                row = cursor.fetchone()
                if not row:
                    raise DomainError("NOT_INITIALIZED", "Operational store is not initialized", 503)
                state = _decode(row["payload"])
                cursor.execute("SELECT UNIX_TIMESTAMP(UTC_TIMESTAMP(6)) AS wall")
                wall = float(cursor.fetchone()["wall"])
                now = state["time"] + (0 if state["paused"] else max(0, wall - state["wall"]) * state["speed"])
                state["wall"] = max(wall, state["wall"])
                self._synchronize(cursor, state, now)
                progressed_state = _decode(_json(state))
                cursor.execute("SAVEPOINT cp_command")
                yield cursor, state, now
                cursor.execute("UPDATE cp_state SET payload=%s WHERE id=1", (_json(state),))
            connection.commit()
            self._observed.clock = self._clock(state)
        except DomainError:
            # Keep due clock progression, but roll back the rejected command.
            if 'progressed_state' in locals():
                with connection.cursor() as cursor:
                    cursor.execute("ROLLBACK TO SAVEPOINT cp_command")
                    cursor.execute("UPDATE cp_state SET payload=%s WHERE id=1", (_json(progressed_state),))
                connection.commit()
                self._observed.clock = self._clock(progressed_state)
            else:
                connection.rollback()
            raise
        except Exception:
            connection.rollback()
            raise
        finally:
            connection.close()

    def _user(self, cursor, user_id):
        cursor.execute("SELECT user_id,name,points FROM cp_users WHERE user_id=%s AND expires_wall>UNIX_TIMESTAMP(UTC_TIMESTAMP(6))", (user_id,))
        row = cursor.fetchone()
        if not row:
            raise DomainError("UNAUTHORIZED", "Demo session is invalid", 401)
        return {"userId": row["user_id"], "name": row["name"], "points": int(row["points"])}

    def _station_rows(self, cursor):
        cursor.execute("SELECT * FROM cp_stations ORDER BY station_id")
        return {row["station_id"]: dict(_decode(row["payload"]), backgroundBusy=row["background_busy"],
                                       backgroundRelease=row["background_release"]) for row in cursor.fetchall()}

    def _trip_rows(self, cursor):
        cursor.execute("SELECT * FROM cp_trips WHERE status IN ('EN_ROUTE','QUEUED','CALLED','RESERVED','CHARGING','PENDING_PAYMENT') ORDER BY sequence")
        return {row["trip_id"]: _decode(row["payload"]) for row in cursor.fetchall()}

    def _persist_trip(self, cursor, trip):
        cursor.execute("UPDATE cp_trips SET status=%s,payload=%s WHERE trip_id=%s", (trip["status"], _json(trip), trip["tripId"]))

    def _event(self, cursor, trip, event, at, details=None):
        cursor.execute("INSERT INTO cp_events (trip_id,type,status,created_at,details) VALUES (%s,%s,%s,%s,%s)",
                       (trip["tripId"], event, trip["status"], at, _json(details or {})))

    def _transition(self, cursor, trip, status, at, event=None, details=None):
        trip.update(status=status, updatedAt=_iso(at))
        if status != "CALLED":
            trip["callExpiresAt"] = None
        if status != "RESERVED":
            trip["reservationExpiresAt"] = None
        self._persist_trip(cursor, trip)
        self._event(cursor, trip, event or status, at, details)

    def _dispatch(self, cursor, state, stations, trips, at):
        for station_id, station in stations.items():
            allocated = sum(t["stationId"] == station_id and t["status"] in ALLOCATED for t in trips.values())
            free = station["capacity"] - station["backgroundBusy"] - allocated
            waiting = sorted((t for t in trips.values() if t["stationId"] == station_id and t["status"] == "QUEUED"),
                             key=lambda t: t["queueSequence"])
            for trip in waiting[:max(0, free)]:
                trip["callExpiresAt"] = _iso(at + state["config"]["callTimeoutSeconds"])
                self._transition(cursor, trip, "CALLED", at)

    def _charge(self, trip, at):
        seconds = max(0., at - _timestamp(trip["chargingStartedAt"]))
        energy = min(trip["targetEnergyKwh"], seconds * trip["powerKw"] / 3600.)
        trip["chargingSeconds"] = round(min(seconds, trip["targetEnergyKwh"] / trip["powerKw"] * 3600), 3)
        trip["energyKwh"] = round(energy, 8)
        trip["amountCents"] = _cents(energy, trip["pricePerKwh"])
        trip["amount"] = trip["amountCents"] / 100.

    def _synchronize(self, cursor, state, now):
        stations, trips = self._station_rows(cursor), self._trip_rows(cursor)
        self._dispatch(cursor, state, stations, trips, state["time"])
        while True:
            events = []
            for sid, station in stations.items():
                if station["backgroundBusy"] and station["backgroundRelease"] is not None:
                    events.append((station["backgroundRelease"], "background", sid))
            for tid, trip in trips.items():
                if trip["status"] == "CALLED":
                    events.append((_timestamp(trip["callExpiresAt"]), "expire", tid))
                elif trip["status"] == "RESERVED":
                    events.append((_timestamp(trip["reservationExpiresAt"]), "expire", tid))
                elif trip["status"] in {"EN_ROUTE", "QUEUED"}:
                    events.append((_timestamp(trip["rewardExpiresAt"]), "expire", tid))
                elif trip["status"] == "CHARGING":
                    events.append((_timestamp(trip["chargingStartedAt"]) + trip["targetEnergyKwh"] / trip["powerKw"] * 3600, "finish", tid))
            due = [event for event in events if event[0] <= now]
            if not due:
                break
            at = min(event[0] for event in due)
            for _, kind, entity in sorted(event for event in due if event[0] == at):
                if kind == "background":
                    stations[entity].update(backgroundBusy=0, backgroundRelease=None)
                    cursor.execute("UPDATE cp_stations SET background_busy=0,background_release=NULL WHERE station_id=%s", (entity,))
                elif kind == "expire":
                    trip = trips[entity]
                    previous = trip["status"]
                    trip["stopReason"] = previous + "_TIMEOUT"
                    trip["rewardStatus"] = "EXPIRED"
                    self._transition(cursor, trip, "EXPIRED", at, details={"previousStatus": previous})
                else:
                    trip = trips[entity]
                    self._charge(trip, at)
                    trip.update(stopReason="TARGET_REACHED", chargingStoppedAt=_iso(at))
                    self._transition(cursor, trip, "PENDING_PAYMENT", at, "CHARGING_STOPPED")
            self._dispatch(cursor, state, stations, trips, at)
        for trip in trips.values():
            if trip["status"] == "CHARGING":
                self._charge(trip, now)
                self._persist_trip(cursor, trip)
        state["time"] = now

    def _clock(self, state):
        return {"time": _iso(state["time"]), "speed": state["speed"], "paused": state["paused"]}

    def clock(self):
        with self._transaction() as (_, state, _):
            return self._clock(state)

    def observed_clock(self):
        """Last committed clock on this caller thread; never advances the DB."""
        value = getattr(self._observed, "clock", None)
        return dict(value) if value is not None else None

    def create_session(self, name):
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 60 or any(ord(c) < 32 for c in name):
            raise DomainError("INVALID_NAME", "Name must contain 1–60 visible characters")
        token, user_id = secrets.token_urlsafe(32), str(uuid.uuid4())
        with self._transaction() as (cursor, state, now):
            expires = state["wall"] + 86400
            cursor.execute("INSERT INTO cp_users (user_id,name,token_hash,created_at,expires_wall) VALUES (%s,%s,%s,%s,%s)",
                           (user_id, name.strip(), hashlib.sha256(token.encode()).hexdigest(), now, expires))
            return {"token": token, "user": self._user(cursor, user_id), "sessionExpiresAt": _iso(expires)}

    def authenticate(self, token):
        if not isinstance(token, str) or not 20 <= len(token) <= 256:
            raise DomainError("UNAUTHORIZED", "Bearer demo session token is required", 401)
        with self._transaction() as (cursor, _, _):
            cursor.execute("SELECT user_id FROM cp_users WHERE token_hash=%s AND expires_wall>UNIX_TIMESTAMP(UTC_TIMESTAMP(6))", (hashlib.sha256(token.encode()).hexdigest(),))
            row = cursor.fetchone()
            if not row:
                raise DomainError("UNAUTHORIZED", "Demo session token is invalid", 401)
            return row["user_id"]

    def _stations(self, cursor, city_id=None):
        stations, trips = self._station_rows(cursor), self._trip_rows(cursor)
        result = []
        for sid, station in stations.items():
            if city_id is not None and station["cityId"] != city_id:
                continue
            statuses = [trip["status"] for trip in trips.values() if trip["stationId"] == sid]
            committed = statuses.count("CALLED") + statuses.count("RESERVED")
            charging = statuses.count("CHARGING")
            station.update(currentFree=max(0, station["capacity"] - station["backgroundBusy"] - committed - charging),
                           queued=statuses.count("QUEUED"), committed=committed, charging=charging,
                           enRoute=statuses.count("EN_ROUTE"))
            release = station.pop("backgroundRelease")
            station["backgroundReleaseAt"] = _iso(release) if release is not None else None
            result.append(station)
        return result

    def stations(self, city_id=None):
        with self._transaction() as (cursor, _, _):
            return self._stations(cursor, city_id)

    def _trip(self, cursor, user_id, trip_id):
        cursor.execute("SELECT payload FROM cp_trips WHERE trip_id=%s AND user_id=%s", (trip_id, user_id))
        row = cursor.fetchone()
        if not row:
            raise DomainError("TRIP_NOT_FOUND", "Trip does not exist", 404)
        return _decode(row["payload"])

    def _public_trip(self, cursor, trip):
        result = {key: value for key, value in trip.items() if key not in {"eligibility", "queueSequence", "amountCents", "userId"}}
        if trip["status"] == "QUEUED":
            waiting = sorted((t for t in self._trip_rows(cursor).values() if t["stationId"] == trip["stationId"] and t["status"] == "QUEUED"), key=lambda t: t["queueSequence"])
            result["queuePosition"] = next(i + 1 for i, t in enumerate(waiting) if t["tripId"] == trip["tripId"])
            result["peopleAhead"] = result["queuePosition"] - 1
        else:
            result.update(queuePosition=None, peopleAhead=0)
        cursor.execute("SELECT * FROM cp_events WHERE trip_id=%s ORDER BY event_id", (trip["tripId"],))
        result["events"] = [{"eventId": row["event_id"], "type": row["type"], "status": row["status"],
                             "createdAt": _iso(row["created_at"]), "details": _decode(row["details"])} for row in cursor.fetchall()]
        return result

    def me(self, user_id):
        with self._transaction() as (cursor, _, _):
            user = self._user(cursor, user_id)
            cursor.execute("SELECT payload FROM cp_trips WHERE user_id=%s ORDER BY sequence DESC", (user_id,))
            trips = [self._public_trip(cursor, _decode(row["payload"])) for row in cursor.fetchall()]
            cursor.execute("SELECT * FROM cp_ledger WHERE user_id=%s ORDER BY created_at DESC,trip_id", (user_id,))
            ledger = [{"tripId": row["trip_id"], "stationId": row["station_id"], "points": int(row["points"]),
                       "amount": row["amount_cents"] / 100, "createdAt": _iso(row["created_at"]),
                       "businessDate": row["business_date"].isoformat()} for row in cursor.fetchall()]
            return {"user": user, "trips": trips, "ledger": ledger}

    def save_recommendation(self, user_id, payload):
        payload = _decode(_json(payload))
        with self._transaction() as (cursor, state, now):
            self._user(cursor, user_id)
            recommendation_id = payload.get("recommendationId") or str(uuid.uuid4())
            try:
                uuid.UUID(recommendation_id)
            except (ValueError, TypeError, AttributeError):
                raise DomainError("INVALID_RECOMMENDATION", "Recommendation ID must be a UUID") from None
            cursor.execute("SELECT * FROM cp_recommendations WHERE recommendation_id=%s", (recommendation_id,))
            existing = cursor.fetchone()
            if existing:
                if existing["user_id"] != user_id:
                    raise DomainError("RECOMMENDATION_NOT_FOUND", "Recommendation does not exist", 404)
                return _decode(existing["payload"])
            candidates = payload.get("candidates")
            if not isinstance(candidates, list) or not candidates:
                raise DomainError("INVALID_RECOMMENDATION", "Recommendation requires eligible candidates")
            station_ids, seen, seen_ranks = self._station_rows(cursor), set(), set()
            for candidate in candidates:
                sid = candidate.get("stationId")
                if sid not in station_ids or sid in seen:
                    raise DomainError("INVALID_RECOMMENDATION", "Candidate station is invalid or duplicated")
                seen.add(sid)
                rank = _number(candidate.get("rank"), "rank", 1, 10000, True)
                if rank in seen_ranks:
                    raise DomainError("INVALID_RECOMMENDATION", "Candidate ranks must be unique")
                seen_ranks.add(rank)
                _number(candidate.get("etaMinutes", 0), "etaMinutes", 0, 1440)
                multiplier = _number(candidate.get("rewardMultiplier", 1), "rewardMultiplier", .5, 2)
                if multiplier not in {.5, 1, 2}:
                    raise DomainError("INVALID_RECOMMENDATION", "Reward multiplier must be 0.5, 1 or 2")
                candidate.update(rank=rank, rewardMultiplier=multiplier,
                    pricePerKwh=_number(candidate.get("pricePerKwh", station_ids[sid]["pricePerKwh"]), "pricePerKwh", 0, 1000),
                    rewardPoints=round(state["config"]["rewards"].get({1: "first", 2: "second"}.get(rank), 0) * multiplier))
            expires = _timestamp(payload["expiresAt"]) if payload.get("expiresAt") else now + 900
            if expires <= now or expires > now + 86400:
                raise DomainError("INVALID_RECOMMENDATION", "Recommendation expiry must be within the next 24 hours")
            payload.update(recommendationId=recommendation_id, createdAt=_iso(now), expiresAt=_iso(expires),
                           energyKwh=_number(payload.get("energyKwh", 20), "energyKwh", .01, 500))
            eligibility = {key: state["config"][key] for key in ("minimumEnergyKwh", "minimumChargingSeconds", "rewardEligibilitySeconds")}
            cursor.execute("INSERT INTO cp_recommendations VALUES (%s,%s,%s,%s,%s)",
                           (recommendation_id, user_id, _json(payload), expires, _json(eligibility)))
            return payload

    def select(self, user_id, recommendation_id, station_id):
        with self._transaction() as (cursor, _, now):
            self._user(cursor, user_id)
            cursor.execute("SELECT * FROM cp_recommendations WHERE recommendation_id=%s AND user_id=%s", (recommendation_id, user_id))
            row = cursor.fetchone()
            if not row:
                raise DomainError("RECOMMENDATION_NOT_FOUND", "Recommendation does not exist", 404)
            cursor.execute("SELECT payload FROM cp_trips WHERE recommendation_id=%s", (recommendation_id,))
            existing = cursor.fetchone()
            if existing:
                trip = _decode(existing["payload"])
                if trip["stationId"] != station_id:
                    raise DomainError("OFFER_ALREADY_USED", "This recommendation already has a selected trip", 409)
                return self._public_trip(cursor, trip)
            if row["expires_at"] <= now:
                raise DomainError("RECOMMENDATION_EXPIRED", "Refresh the recommendation before selecting", 409)
            offer = _decode(row["payload"])
            candidate = next((item for item in offer["candidates"] if item["stationId"] == station_id and item.get("eligible", True)), None)
            if not candidate:
                raise DomainError("INELIGIBLE_STATION", "Station is not eligible in this recommendation", 409)
            cursor.execute("SELECT trip_id FROM cp_trips WHERE user_id=%s AND status IN ('EN_ROUTE','QUEUED','CALLED','RESERVED','CHARGING','PENDING_PAYMENT') LIMIT 1", (user_id,))
            if cursor.fetchone():
                raise DomainError("ACTIVE_TRIP", "Finish or cancel the existing trip first", 409)
            station = self._station_rows(cursor)[station_id]
            eligibility = _decode(row["eligibility"])
            trip = dict(tripId=str(uuid.uuid4()), userId=user_id, recommendationId=recommendation_id,
                        origin=offer.get("origin"),
                        stationId=station_id, stationName=station["stationName"], status="EN_ROUTE",
                        createdAt=_iso(now), updatedAt=_iso(now), arrivalEligibleAt=_iso(now + candidate.get("etaMinutes", 0) * 60),
                        recommendedAt=offer["createdAt"], rank=candidate["rank"], rewardMultiplier=candidate.get("rewardMultiplier", 1),
                        arrivedAt=None, actualFreeAtArrival=None, actualWaitMinutes=None,
                        rewardExpiresAt=_iso(now + eligibility["rewardEligibilitySeconds"]), eligibility=eligibility,
                        targetEnergyKwh=offer["energyKwh"], energyKwh=0., amount=0., amountCents=0,
                        chargingSeconds=0., rewardPoints=candidate["rewardPoints"], awardedPoints=0,
                        pricePerKwh=candidate.get("pricePerKwh", station["pricePerKwh"]), powerKw=station["powerKw"], stopReason=None,
                        callExpiresAt=None, reservationExpiresAt=None, queueSequence=None,
                        rewardStatus="NOT_PAID", paidAt=None)
            cursor.execute("INSERT INTO cp_trips (trip_id,user_id,station_id,recommendation_id,status,payload) VALUES (%s,%s,%s,%s,%s,%s)",
                           (trip["tripId"], user_id, station_id, recommendation_id, "EN_ROUTE", _json(trip)))
            self._event(cursor, trip, "SELECTED", now, {"rank": candidate["rank"], "rewardPoints": candidate["rewardPoints"]})
            return self._public_trip(cursor, trip)

    def act(self, user_id, trip_id, action):
        if action not in {"arrive", "confirm", "start", "stop", "pay", "cancel"}:
            raise DomainError("INVALID_ACTION", "Unknown trip action")
        with self._transaction() as (cursor, state, now):
            self._user(cursor, user_id)
            trip = self._trip(cursor, user_id, trip_id)
            status = trip["status"]
            idempotent = {"arrive": {"QUEUED", "CALLED", "RESERVED"}, "confirm": {"RESERVED"},
                          "start": {"CHARGING"}, "stop": {"PENDING_PAYMENT", "COMPLETED"},
                          "pay": {"COMPLETED"}, "cancel": {"CANCELLED"}}
            if status in idempotent[action]:
                return self._public_trip(cursor, trip)
            allowed = {"arrive": {"EN_ROUTE"}, "confirm": {"CALLED"}, "start": {"RESERVED"},
                       "stop": {"CHARGING"}, "pay": {"PENDING_PAYMENT"},
                       "cancel": {"EN_ROUTE", "QUEUED", "CALLED", "RESERVED"}}
            if status not in allowed[action]:
                raise DomainError("INVALID_TRANSITION", f"Cannot {action} a {status} trip", 409)
            if action == "arrive":
                if now < _timestamp(trip["arrivalEligibleAt"]):
                    raise DomainError("ARRIVAL_TOO_EARLY", "The replay clock has not reached the estimated arrival", 409)
                station = next(item for item in self._stations(cursor) if item["stationId"] == trip["stationId"])
                trip.update(arrivedAt=_iso(now), actualFreeAtArrival=station["currentFree"])
                if station["currentFree"] > 0 and station["queued"] == 0:
                    trip["reservationExpiresAt"] = _iso(now + state["config"]["reservationTimeoutSeconds"])
                    self._transition(cursor, trip, "RESERVED", now, "ARRIVED_RESERVED")
                else:
                    state["queueSequence"] += 1
                    trip["queueSequence"] = state["queueSequence"]
                    self._transition(cursor, trip, "QUEUED", now, "ARRIVED_QUEUED")
            elif action == "confirm":
                trip["reservationExpiresAt"] = _iso(now + state["config"]["reservationTimeoutSeconds"])
                self._transition(cursor, trip, "RESERVED", now, "CALL_CONFIRMED")
            elif action == "start":
                trip["chargingStartedAt"] = _iso(now)
                trip["actualWaitMinutes"] = round((now - _timestamp(trip["arrivedAt"])) / 60, 6)
                self._transition(cursor, trip, "CHARGING", now, "CHARGING_STARTED")
            elif action == "stop":
                self._charge(trip, now)
                trip.update(stopReason="MANUAL", chargingStoppedAt=_iso(now))
                self._transition(cursor, trip, "PENDING_PAYMENT", now, "CHARGING_STOPPED")
            elif action == "cancel":
                trip.update(stopReason="CANCELLED", rewardStatus="CANCELLED")
                self._transition(cursor, trip, "CANCELLED", now)
            elif action == "pay":
                self._pay(cursor, state, trip, now)
            self._dispatch(cursor, state, self._station_rows(cursor), self._trip_rows(cursor), now)
            return self._public_trip(cursor, self._trip(cursor, user_id, trip_id))

    def _pay(self, cursor, state, trip, now):
        # No amount, charging duration or award enters from a client request.
        cursor.execute("INSERT INTO cp_payments VALUES (%s,%s,%s,%s)",
                       (trip["tripId"], trip["userId"], trip["amountCents"], now))
        eligible = trip["eligibility"]
        reason = "AWARDED"
        if trip["amountCents"] <= 0:
            reason = "NO_POSITIVE_PAYMENT"
        elif trip["chargingSeconds"] < eligible["minimumChargingSeconds"]:
            reason = "BELOW_MINIMUM_DURATION"
        elif trip["energyKwh"] < eligible["minimumEnergyKwh"]:
            reason = "BELOW_MINIMUM_ENERGY"
        elif now >= _timestamp(trip["rewardExpiresAt"]):
            reason = "REWARD_EXPIRED"
        elif trip["rewardPoints"] <= 0:
            reason = "NO_OFFER_REWARD"
        business_date = datetime.fromtimestamp(now, timezone(timedelta(hours=8))).date()
        cursor.execute("SELECT COALESCE(SUM(points),0) AS used FROM cp_ledger WHERE business_date=%s", (business_date,))
        remaining = max(0, state["config"]["dailyRewardBudget"] - int(cursor.fetchone()["used"]))
        award = min(trip["rewardPoints"], remaining) if reason == "AWARDED" else 0
        if reason == "AWARDED" and award < trip["rewardPoints"]:
            reason = "DAILY_BUDGET_LIMIT" if award else "DAILY_BUDGET_EXHAUSTED"
        if award:
            cursor.execute("INSERT INTO cp_ledger VALUES (%s,%s,%s,%s,%s,%s,%s)",
                           (trip["tripId"], trip["userId"], trip["stationId"], award, trip["amountCents"], now, business_date))
            cursor.execute("UPDATE cp_users SET points=points+%s WHERE user_id=%s", (award, trip["userId"]))
        trip.update(awardedPoints=award, rewardStatus=reason, paidAt=_iso(now))
        self._transition(cursor, trip, "COMPLETED", now, "PAYMENT_COMPLETED", {"amount": trip["amount"], "awardedPoints": award, "rewardStatus": reason})

    def set_baseline(self, snapshot):
        with self._transaction() as (cursor, state, now):
            if state["baselineInitialized"]:
                return {"initialized": False, "stations": self._stations(cursor)}
            stations = {item["stationId"]: item for item in self._stations(cursor)}
            prepared = []
            for item in snapshot:
                station = stations.get(item.get("stationId"))
                if not station:
                    raise DomainError("STATION_NOT_FOUND", "Baseline station does not exist", 404)
                free = _number(item["currentFree"], "currentFree", 0, station["capacity"], True)
                delay = _number(item.get("releaseAfterSeconds", 300), "releaseAfterSeconds", 1, 604800)
                busy = min(station["capacity"] - free, station["currentFree"])
                prepared.append((busy, now + delay if busy else None, item["stationId"]))
            for row in prepared:
                cursor.execute("UPDATE cp_stations SET background_busy=%s,background_release=%s WHERE station_id=%s", row)
            state["baselineInitialized"] = True
            return {"initialized": True, "stations": self._stations(cursor)}

    def set_background(self, station_id, busy_count, release_after_seconds):
        busy_count = _number(busy_count, "busyCount", 0, 10000, True)
        delay = _number(release_after_seconds, "releaseAfterSeconds", 1 if busy_count else 0, 604800)
        with self._transaction() as (cursor, state, now):
            station = next((item for item in self._stations(cursor) if item["stationId"] == station_id), None)
            if not station:
                raise DomainError("STATION_NOT_FOUND", "Station does not exist", 404)
            if busy_count > station["capacity"] - station["committed"] - station["charging"]:
                raise DomainError("CAPACITY_CONFLICT", "Background cannot displace user allocations", 409)
            cursor.execute("UPDATE cp_stations SET background_busy=%s,background_release=%s WHERE station_id=%s", (busy_count, now + delay if busy_count else None, station_id))
            self._dispatch(cursor, state, self._station_rows(cursor), self._trip_rows(cursor), now)
            return next(item for item in self._stations(cursor) if item["stationId"] == station_id)

    def configure(self, patch):
        if not isinstance(patch, dict) or not patch or set(patch) - set(DEFAULT_CONFIG):
            raise DomainError("INVALID_CONFIG", "Configuration contains unsupported fields")
        with self._transaction() as (_, state, _):
            config = _decode(_json(state["config"]))
            for key, value in patch.items():
                if key == "weights":
                    if not isinstance(value, dict) or set(value) != set(DEFAULT_CONFIG["weights"]):
                        raise DomainError("INVALID_CONFIG", "All six scoring weights are required")
                    weights = {name: _number(number, name, 0, 1) for name, number in value.items()}
                    if not math.isclose(sum(weights.values()), 1, abs_tol=1e-8):
                        raise DomainError("INVALID_CONFIG", "Scoring weights must sum to one")
                    config[key] = weights
                elif key == "rewards":
                    if not isinstance(value, dict) or set(value) != {"first", "second"}:
                        raise DomainError("INVALID_CONFIG", "Both first and second reward values are required")
                    config[key] = {name: _number(number, name, 0, 100000, True) for name, number in value.items()}
                elif key == "minimumEnergyKwh":
                    config[key] = _number(value, key, .001, 500)
                else:
                    low, high = (0, 10000000) if key == "dailyRewardBudget" else (1, 604800)
                    config[key] = _number(value, key, low, high, True)
            state["config"] = config
            return config

    def control_clock(self, payload):
        if not isinstance(payload, dict):
            raise DomainError("INVALID_CLOCK", "Clock command must be an object")
        action = payload.get("action")
        if action == "advance":
            if set(payload) != {"action", "seconds"}:
                raise DomainError("INVALID_CLOCK", "Advance requires seconds only")
            seconds = _number(payload.get("seconds"), "seconds", 0, 604800)
        elif action == "configure":
            if set(payload) - {"action", "speed", "paused"}:
                raise DomainError("INVALID_CLOCK", "Unsupported clock field")
            if "paused" in payload and not isinstance(payload["paused"], bool):
                raise DomainError("INVALID_CLOCK", "paused must be boolean")
            if "speed" in payload:
                _number(payload["speed"], "speed", 0, 3600)
        else:
            raise DomainError("INVALID_CLOCK", "Unknown clock command")
        with self._transaction() as (cursor, state, now):
            if action == "advance":
                self._synchronize(cursor, state, now + seconds)
            else:
                state.update({key: payload[key] for key in ("speed", "paused") if key in payload})
            return self._clock(state)

    def admin(self):
        with self._transaction() as (cursor, state, _):
            counts = {}
            for label, table in (("users", "cp_users"), ("recommendations", "cp_recommendations"), ("trips", "cp_trips"), ("payments", "cp_payments")):
                cursor.execute("SELECT COUNT(*) AS n FROM " + table)
                counts[label] = cursor.fetchone()["n"]
            cursor.execute("SELECT status,COUNT(*) AS n FROM cp_trips GROUP BY status")
            counts["statuses"] = {row["status"]: row["n"] for row in cursor.fetchall()}
            return {"clock": self._clock(state), "config": state["config"], "stations": self._stations(cursor),
                    "counts": counts, "baselineInitialized": state["baselineInitialized"]}

    def feedback(self):
        """Admin HTTP layer authorizes export; no identity names or tokens appear.

        Unserved trips retain null waits. These are operational simulation records,
        not new real-city observations and not automatically added to model fits.
        """
        with self._transaction() as (cursor, _, _):
            cursor.execute("SELECT payload FROM cp_trips ORDER BY sequence")
            result = []
            for row in cursor.fetchall():
                trip = _decode(row["payload"])
                result.append({
                    "recommendationId": trip["recommendationId"], "tripId": trip["tripId"],
                    "userId": trip["userId"], "stationId": trip["stationId"], "rank": trip.get("rank"),
                    "recommendedAt": trip.get("recommendedAt"), "selectedAt": trip["createdAt"],
                    "arrivedAt": trip.get("arrivedAt"), "actualFreeAtArrival": trip.get("actualFreeAtArrival"),
                    "startedAt": trip.get("chargingStartedAt"), "actualWaitMinutes": trip.get("actualWaitMinutes"),
                    "energyKwh": trip["energyKwh"], "amount": trip["amount"], "status": trip["status"],
                    "rewardPoints": trip["rewardPoints"], "awardedPoints": trip["awardedPoints"],
                    "source": "SIMULATED_OPERATIONAL_FEEDBACK",
                })
            return result
