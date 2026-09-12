"""Event-based synthetic operations simulator. No real records are copied.

Run from repository root:
python -m data_analysis.charging_data.generator --config data_analysis/config/full.json
All random streams and gzip headers are reproducible. Only the standard library
is needed. The output contains raw data and clearly labeled reference aggregates,
not pretense Spark output or pre-trained machine learning predictions.
"""

import argparse
from collections import defaultdict, deque
from datetime import date, datetime, timedelta, timezone
import json
import math
from pathlib import Path
import random
import re

from . import __version__
from .io import DatasetWriter, write_json
from .schema import SCHEMA_VERSION, TABLES, SUMMARY_TABLES

BUSINESS_TZ = timezone(timedelta(hours=8))
CITIES = [
    ("DL", "大连市", 38.9140, 121.6147, -2),
    ("SY", "沈阳市", 41.8057, 123.4315, -6),
    ("BJ", "北京市", 39.9042, 116.4074, -1),
    ("SH", "上海市", 31.2304, 121.4737, 8),
    ("SZ", "深圳市", 22.5431, 114.0579, 17),
]
SITES = [("OFFICE", "科创园", .13), ("SHOPPING", "商业中心", .14),
         ("RESIDENTIAL", "社区", .10), ("TRANSIT", "交通枢纽", .17),
         ("CAMPUS", "大学城", .085)]


def utc_text(value):
    return value.astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def cents(wh, cents_per_kwh):
    return (wh * cents_per_kwh + 500) // 1000


def validate_config(config):
    required = {"dataset_id", "seed", "start_date", "days", "users_per_city", "interval_minutes", "dirty_rate"}
    if set(config) != required:
        raise ValueError(f"Config keys must be exactly {sorted(required)}")
    if not isinstance(config["dataset_id"], str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", config["dataset_id"]):
        raise ValueError("dataset_id must be a safe ASCII identifier")
    for key, low, high in [("days", 1, 366), ("users_per_city", 1, 20000), ("seed", 0, 2**32-1)]:
        if type(config[key]) is not int or not low <= config[key] <= high:
            raise ValueError(f"Invalid {key}")
    if config["interval_minutes"] != 5 or isinstance(config["interval_minutes"], bool):
        raise ValueError("Version 1 supports exactly five-minute intervals")
    if type(config["dirty_rate"]) not in (int, float) or not 0 <= config["dirty_rate"] <= .1:
        raise ValueError("dirty_rate must be in [0, 0.1]")
    start = date.fromisoformat(config["start_date"])
    if not 2000 <= start.year <= 2100:
        raise ValueError("start_date must be between 2000 and 2100")


class Simulator:
    def __init__(self, config, destination):
        self.config = config
        self.root = destination
        self.rng = random.Random(config["seed"])
        self.noise_rng = random.Random(config["seed"] + 1097)
        self.writer = DatasetWriter(destination, TABLES)
        self.summaries = DatasetWriter(destination, SUMMARY_TABLES, "reference_aggregates")
        self.start = datetime.combine(date.fromisoformat(config["start_date"]), datetime.min.time(), BUSINESS_TZ)
        self.ticks = config["days"] * 288
        self.times = [self.start + timedelta(minutes=i * 5) for i in range(self.ticks + 1)]
        self.stamps = [utc_text(t) for t in self.times]
        self.counters = defaultdict(int)
        self.stations = []
        self.chargers = []
        self.users = defaultdict(list)
        self.locks = {}
        self.tariffs = {}
        self.weather = {}
        self.events = {}
        self.campaigns = {}
        self.campaign_spend = defaultdict(int)
        self.daily = {}
        self.canonical_sessions = 0
        self.corruptions = defaultdict(int)

    def ident(self, kind):
        self.counters[kind] += 1
        return f"{kind}-{self.counters[kind]:08d}"

    def emit(self, table, row, tick=None):
        partition = "static" if tick is None else self.times[min(tick, self.ticks)].strftime("%Y-%m")
        self.writer.write(table, row, partition)

    def daily_row(self, station, tick):
        day = self.times[min(tick, self.ticks - 1)].date().isoformat()
        key = station["station_id"], day
        if key not in self.daily:
            row = dict.fromkeys(SUMMARY_TABLES["station_daily"], 0)
            row.update(station_id=key[0], city_id=station["city_id"], business_date=day)
            self.daily[key] = row
        return self.daily[key]

    def dimensions(self):
        rng = self.rng
        for city_index, (cid, cname, lat, lon, winter) in enumerate(CITIES):
            self.emit("cities", dict(city_id=cid, city_name=cname, latitude=lat, longitude=lon, timezone="Asia/Shanghai"))
            for hour in range(24):
                period = "VALLEY" if hour < 7 else "PEAK" if hour in (10, 11, 18, 19, 20, 21) else "NORMAL"
                rate = {"VALLEY": 42, "NORMAL": 70, "PEAK": 105}[period] + city_index * 3
                row = dict(tariff_id=f"T-{cid}-{hour:02}", city_id=cid, hour=hour,
                           energy_price_cents_per_kwh=rate + 12, service_price_cents_per_kwh=30 + city_index * 2,
                           grid_price_cents_per_kwh=rate, period=period)
                self.tariffs[cid, hour] = row
                self.emit("tariffs", row)
            for user_index in range(self.config["users_per_city"]):
                uid = f"U-{cid}-{user_index+1:06}"
                segment = rng.choices(["COMMUTER", "RIDE_HAILING", "FAMILY", "FLEET"], [45, 20, 25, 10])[0]
                self.emit("users", dict(user_id=uid, home_city_id=cid,
                    registered_at=utc_text(self.start - timedelta(days=rng.randint(1, 730))),
                    segment=segment, acquisition_channel=rng.choice(["SEARCH", "REFERRAL", "ADS", "ORGANIC"]),
                    membership=rng.choices(["STANDARD", "PLUS"], [75, 25])[0]))
                vehicle = dict(vehicle_id=f"V-{cid}-{user_index+1:06}", user_id=uid,
                    battery_capacity_kwh=rng.choice([40, 50, 60, 75, 90]),
                    max_charge_kw=rng.choice([50, 80, 120, 150]), vehicle_class=segment)
                self.emit("vehicles", vehicle)
                self.users[cid].append(vehicle)
                self.locks[uid] = 0
            for site_index, (kind, label, intensity) in enumerate(SITES):
                sid = f"ST-{cid}-{site_index+1:02}"
                station = dict(station_id=sid, city_id=cid, station_name=f"{cname}{label}模拟充电站",
                    site_type=kind, latitude=round(lat+(site_index-2)*.025, 6),
                    longitude=round(lon+(site_index%3-1)*.035, 6),
                    opened_at=utc_text(self.start-timedelta(days=730)), transformer_kw=360,
                    rent_daily_cents=(12000+city_index*2500+site_index*1000))
                self.emit("stations", station)
                station.update(intensity=intensity*(1+city_index*.08), chargers=[], waiting=deque())
                self.stations.append(station)
                for charger_index in range(3):
                    ac = charger_index == 2 or (kind == "RESIDENTIAL" and charger_index == 1)
                    charger = dict(charger_id=f"CH-{cid}-{site_index+1:02}-{charger_index+1}", station_id=sid,
                        connector_type="AC" if ac else "DC", rated_power_kw=7 if ac else [60, 120][charger_index],
                        commissioned_at=station["opened_at"], manufacturer=f"SIM_VENDOR_{charger_index+1}",
                        model="SIM-AC7" if ac else f"SIM-DC{[60,120][charger_index]}")
                    self.emit("chargers", charger)
                    charger.update(station=station, session=None, hold=None, blocked_until=0, blocked_state="",
                                   meter_wh=0)
                    station["chargers"].append(charger)
                    self.chargers.append(charger)
            for day in range(self.config["days"]):
                dt = self.times[day*288]
                event = "LOCAL_EXPO" if day % 45 in (20, 21, 22) else "NONE"
                multiplier = 1.6 if event != "NONE" else 1.0
                self.events[cid, day] = multiplier
                self.emit("calendar", dict(city_id=cid, business_date=dt.date().isoformat(),
                    is_weekend=int(dt.weekday() >= 5), scenario_event=event, demand_multiplier=multiplier))
                base_temp = winter + 13 * (1-math.cos(day/180*math.pi)) + rng.uniform(-3, 3)
                wet = rng.random() < .22
                for hour in range(24):
                    temp = round(base_temp + 4 * math.sin((hour-8)*math.pi/12), 1)
                    w = dict(city_id=cid, recorded_at=self.stamps[day*288+hour*12], temperature_c=temp,
                             humidity_pct=rng.randint(65, 95) if wet else rng.randint(30, 65),
                             weather="SNOW" if wet and temp < 0 else "RAIN" if wet else "CLEAR",
                             rainfall_mm=round(rng.uniform(.1, 2.5), 1) if wet else 0)
                    self.weather[cid, day*24+hour] = w
                    self.emit("weather_hourly", w, day*288)
            for block in range((self.config["days"]+29)//30):
                begin = block*30*288
                end = min((block+1)*30*288, self.ticks)
                campaign = dict(campaign_id=f"CP-{cid}-{block+1}", city_id=cid,
                    starts_at=self.stamps[begin], ends_at=self.stamps[end], channel="APP_COUPON",
                    discount_cents=300+city_index*50, budget_cents=150000)
                self.campaigns[cid, block] = campaign
                self.emit("campaigns", campaign)
        for station in self.stations:
            for day in range(self.config["days"]):
                cost = dict(station_id=station["station_id"], business_date=self.times[day*288].date().isoformat(),
                    rent_cents=station["rent_daily_cents"], labor_cents=8000,
                    network_cents=600)
                self.emit("operating_costs", cost, day*288)
                self.daily_row(station, day*288)["operating_cost_cents"] = sum(cost[k] for k in ("rent_cents", "labor_cents", "network_cents"))

    def finalize_request(self, request, outcome, tick, charger=None, session_id="", reason=""):
        attempt = request["attempt"]
        attempt.update(outcome=outcome, session_id=session_id,
                       charger_id=charger["charger_id"] if charger else "", failure_reason=reason)
        self.emit("charging_attempts", attempt, request["tick"])
        if not session_id:
            self.locks[attempt["user_id"]] = tick + 3

    def record_session(self, row, tick):
        canonical = dict(row)
        self.canonical_sessions += 1
        dirty = self.noise_rng.random() < self.config["dirty_rate"]
        mode = self.noise_rng.choice(["DUPLICATE", "NEGATIVE_FEE", "UNKNOWN_STATION", "MISSING_ID", "STATUS_FORMAT"])
        if dirty and mode == "STATUS_FORMAT":
            row = dict(row, status=f" {row['status'].lower()} ")
        self.emit("charging_sessions", row, tick)
        if not dirty:
            return
        cid = self.ident("COR")
        if mode != "STATUS_FORMAT":
            extra = dict(canonical)
            if mode != "DUPLICATE":
                extra["session_id"] = "BAD-" + cid
            if mode == "NEGATIVE_FEE":
                extra["total_fee_cents"] = -100
            elif mode == "UNKNOWN_STATION":
                extra["station_id"] = "ST-UNKNOWN"
            elif mode == "MISSING_ID":
                extra["session_id"] = ""
            self.emit("charging_sessions", extra, tick)
        self.corruptions[mode] += 1
        self.emit("corruption_log", dict(corruption_id=cid, table_name="charging_sessions",
            record_id=canonical["session_id"], corruption_type=mode,
            expected_action="NORMALIZE" if mode == "STATUS_FORMAT" else "REMOVE_DUPLICATE" if mode == "DUPLICATE" else "QUARANTINE"))

    def start_session(self, request, charger, tick):
        rng = self.rng
        station = charger["station"]
        vehicle = request["vehicle"]
        sid = self.ident("SES")
        soc = rng.randint(10, 55)
        target_soc = rng.randint(72, 96)
        target_wh = int(vehicle["battery_capacity_kwh"] * (target_soc-soc) * 10)
        requested_target_wh = target_wh
        anomaly = rng.choices(["NONE", "POWER_DERATING", "EARLY_STOP", "THERMAL_STRESS"], [96, 1.5, 1.5, 1])[0]
        stop_reason = "TARGET_REACHED"
        if anomaly == "EARLY_STOP":
            target_wh = max(100, target_wh // 4)
            stop_reason = "USER_STOPPED"
        steps = []
        energy = grid_energy = energy_fee = service_fee = grid_cost = 0
        max_steps = min(60, self.ticks-12-tick)
        if max_steps <= 0:
            self.finalize_request(request, "FAILED", tick, charger, reason="PERIOD_CLOSING")
            return ""
        while energy < target_wh and len(steps) < max_steps:
            at = tick + len(steps)
            temp = self.weather[station["city_id"], at//12]["temperature_c"]
            current_soc = soc + energy/(vehicle["battery_capacity_kwh"]*10)
            taper = 1 if current_soc < 72 else max(.25, (100-current_soc)/28)
            weather_factor = max(.65, 1 - max(0, 8-temp)*.006)
            power = min(charger["rated_power_kw"], vehicle["max_charge_kw"]) * taper * weather_factor
            power *= .4 if anomaly == "POWER_DERATING" else 1
            wh = min(target_wh-energy, max(1, math.floor(power*1000/12)))
            efficiency = .92 if charger["connector_type"] == "AC" else .95
            gwh = math.ceil(wh / efficiency)
            tariff = self.tariffs[station["city_id"], self.times[at].hour]
            ef = cents(wh, tariff["energy_price_cents_per_kwh"])
            sf = cents(wh, tariff["service_price_cents_per_kwh"])
            gc = cents(gwh, tariff["grid_price_cents_per_kwh"])
            steps.append((wh, gwh, gc, current_soc))
            energy += wh
            grid_energy += gwh
            energy_fee += ef
            service_fee += sf
            grid_cost += gc
        end = tick + len(steps)
        if energy < target_wh:
            stop_reason = "DURATION_LIMIT"
        parked_ticks = rng.choices([0, 1, 2, 3, 6, 12], [40, 25, 15, 10, 7, 3])[0]
        unplugged = min(end + parked_ticks, self.ticks)
        parking_fee = max(0, (unplugged-end)*5-10) * 8
        campaign = self.campaigns[station["city_id"], (tick//288)//30]
        discount = 0
        if rng.random() < .30 and self.campaign_spend[campaign["campaign_id"]] + campaign["discount_cents"] <= campaign["budget_cents"]:
            discount = min(energy_fee+service_fee, campaign["discount_cents"])
            self.campaign_spend[campaign["campaign_id"]] += discount
        total_fee = energy_fee+service_fee+parking_fee-discount
        paid = rng.random() < .97
        row = dict(session_id=sid, attempt_id=request["attempt"]["attempt_id"], user_id=vehicle["user_id"],
            vehicle_id=vehicle["vehicle_id"], station_id=station["station_id"], charger_id=charger["charger_id"],
            started_at=self.stamps[tick], ended_at=self.stamps[end], unplugged_at=self.stamps[unplugged],
            energy_wh=energy, grid_energy_wh=grid_energy, electricity_fee_cents=energy_fee,
            service_fee_cents=service_fee, parking_fee_cents=parking_fee, discount_cents=discount,
            total_fee_cents=total_fee, grid_cost_cents=grid_cost, status="COMPLETED" if paid else "WAITING_PAYMENT",
            stop_reason=stop_reason, start_soc_pct=soc,
            end_soc_pct=round(soc+energy/(vehicle["battery_capacity_kwh"]*10), 4),
            target_mode="ENERGY", target_value=requested_target_wh,
            campaign_id=campaign["campaign_id"] if discount else "")
        self.record_session(row, tick)
        self.daily_row(station, end)["completed_sessions"] += 1
        payment_tick = min(max(end+1, unplugged), self.ticks-1)
        channel = rng.choice(["WECHAT", "ALIPAY", "WALLET", "FLEET_ACCOUNT"])
        if rng.random() < .09 or not paid:
            self.emit("payments", dict(payment_id=self.ident("PAY"), session_id=sid, user_id=vehicle["user_id"],
                occurred_at=self.stamps[payment_tick], transaction_type="PAYMENT", status="FAILED", channel=channel,
                amount_cents=total_fee, failure_reason="INSUFFICIENT_BALANCE" if channel == "WALLET" else "CHANNEL_TIMEOUT"), payment_tick)
        if paid:
            payment_tick = min(payment_tick+1, self.ticks-1)
            self.emit("payments", dict(payment_id=self.ident("PAY"), session_id=sid, user_id=vehicle["user_id"],
                occurred_at=self.stamps[payment_tick], transaction_type="PAYMENT", status="SUCCESS", channel=channel,
                amount_cents=total_fee, failure_reason=""), payment_tick)
            self.daily_row(station, payment_tick)["paid_cents"] += total_fee
            if rng.random() < .025 and total_fee > 0:
                refund_tick = min(payment_tick+rng.randint(1, 288), self.ticks-1)
                refund = max(1, total_fee//rng.randint(2, 5))
                self.emit("payments", dict(payment_id=self.ident("PAY"), session_id=sid, user_id=vehicle["user_id"],
                    occurred_at=self.stamps[refund_tick], transaction_type="REFUND", status="SUCCESS", channel=channel,
                    amount_cents=refund, failure_reason=""), refund_tick)
                self.daily_row(station, refund_tick)["refund_cents"] += refund
        if rng.random() < .22:
            rating = rng.choices([1, 2, 3, 4, 5], [2, 3, 10, 35, 50])[0]
            if anomaly != "NONE" or tick-request["tick"] > 6:
                rating = min(rating, 3)
            issue = "SLOW_CHARGING" if anomaly == "POWER_DERATING" else "LONG_WAIT" if tick-request["tick"] > 6 else "NONE"
            self.emit("reviews", dict(review_id=self.ident("REV"), session_id=sid, user_id=vehicle["user_id"],
                station_id=station["station_id"], created_at=self.stamps[payment_tick], rating=rating, issue_type=issue,
                comment={"SLOW_CHARGING": "模拟评价：充电功率偏低", "LONG_WAIT": "模拟评价：等待时间较长", "NONE": "模拟评价：常规充电体验"}[issue]), payment_tick)
        if anomaly != "NONE":
            self.emit("anomaly_labels", dict(label_id=self.ident("ANOM"), session_id=sid,
                recorded_at=self.stamps[tick], anomaly_type=anomaly, severity="MEDIUM"))
        charger["session"] = dict(row=row, steps=steps, start=tick, end=end, unplugged=unplugged,
                                   anomaly=anomaly)
        self.locks[vehicle["user_id"]] = unplugged
        self.finalize_request(request, "STARTED", tick, charger, sid)
        return sid

    def resolve_hold(self, charger, tick):
        hold = charger["hold"]
        request = hold["request"]
        charger["hold"] = None
        sid = self.start_session(request, charger, tick) if hold["accept"] else ""
        if hold["kind"] == "queue":
            row = request["queue"]
            row.update(resolved_at=self.stamps[tick], status="SERVED" if sid else "CALL_EXPIRED", session_id=sid)
            self.emit("queue_entries", row, request["tick"])
            if not sid and not hold["accept"]:
                self.finalize_request(request, "ABANDONED", tick, charger, reason="CALL_TIMEOUT")
        else:
            row = hold["row"]
            status = "CONFIRMED" if sid else hold["status"]
            row.update(resolved_at=self.stamps[tick], status=status, session_id=sid)
            self.emit("reservations", row, request["tick"])
            if not sid and not hold["accept"]:
                self.finalize_request(request, "RESERVATION_"+status, tick, charger, reason=status)

    def maintenance(self, charger, tick):
        if self.rng.random() >= .00019:
            return
        end = tick + self.rng.randint(6, 72)
        accepted = min(tick+1, self.ticks)
        working = min(tick+2, self.ticks)
        resolved = end < self.ticks
        fault = self.rng.choice(["CONNECTOR", "COMMUNICATION", "COOLING", "POWER_MODULE"])
        row = dict(ticket_id=self.ident("TKT"), charger_id=charger["charger_id"], station_id=charger["station_id"],
            reported_at=self.stamps[tick], accepted_at=self.stamps[accepted], work_started_at=self.stamps[working],
            restored_at=self.stamps[end] if resolved else "", fault_type=fault, severity="MEDIUM",
            labor_cost_cents=self.rng.randint(8000, 30000), parts_cost_cents=self.rng.randint(0, 50000),
            status="RESOLVED" if resolved else "IN_PROGRESS")
        self.emit("maintenance_tickets", row, tick)
        for at, status in [(tick, "SUBMITTED"), (accepted, "ACCEPTED"), (working, "IN_PROGRESS")] + ([(end, "RESOLVED")] if resolved else []):
            self.emit("maintenance_events", dict(event_id=self.ident("MEV"), ticket_id=row["ticket_id"],
                event_at=self.stamps[at], status=status, note="模拟维修流程"), at)
        if resolved:
            self.daily_row(charger["station"], end)["maintenance_cost_cents"] += row["labor_cost_cents"]+row["parts_cost_cents"]
        charger["blocked_until"] = end
        charger["blocked_state"] = "OFFLINE" if fault == "COMMUNICATION" else "MAINTENANCE"

    def new_request(self, station, tick, available):
        rng = self.rng
        dt = self.times[tick]
        hour = dt.hour
        if station["site_type"] == "OFFICE":
            shape = 2.2 if 8 <= hour <= 10 else 1.4 if 16 <= hour <= 19 else .55
            if dt.weekday() >= 5:
                shape *= .55
        elif station["site_type"] == "RESIDENTIAL":
            shape = 2 if hour >= 18 else .65
        elif station["site_type"] == "SHOPPING":
            shape = 1.6 if 11 <= hour <= 21 else .25
            if dt.weekday() >= 5:
                shape *= 1.35
        else:
            shape = 1.4 if 7 <= hour <= 21 else .3
        demand = station["intensity"] * shape * self.events[station["city_id"], tick//288]
        if self.weather[station["city_id"], tick//12]["weather"] != "CLEAR":
            demand *= .88
        if rng.random() >= min(.8, demand) or tick >= self.ticks-24:
            return
        vehicle = None
        for _ in range(10):
            candidate = rng.choice(self.users[station["city_id"]])
            if self.locks[candidate["user_id"]] <= tick:
                vehicle = candidate
                break
        if vehicle is None:
            return
        attempt = dict(attempt_id=self.ident("ATT"), user_id=vehicle["user_id"], vehicle_id=vehicle["vehicle_id"],
            station_id=station["station_id"], charger_id="", attempted_at=self.stamps[tick], outcome="",
            failure_reason="", session_id="", queue_id="", reservation_id="")
        request = dict(attempt=attempt, vehicle=vehicle, tick=tick, patience=tick+rng.randint(3, 12))
        self.locks[vehicle["user_id"]] = self.ticks+1
        if rng.random() < .035:
            self.finalize_request(request, "FAILED", tick, available[0] if available else None,
                reason=rng.choice(["AUTH_FAILED", "CONNECTOR_HANDSHAKE", "APP_TIMEOUT"]))
        elif available and not station["waiting"]:
            charger = available[0]
            if rng.random() < .18:
                due = tick+rng.randint(1, 4)
                choice = rng.random()
                status = "CANCELLED" if choice < .10 else "EXPIRED" if choice < .18 else "CONFIRMED"
                row = dict(reservation_id=self.ident("RSV"), user_id=vehicle["user_id"], station_id=station["station_id"],
                    charger_id=charger["charger_id"], created_at=self.stamps[tick], expires_at=self.stamps[due],
                    resolved_at="", status=status, session_id="")
                attempt["reservation_id"] = row["reservation_id"]
                charger["hold"] = dict(kind="reservation", request=request, due=due,
                    accept=status=="CONFIRMED", status=status, row=row)
            else:
                self.start_session(request, charger, tick)
        elif rng.random() < .60 and len(station["waiting"]) < 20:
            row = dict(queue_id=self.ident("QUE"), user_id=vehicle["user_id"], station_id=station["station_id"],
                joined_at=self.stamps[tick], called_at="", resolved_at="", position_at_join=len(station["waiting"])+1,
                status="WAITING", session_id="")
            request["queue"] = row
            attempt["queue_id"] = row["queue_id"]
            station["waiting"].append(request)
        else:
            self.finalize_request(request, "ABANDONED", tick, reason="NO_AVAILABLE_CHARGER")

    def telemetry(self, charger, tick):
        session = charger["session"]
        energy = grid = gc = 0
        session_id = ""
        if session:
            session_id = session["row"]["session_id"]
            if tick < session["end"]:
                state = "CHARGING"
                energy, grid, gc, soc = session["steps"][tick-session["start"]]
                voltage = round((3.2 + soc*.009)*96, 2)
                ambient = self.weather[charger["station"]["city_id"], tick//12]["temperature_c"]
                temp = round(max(15, ambient*.2+23+energy*.0012), 1)
                if session["anomaly"] == "THERMAL_STRESS":
                    temp += 12
                self.emit("battery_samples", dict(session_id=session_id, charger_id=charger["charger_id"],
                    recorded_at=self.stamps[tick], soc_pct=round(soc, 4), pack_voltage_v=voltage,
                    charge_current_a=round(energy*12/voltage, 2),
                    max_cell_voltage_v=round(voltage/96+.018, 4), min_cell_voltage_v=round(voltage/96-.018, 4),
                    max_temperature_c=temp, min_temperature_c=round(temp-2.5, 1)), tick)
            else:
                state = "OCCUPIED"
        elif charger["hold"]:
            state = "RESERVED"
        elif tick < charger["blocked_until"]:
            state = charger["blocked_state"]
        else:
            state = "AVAILABLE"
        charger["meter_wh"] += energy
        row = dict(charger_id=charger["charger_id"], station_id=charger["station_id"], recorded_at=self.stamps[tick],
            interval_seconds=300, state=state, session_id=session_id, power_kw=round(energy*.012, 6),
            energy_wh=energy, grid_energy_wh=grid, grid_cost_cents=gc,
            meter_wh=charger["meter_wh"], online=int(state != "OFFLINE"))
        self.emit("charger_telemetry", row, tick)
        return row

    def run(self):
        self.dimensions()
        hourly = {}
        for tick in range(self.ticks):
            if tick % 288 == 0 and tick//288 % 30 == 0:
                print(f"Simulating day {tick//288+1}/{self.config['days']}", flush=True)
            if tick % 12 == 0:
                hourly = {}
                for station in self.stations:
                    row = dict.fromkeys(SUMMARY_TABLES["station_hourly"], 0)
                    row.update(station_id=station["station_id"], city_id=station["city_id"],
                               recorded_at=self.stamps[tick], capacity=3)
                    hourly[station["station_id"]] = row
            for charger in self.chargers:
                if charger["session"] and tick >= charger["session"]["unplugged"]:
                    charger["session"] = None
                if charger["hold"] and tick >= charger["hold"]["due"]:
                    self.resolve_hold(charger, tick)
                if not charger["session"] and not charger["hold"] and tick >= charger["blocked_until"]:
                    self.maintenance(charger, tick)
            for station in self.stations:
                waiting = station["waiting"]
                remaining = deque()
                for request in waiting:
                    if tick >= request["patience"]:
                        request["queue"].update(resolved_at=self.stamps[tick], status="ABANDONED")
                        self.emit("queue_entries", request["queue"], request["tick"])
                        self.finalize_request(request, "ABANDONED", tick, reason="QUEUE_PATIENCE")
                    else:
                        remaining.append(request)
                station["waiting"] = remaining
                available = [c for c in station["chargers"] if not c["session"] and not c["hold"] and tick >= c["blocked_until"]]
                for charger in available:
                    if not remaining or tick >= self.ticks-12:
                        break
                    request = remaining.popleft()
                    request["queue"]["called_at"] = self.stamps[tick]
                    charger["hold"] = dict(kind="queue", request=request, due=tick+1, accept=self.rng.random()<.85)
                available = [c for c in available if not c["hold"]]
                self.new_request(station, tick, available)
                available_count = 0
                for charger in station["chargers"]:
                    sample = self.telemetry(charger, tick)
                    row = hourly[station["station_id"]]
                    row[sample["state"].lower()+"_samples"] += 1
                    row["sample_count"] += 1
                    row["energy_wh"] += sample["energy_wh"]
                    available_count += sample["state"] == "AVAILABLE"
                    daily = self.daily_row(station, tick)
                    daily["energy_wh"] += sample["energy_wh"]
                    daily["grid_cost_cents"] += sample["grid_cost_cents"]
                row["end_available_count"] = available_count
            if tick % 12 == 11:
                for row in hourly.values():
                    row["mean_power_kw"] = round(row["energy_wh"]/1000, 6)
                    self.summaries.write("station_hourly", row, self.times[tick].strftime("%Y-%m"))
        for station in self.stations:
            for request in station["waiting"]:
                self.emit("queue_entries", request["queue"], request["tick"])
                self.finalize_request(request, "QUEUED", self.ticks, reason="PERIOD_END_PENDING")
        for (_, _), row in sorted(self.daily.items()):
            self.summaries.write("station_daily", row, row["business_date"][:7])
        tables = self.writer.finish(preview=True)
        reference = self.summaries.finish()
        manifest = dict(dataset_id=self.config["dataset_id"], schema_version=SCHEMA_VERSION,
            generator_version=__version__, source="SIMULATED", config=self.config,
            business_timezone="Asia/Shanghai", timestamp_timezone="UTC",
            period_start=self.stamps[0], period_end_exclusive=self.stamps[-1],
            tables=tables, reference_aggregates=reference,
            canonical_session_count=self.canonical_sessions, corruptions=dict(self.corruptions),
            expected_telemetry_rows=75*self.ticks,
            definitions={"energy_wh": "charger output interval energy", "grid_energy_wh": "simulated purchased interval energy including conversion loss",
                         "power_kw": "interval mean output power", "meter_wh": "cumulative output energy at interval end",
                         "reference_aggregates": "independent Python simulator controls, not Spark outputs",
                         "battery_samples": "simulated charging-only values; positive current means charging; not physical diagnostics",
                         "coordinate_system": "WGS84 illustrative offsets, not actual station locations",
                         "profit": "modeled cash contribution only; excludes taxes, capex, depreciation and demand charges"})
        write_json(self.root/"manifest.json", manifest)
        write_json(self.root/"schema.json", {"raw": TABLES, "reference_aggregates": SUMMARY_TABLES})
        print(f"Generated {self.canonical_sessions} sessions and {75*self.ticks} telemetry rows", flush=True)
        return manifest


def generate_dataset(config, destination):
    validate_config(config)
    destination = Path(destination)
    if destination.exists() and any(destination.iterdir()):
        raise FileExistsError(f"Refusing to overwrite non-empty dataset: {destination}")
    destination.mkdir(parents=True, exist_ok=True)
    return Simulator(dict(config), destination).run()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    destination = args.output or Path(__file__).resolve().parents[1]/"datasets"/config["dataset_id"]
    generate_dataset(config, destination)


if __name__ == "__main__":
    main()
