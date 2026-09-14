"""Explainable bounded scoring. Predictions and policy adjustments are separate."""
from __future__ import annotations
from datetime import datetime, timedelta, timezone
import math
import uuid

DEFAULT_WEIGHTS = {"availability": .30, "wait": .25, "travel": .20,
                   "price": .10, "power": .10, "balance": .05}


def clip(value, low=0.0, high=1.0):
    return min(high, max(low, float(value)))


def adjusted_prediction(prediction, station):
    """A transparent conservative scenario overlay, NOT another trained model.

    Shift count distribution by observed operational-free delta, and subtract
    waiting/en-route demand. Do not advertise raw holdout calibration for this
    intervention. Registered claims (CALLED/RESERVED) already reduce currentFree.
    """
    p = dict(prediction)
    capacity = int(station["capacity"])
    current = int(station["currentFree"])
    backlog = int(station.get("enRoute", 0))+int(station.get("queued", 0))
    shift = current-int(p["currentFree"])-backlog
    raw_distribution = p.get("availabilityDistribution")
    if raw_distribution is None or len(raw_distribution) != capacity+1:
        raise ValueError("arrival model count-distribution contract mismatch")
    distribution = [0.0]*(capacity+1)
    for free, probability in enumerate(raw_distribution):
        distribution[int(clip(free+shift, 0, capacity))] += float(probability)
    p["rawModelPrediction"] = {k: prediction[k] for k in (
        "expectedFree", "availableProbability", "waitMinutes", "waitP90Minutes", "serviceProbability")}
    p["availabilityDistribution"] = distribution
    p["expectedFree"] = sum(free*prob for free, prob in enumerate(distribution))
    p["availableProbability"] = 1-distribution[0]
    added_wait = max(0, -shift)*10/max(1, capacity)
    p["waitMinutes"] = max(0, float(p["waitMinutes"])+added_wait)
    p["waitP90Minutes"] = max(p["waitMinutes"], float(p["waitP90Minutes"])+added_wait)
    p["currentFree"] = current
    p["loadRatio"] = clip(1-current/max(1, capacity))
    p["operationalAdjustment"] = {"countShift": shift, "registeredDemand": backlog,
        "extraWaitMinutes": round(added_wait, 2), "kind": "CONSERVATIVE_SCENARIO_HEURISTIC",
        "evaluatedModelProbability": prediction["availableProbability"]}
    return p


def score_candidates(candidates, config=None):
    config = config or {}
    weights = config.get("weights", DEFAULT_WEIGHTS)
    if set(weights) != set(DEFAULT_WEIGHTS) or any(not math.isfinite(float(v)) or v < 0 for v in weights.values()) or abs(sum(weights.values())-1) > 1e-6:
        raise ValueError("six nonnegative weights must sum to1")
    rewards = config.get("rewards", {"first": 30, "second": 10})
    if not candidates:
        return []
    congestion = sum(c["loadRatio"] for c in candidates)/len(candidates)
    imbalance = max(c["loadRatio"] for c in candidates)-min(c["loadRatio"] for c in candidates)
    multiplier = 2.0 if congestion >= .7 and imbalance >= .3 else (.5 if imbalance < .15 else 1.0)
    output = []
    for source in candidates:
        c = dict(source)
        dimensions = {"availability": clip(c["availableProbability"]),
                      "wait": 1-clip(c["waitMinutes"]/45),
                      "travel": 1-clip(c["etaMinutes"]/60),
                      "price": 1-clip(c["pricePerKwh"]/3),
                      "power": clip(c["powerKw"]/120),
                      "balance": 1-clip(c["loadRatio"])}
        c["scoreBreakdown"] = {key: round(dimensions[key]*weights[key]*100, 3) for key in weights}
        c["score"] = round(sum(c["scoreBreakdown"].values()), 2)
        c["totalMinutes"] = round(c["etaMinutes"]+c["waitMinutes"]+c.get("chargingMinutes", 0), 2)
        c["rewardMultiplier"] = multiplier
        chance = c["availableProbability"]
        chance_text = ">99.9%" if .999 <= chance < 1 else f"{chance:.1%}"
        c["reasons"] = [f"到站有空桩机会约{chance_text}",
                         f"成功开始充电条件下预计等{c['waitMinutes']:.1f}分钟",
                         f"行驶约{c['etaMinutes']:.1f}分钟，{c['pricePerKwh']:.2f}元/度"]
        if c.get("operationalAdjustment", {}).get("countShift"):
            c["reasons"].append("已考虑当前演示订单和排队承诺；该修正为保守规则")
        output.append(c)
    output.sort(key=lambda c: (-c["score"], c["etaMinutes"], c["stationId"]))
    for rank, candidate in enumerate(output, 1):
        candidate["rank"] = rank
        base = rewards["first"] if rank == 1 else rewards["second"] if rank == 2 else 0
        candidate["rewardPoints"] = max(0, int(round(base*multiplier)))
    return output


class RecommendationService:
    def __init__(self, predictor, store, routing):
        self.predictor, self.store, self.routing = predictor, store, routing

    def recommend(self, user_id, city_id, origin, energy_kwh, max_eta):
        from .store import DomainError
        stations = self.store.stations(city_id)
        if not stations:
            raise DomainError("CITY_NOT_FOUND", "城市不存在", 404)
        city = next((c for c in self.predictor.cities if c["cityId"] == city_id), None)
        from .routing import haversine
        if city is None or haversine(origin, city) > 100:
            raise DomainError("ORIGIN_OUT_OF_CITY", "起点需在所选城市周边100公里内", 422)
        routes = {r["stationId"]: r for r in self.routing.routes(origin, stations)}
        # Slow external I/O precedes the atomic operational-clock/capacity snapshot.
        state = self.store.admin()
        reference = state["clock"]["time"]
        stations = [s for s in state["stations"] if s["cityId"] == city_id]
        candidates, warnings = [], []
        config = state["config"]
        for station in stations:
            route = routes[station["stationId"]]
            if route["etaMinutes"] > min(60, max_eta):
                continue
            try:
                prediction = self.predictor.predict(station["stationId"], reference, route["etaMinutes"])
            except ValueError as exc:
                raise DomainError("MODEL_WINDOW_UNAVAILABLE", "回放时间超出模型历史范围，请启动新的演示数据库", 409) from exc
            adjusted = adjusted_prediction(prediction, station)
            quoted_price = self.predictor.price(station["stationId"], adjusted["arrivalTime"])
            candidates.append({**station, **route, **adjusted, "pricePerKwh": quoted_price,
                "pricePolicy": "ARRIVAL_TARIFF_FIXED_QUOTE",
                "chargingMinutes": round(energy_kwh/max(.1, station["powerKw"])*60, 2)})
        candidates = score_candidates(candidates, config)
        if not candidates:
            raise DomainError("NO_CANDIDATES", "行驶时间范围内没有电站，请更换起点或扩大范围", 422)
        if any(c["routeSource"] != "TENCENT_CURRENT_TRAFFIC" for c in candidates):
            warnings.append("未使用实时路况：ETA为距离×1.3、时速28公里及2分钟准备时间的情景估计。")
        else:
            warnings.append("ETA使用腾讯当前路况；充电状态为历史模拟回放，两者并非同日实测。")
        warnings.extend(["电站与充电行为均为模拟数据，不可用于真实驾车或运营决策。",
            "预测为5分钟粒度，等待为成功开始充电条件下估计；同时展示服务成功率。",
            "当前演示订单修正属于保守规则，原模型测试指标不等于干预后的准确率。"])
        nearest = min(candidates, key=lambda c: (c["distanceKm"], c["stationId"]))
        best = candidates[0]
        created = datetime.fromisoformat(reference.replace("Z", "+00:00"))
        payload = {"recommendationId": uuid.uuid4().hex, "createdAt": reference,
            "expiresAt": (created+timedelta(minutes=5)).isoformat().replace("+00:00", "Z"),
            "referenceTime": reference, "origin": origin, "cityId": city_id, "energyKwh": energy_kwh,
            "candidates": candidates, "warnings": warnings,
            "nearestComparison": {"nearestStationId": nearest["stationId"],
                "nearestStationName": nearest["stationName"], "recommendedStationId": best["stationId"],
                "nearestTotalMinutes": nearest["totalMinutes"], "recommendedTotalMinutes": best["totalMinutes"],
                "savedMinutes": round(nearest["totalMinutes"]-best["totalMinutes"], 2)}}
        return self.store.save_recommendation(user_id, payload)
