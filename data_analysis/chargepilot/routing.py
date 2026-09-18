"""One-to-many ETA, with explicit and bounded offline fallback.

Tencent matrix duration is SECONDS, direction duration is MINUTES.
Sources: TencentLBS/tencentmap-webservice-skill references/api-direction.md.
Synthetic catalogue and Leaflet use approximate WGS84; provider calls use GCJ02.
No secret, provider URL, or raw upstream response is returned to clients/logs.
"""
from __future__ import annotations
import hashlib
import math
import threading
import time
from urllib.parse import urlencode

import httpx


def haversine(a, b):
    lat1, lon1, lat2, lon2 = map(math.radians, (a["latitude"], a["longitude"], b["latitude"], b["longitude"]))
    d = math.sin((lat2-lat1)/2)**2 + math.cos(lat1)*math.cos(lat2)*math.sin((lon2-lon1)/2)**2
    return 6371.0088 * 2 * math.asin(math.sqrt(min(1, max(0, d))))


def wgs_to_gcj(lat, lon):
    """Conventional GCJ02 approximate offset; outside China return unchanged."""
    if not (72.004 <= lon <= 137.8347 and 0.8293 <= lat <= 55.8271):
        return lat, lon
    x, y = lon-105, lat-35
    dlat = -100+2*x+3*y+0.2*y*y+0.1*x*y+0.2*math.sqrt(abs(x))
    dlon = 300+x+2*y+0.1*x*x+0.1*x*y+0.1*math.sqrt(abs(x))
    common = (20*math.sin(6*x*math.pi)+20*math.sin(2*x*math.pi))*2/3
    dlat += common+(20*math.sin(y*math.pi)+40*math.sin(y*math.pi/3))*2/3
    dlat += (160*math.sin(y*math.pi/12)+320*math.sin(y*math.pi/30))*2/3
    dlon += common+(20*math.sin(x*math.pi)+40*math.sin(x*math.pi/3))*2/3
    dlon += (150*math.sin(x*math.pi/12)+300*math.sin(x*math.pi/30))*2/3
    rad = lat/180*math.pi
    magic = 1-0.00669342162296594323*math.sin(rad)**2
    dlat = dlat*180/((6378245*(1-0.00669342162296594323))/(magic*math.sqrt(magic))*math.pi)
    dlon = dlon*180/(6378245/math.sqrt(magic)*math.cos(rad)*math.pi)
    return lat+dlat, lon+dlon


def gcj_to_wgs(lat, lon):
    # A few fixed-point iterations are sufficient for display, not survey use.
    a, b = lat, lon
    for _ in range(3):
        aa, bb = wgs_to_gcj(a, b)
        a, b = a+(lat-aa), b+(lon-bb)
    return a, b


def estimated_routes(origin, stations):
    """Scenario assumption, not measured traffic: distance*1.3 / 28 km/h +2m."""
    result = []
    for station in stations:
        distance = haversine(origin, station)*1.3
        result.append({"stationId": station["stationId"], "distanceKm": round(distance, 3),
                       "etaMinutes": round(max(2, 2+distance/28*60), 2),
                       "routeSource": "ESTIMATED_DISTANCE_SPEED"})
    return result


class RoutePlanner:
    def __init__(self, key="", secret="", client=None):
        self.key, self.secret = key, secret
        self.client = client or httpx.Client(timeout=httpx.Timeout(4.0, connect=2.0), trust_env=False,
                                             follow_redirects=False)
        self._cache = {}
        self._lock = threading.Lock()
        self._cooldown = 0.0

    def _get(self, path, params):
        params = {**params, "key": self.key}
        if self.secret:
            # Provider signs sorted raw query values, before URL encoding.
            raw = "&".join(f"{k}={params[k]}" for k in sorted(params))
            params["sig"] = hashlib.md5((path+"?"+raw+self.secret).encode()).hexdigest()
        response = self.client.get("https://apis.map.qq.com"+path, params=params)
        response.raise_for_status()
        if len(response.content) > 2_000_000:
            raise ValueError("provider response too large")
        payload = response.json()
        if payload.get("status") != 0:
            raise ValueError("provider rejected request")
        return payload["result"]

    def routes(self, origin, stations):
        fallback = estimated_routes(origin, stations)
        if not self.key or time.monotonic() < self._cooldown:
            return fallback
        cache_key = (round(origin["latitude"], 5), round(origin["longitude"], 5),
                     tuple(s["stationId"] for s in stations))
        now = time.monotonic()
        with self._lock:
            cached = self._cache.get(cache_key)
            if cached and cached[0] > now:
                return cached[1]
        try:
            a, b = wgs_to_gcj(origin["latitude"], origin["longitude"])
            targets = [wgs_to_gcj(s["latitude"], s["longitude"]) for s in stations]
            data = self._get("/ws/distance/v1/matrix", {
                "mode": "driving", "from": f"{a:.6f},{b:.6f}",
                "to": ";".join(f"{lat:.6f},{lon:.6f}" for lat, lon in targets)})
            elements = data["rows"][0]["elements"]
            if len(elements) != len(stations):
                raise ValueError("incomplete matrix")
            routes = []
            for element, station, default in zip(elements, stations, fallback):
                duration, distance = float(element.get("duration", -1)), float(element.get("distance", -1))
                if (element.get("status", 0) != 0 or not math.isfinite(duration+distance)
                        or duration <= 0 or distance < 0):
                    routes.append(default)
                else:
                    routes.append({"stationId": station["stationId"], "etaMinutes": round(duration/60, 2),
                                   "distanceKm": round(distance/1000, 3), "routeSource": "TENCENT_CURRENT_TRAFFIC"})
            with self._lock:
                if len(self._cache) >= 256:
                    self._cache.clear()
                self._cache[cache_key] = (now+300, routes)
            return routes
        except (httpx.HTTPError, ValueError, KeyError, TypeError, IndexError):
            # A single bounded attempt, no hidden retry loop or API quota storm.
            self._cooldown = time.monotonic()+30
            return fallback

    def path(self, origin, station):
        line = [[origin["latitude"], origin["longitude"]], [station["latitude"], station["longitude"]]]
        if not self.key or time.monotonic() < self._cooldown:
            return {"routeSource": "STRAIGHT_LINE_DEMO", "coordinates": line,
                    "notice": "演示连线，并非道路导航；请勿用于实际驾驶"}
        try:
            a, b = wgs_to_gcj(*line[0]); c, d = wgs_to_gcj(*line[1])
            data = self._get("/ws/direction/v1/driving/", {"from": f"{a:.6f},{b:.6f}",
                "to": f"{c:.6f},{d:.6f}", "policy": "LEAST_TIME", "no_step": 1})
            route = data["routes"][0]
            points = list(map(float, route["polyline"]))
            if not 4 <= len(points) <= 100000 or len(points) % 2:
                raise ValueError("invalid polyline")
            for i in range(2, len(points)):
                points[i] = points[i-2]+points[i]/1_000_000
            coordinates = [list(gcj_to_wgs(points[i], points[i+1])) for i in range(0, len(points), 2)]
            if not all(math.isfinite(a+b) and -90 <= a <= 90 and -180 <= b <= 180 for a,b in coordinates):
                raise ValueError("invalid coordinates")
            return {"routeSource": "TENCENT_CURRENT_TRAFFIC", "coordinates": coordinates,
                    "notice": "腾讯当前道路路线；与历史回放日期的路况无关"}
        except (httpx.HTTPError, ValueError, KeyError, TypeError, IndexError):
            self._cooldown = time.monotonic()+30
            return {"routeSource": "STRAIGHT_LINE_DEMO", "coordinates": line,
                    "notice": "路线服务暂不可用，当前为演示连线，并非道路导航"}

    def close(self):
        self.client.close()
