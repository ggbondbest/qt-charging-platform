"""Paired deterministic insertion simulation, not a real-world causal experiment.

Both policies share origins, energy requests, release clock and fixed replay
background. Forecasters only see data before each decision. Future availability
is read ONLY by the simulator to allocate a feasible charging interval. This is
a deliberately limited capacity-constrained replay, not proof of field uplift.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import tempfile
import os
import random
import statistics
import time

from .ranking import score_candidates, clip, DEFAULT_WEIGHTS
from .routing import estimated_routes


def stamp(value):
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def atomic_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name+".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf8") as handle:
            json.dump(report, handle, ensure_ascii=False, allow_nan=False, indent=2)
            handle.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def run_experiment(predictor, users=1000, seed=42, output=None, config=None):
    if not 100 <= users <= 2000:
        raise ValueError("users must be100..2000")
    began = time.monotonic()
    config = {"weights": dict((config or {}).get("weights", DEFAULT_WEIGHTS)),
              "rewards": dict((config or {}).get("rewards", {"first":30,"second":10}))}
    randomizer = random.Random(seed)
    city_stations = defaultdict(list)
    for station in predictor.catalog:
        city_stations[station["cityId"]].append(station)
    # A held-out date, never selected after seeing policy results.
    start = datetime(2026, 5, 6, tzinfo=timezone.utc)  # 08:00 local
    end = start+timedelta(hours=12)
    requests = []
    for index in range(users):
        city_id = randomizer.choice(sorted(city_stations))
        # Uneven demand around first sites is an explicit scenario, not inferred GPS.
        group = city_stations[city_id]
        anchor = randomizer.choices(group, weights=[4]+[1]*(len(group)-1))[0]
        origin = {"latitude": anchor["latitude"]+randomizer.gauss(0, .015),
                  "longitude": anchor["longitude"]+randomizer.gauss(0, .02)}
        seconds = randomizer.randint(0, 10*3600)
        requests.append({"id": index, "cityId": city_id, "time": stamp(start+timedelta(seconds=seconds)),
                         "origin": origin, "energyKwh": round(randomizer.uniform(5, 15), 2),
                         "choiceDraw": randomizer.random()})
    requests.sort(key=lambda r: (r["time"], r["id"]))
    request_hash = hashlib.sha256(json.dumps(requests, sort_keys=True).encode()).hexdigest()
    # Shared causal model cache: exact same station/time/ETA input for both policies.
    forecasts, actual_cache = {}, {}

    def actual_count(sid, tick):
        key = (sid, int(tick.timestamp())//300)
        if key not in actual_cache:
            result = predictor.actual(sid, stamp(tick))
            actual_cache[key] = int(result.get("availableCount", result["currentFree"]))
        return actual_cache[key]

    results = []
    for policy in ("nearest", "ai_incentive"):
        allocations, commitments, rows = defaultdict(list), defaultdict(list), []
        for request in requests:
            now = datetime.fromisoformat(request["time"].replace("Z", "+00:00"))
            stations = city_stations[request["cityId"]]
            routes = estimated_routes(request["origin"], stations)
            candidates = []
            for station, route in zip(stations, routes):
                eta = route["etaMinutes"]
                if eta > 60:
                    continue
                key = (station["stationId"], request["time"], eta)
                if key not in forecasts:
                    forecasts[key] = predictor.predict(key[0], key[1], key[2])
                prediction = dict(forecasts[key])
                # Only whether an earlier user is still outstanding at NOW is known.
                outstanding = sum(terminal > now for terminal in commitments[station["stationId"]])
                distribution = [0.0]*(station["capacity"]+1)
                for free, probability in enumerate(prediction["availabilityDistribution"]):
                    distribution[max(0, free-outstanding)] += probability
                prediction["availableProbability"] = 1-distribution[0]
                prediction["expectedFree"] = sum(i*p for i,p in enumerate(distribution))
                prediction["waitMinutes"] += outstanding*10/max(1, station["capacity"])
                prediction["loadRatio"] = clip(prediction["loadRatio"]+outstanding/max(1,station["capacity"]))
                candidates.append({**station, **route, **prediction,
                    "pricePerKwh": predictor.price(station["stationId"], stamp(now+timedelta(minutes=eta))),
                    "chargingMinutes": request["energyKwh"]/max(1, station["powerKw"])*60})
            ranked = score_candidates(candidates, config)
            if not ranked:
                rows.append({"served": False, "wait": 120, "drive": 60, "total": 180,
                             "free": False, "rank": None, "reward": 0})
                continue
            nearest = min(ranked, key=lambda r: (r["distanceKm"], r["stationId"]))
            # Hypothetical 80% incentive take-up is fixed in advance, not learned.
            chosen = ranked[0] if policy == "ai_incentive" and request["choiceDraw"] < .8 else nearest
            sid, capacity = chosen["stationId"], chosen["capacity"]
            arrival = now+timedelta(minutes=chosen["etaMinutes"])
            minutes = max(5, chosen["chargingMinutes"])
            duration = timedelta(minutes=minutes)
            slot, waiting, immediate = None, 120.0, False
            for step in range(25):
                candidate_start = arrival+timedelta(minutes=step*5)
                candidate_end = candidate_start+duration
                # Fixed background gets priority; a complete feasible interval is required.
                tick = candidate_start
                checks = {candidate_start}
                while tick < candidate_end:
                    checks.add(tick)
                    tick = datetime.fromtimestamp((int(tick.timestamp())//300+1)*300, tz=timezone.utc)
                # Changes from our own continuous-time allocations may be off-grid.
                checks.update(j[edge] for j in allocations[sid] for edge in ("start", "end")
                              if candidate_start <= j[edge] < candidate_end)
                feasible = True
                for tick in sorted(checks):
                    occupied = sum(j["start"] <= tick < j["end"] for j in allocations[sid])
                    if actual_count(sid, tick) <= occupied:
                        feasible = False
                        break
                if step == 0:
                    immediate = actual_count(sid, arrival) > sum(j["start"] <= arrival < j["end"] for j in allocations[sid])
                if feasible:
                    slot, waiting = candidate_start, step*5.0
                    break
            served = slot is not None
            if served:
                allocations[sid].append({"arrival": arrival, "start": slot, "end": slot+duration})
            # Fixed input-known lease: NEVER derive recommendation feature expiry
            # from an oracle-scheduled completion/failure. Future feasibility can
            # retroactively change those outcomes, even before the changed future.
            commitments[sid].append(arrival+timedelta(minutes=120)+duration)
            rows.append({"served": served, "wait": waiting, "drive": chosen["etaMinutes"],
                         "total": chosen["etaMinutes"]+waiting+(minutes if served else 0),
                         "free": immediate, "rank": chosen["rank"],
                         "reward": chosen["rewardPoints"] if policy == "ai_incentive" and served else 0})
        utilizations, congestion, idle = [], [], []
        for station in predictor.catalog:
            sid, cap = station["stationId"], station["capacity"]
            ticks, busy_total, congested = 0, 0, 0
            tick = start
            # Verify non-grid transitions too, including allocations completing after12h.
            for change in sorted({j[edge] for j in allocations[sid] for edge in ("start", "end")}):
                own = sum(j["start"] <= change < j["end"] for j in allocations[sid])
                if own > actual_count(sid, change):
                    raise AssertionError("off-grid capacity violation in paired simulator")
            while tick < end:
                background = cap-actual_count(sid, tick)
                own = sum(j["start"] <= tick < j["end"] for j in allocations[sid])
                # Feasibility above guarantees capacity, assert to catch simulation defects.
                if background+own > cap:
                    raise AssertionError("capacity violation in paired simulator")
                busy_total += background+own
                congested += int(background+own == cap)
                ticks += 1
                tick += timedelta(minutes=5)
            utilization = busy_total/(ticks*cap)
            utilizations.append(utilization)
            congestion.append(congested/ticks)
            idle.append(1-utilization)
        results.append({"policy": policy, "label": "就近找桩" if policy == "nearest" else "AI推荐＋积分引导",
            "users": users, "served": sum(r["served"] for r in rows), "abandoned": sum(not r["served"] for r in rows),
            "meanWaitMinutes": round(statistics.mean(r["wait"] for r in rows), 3),
            "availabilitySuccessRate": round(statistics.mean(r["free"] for r in rows), 4),
            "serviceSuccessRate": round(statistics.mean(r["served"] for r in rows), 4),
            "meanDrivingMinutes": round(statistics.mean(r["drive"] for r in rows), 3),
            "meanTotalMinutes": round(statistics.mean(r["total"] for r in rows), 3),
            "meanCompletedJourneyMinutes": round(statistics.mean(r["total"] for r in rows if r["served"]), 3)
                if any(r["served"] for r in rows) else None,
            "utilizationStd": round(statistics.pstdev(utilizations), 4),
            "maxUtilization": round(max(utilizations), 4),
            "congestedStationRate": round(statistics.mean(congestion), 4),
            "idleWasteRate": round(statistics.mean(idle), 4),
            "topKChoiceRate": round(statistics.mean(r["rank"] is not None and r["rank"] <= 2 for r in rows), 4),
            "rewardPoints": sum(r["reward"] for r in rows)})
    nearest, ai = results
    report = {"status": "READY", "users": users, "seed": seed, "dataSource": "SIMULATED",
        "policyConfig": config, "simulatorVersion": "capacity-insertion-v2-causal-lease",
        "policyScope": "ARRIVAL_MODEL_AND_INCENTIVE_BASELINE_WITHOUT_HOURLY_LOAD_ENRICHMENT",
        "utilizationDefinition": "resource unavailability including reservation/maintenance/offline, not charging-only utilization",
        "requestHash": request_hash, "generatedAt": stamp(datetime.now(timezone.utc)),
        "modelId": predictor.metadata.get("modelId"), "modelArtifactSha256": predictor.metadata.get("artifacts", {}).get("arrival.joblib", {}).get("sha256"),
        "policies": results, "durationSeconds": round(time.monotonic()-began, 2),
        "changes": {"waitReductionPercent": round((nearest["meanWaitMinutes"]-ai["meanWaitMinutes"])/max(.001,nearest["meanWaitMinutes"])*100, 2),
                    "totalTimeReductionPercent": round((nearest["meanTotalMinutes"]-ai["meanTotalMinutes"])/max(.001,nearest["meanTotalMinutes"])*100, 2)},
        "notes": ["固定随机种子、相同请求、相同背景负荷，策略分别运行。",
                  "此固定基线实验不含统一交付新增的小时负荷均衡项，不用于声称完整在线策略收益。",
                  "TEST期间2026-05-06，1000为默认人数；起点、5–15kWh需求和80%积分接受率是预设情景，不是真实用户实验。",
                  "未来状态仅供仿真环境检查连续容量；推荐输入使用出发前数据及固定承诺租约，不读取未来实际成功/失败。",
                  "承诺租约在选择时固定为ETA+120分钟+目标充电时长，即使提前完成也不缩短；属于保守对照情景。",
                  "固定背景占用优先，新增请求按出发决策先后插入可完整容纳充电的时间段，最长等待120分钟；不等同业务端到站FIFO。",
                  "utilization指标表示含维护/预留/占位的资源忙碌度，不是纯充电利用率。",
                  "失败等待按120分钟计入均值；meanTotalMinutes对失败只计行驶+等待，不宣称完成时间。",
                  "奖励是按配置计算的仿真成本，不写入用户账户，也不受演示账户日预算影响。",
                  "仿真不是现场因果证据；指标即使变差也原样报告。"]}
    if output:
        atomic_report(output, report)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, default=Path("data_analysis/outputs/chargepilot"))
    parser.add_argument("--users", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    from .ml.predict import ArrivalPredictor
    result = run_experiment(ArrivalPredictor(args.model_dir), args.users, args.seed, args.model_dir/"experiment.json")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
