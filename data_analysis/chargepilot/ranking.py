"""Explainable bounded scoring. Predictions and policy adjustments are separate."""
from __future__ import annotations
from datetime import datetime, timedelta, timezone
import math
import threading
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
                      "balance": 1-clip(c.get("balancePressure", c["loadRatio"]))}
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
        if c.get("loadForecast"):
            c["reasons"].append(f"到站所在小时预计平均负荷{c['loadForecast']['meanPowerKw']:.1f}kW，已用于均衡评分（非即时功率）")
        output.append(c)
    output.sort(key=lambda c: (-c["score"], c["etaMinutes"], c["stationId"]))
    for rank, candidate in enumerate(output, 1):
        candidate["rank"] = rank
        base = rewards["first"] if rank == 1 else rewards["second"] if rank == 2 else 0
        candidate["rewardPoints"] = max(0, int(round(base*multiplier)))
    return output


class RecommendationService:
    def __init__(self, predictor, store, routing, load_adapter=None):
        self.predictor, self.store, self.routing = predictor, store, routing
        self.load_adapter = load_adapter
        self._load_cache = {}
        self._load_lock = threading.RLock()

    def load_context(self, station, reference, arrival_time, occupied_ratio):
        """PR66 forecasts only help the 5% balancing dimension, not availability.

        Features end strictly before the current complete-hour boundary. The
        forecast for the arrival's hour remains an hourly mean, not arrival kW.
        No failed/missing model is replaced by a fabricated prediction.
        """
        if self.load_adapter is None or self.load_adapter.report["status"] != "READY":
            return {}
        ref = datetime.fromisoformat(reference.replace("Z", "+00:00")).replace(minute=0, second=0, microsecond=0)
        hour = ref.isoformat().replace("+00:00", "Z")
        key = (station["stationId"], hour)
        from .store import DomainError
        try:
            rated = float(station["ratedCapacityKw"])
            if not math.isfinite(rated) or rated <= 0:
                return {}
            with self._load_lock:
                if key not in self._load_cache:
                    result = self.load_adapter.predict(station["stationId"], hour, 6)
                    from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
                    metadata = self.load_adapter.report.get("metadata", {})
                    if (result.get("unit") != "kW" or result.get("schemaVersion") != CONTRACT_VERSION or
                            result.get("featureVersion") != FEATURE_VERSION or
                            result.get("modelId") != self.load_adapter.report.get("modelId") or
                            result.get("modelVersion") != metadata.get("modelVersion") or
                            result.get("referenceTime") != hour or len(result.get("points", [])) != 6):
                        return {}
                    for step, point in enumerate(result["points"]):
                        expected = (ref + timedelta(hours=step)).isoformat().replace("+00:00", "Z")
                        value = point.get("value")
                        if (point.get("timestamp") != expected or type(value) not in (int, float) or
                                not math.isfinite(value) or not 0 <= value <= rated):
                            return {}
                    self._load_cache[key] = result
                    if len(self._load_cache) > 150:
                        self._load_cache.pop(next(iter(self._load_cache)))
                result = self._load_cache[key]
            arrival = datetime.fromisoformat(arrival_time.replace("Z", "+00:00"))
            index = int((arrival - ref).total_seconds() // 3600)
            if not 0 <= index < len(result["points"]):
                return {}
            point = result["points"][index]
            power = float(point["value"])
            if not math.isfinite(power) or rated <= 0 or not 0 <= power <= rated:
                return {}
            return {"loadForecast": {"modelId": result["modelId"], "referenceTime": hour,
                    "timestamp": point["timestamp"], "meanPowerKw": power, "ratedCapacityKw": rated,
                    "targetSemantics": "MEAN_POWER_DURING_HOUR"},
                    "balancePressure": .5 * clip(occupied_ratio) + .5 * clip(power / rated),
                    "balancePolicy": "HALF_CURRENT_OCCUPANCY_HALF_FORECAST_POWER"}
        except (DomainError, ValueError, KeyError, RuntimeError):
            return {}

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
            load = self.load_context(station, reference, adjusted["arrivalTime"], adjusted["loadRatio"])
            quoted_price = self.predictor.price(station["stationId"], adjusted["arrivalTime"])
            candidates.append({**station, **route, **adjusted, **load, "pricePerKwh": quoted_price,
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
        if any(not c.get("loadForecast") for c in candidates):
            warnings.append("部分小时负荷模型不可用，均衡项只使用当前占用率；未伪造负荷预测。")
        else:
            warnings.append("复用小时负荷预测辅助5%的均衡评分；到站空桩和等待仍由分钟模型独立预测。")
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
