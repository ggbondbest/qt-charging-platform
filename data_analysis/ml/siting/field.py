"""需求空间场：距离衰减核、候选网格打分、留一站回测的预测式（纯函数）。

模型假设（写进产物、不是数据事实）：
* 一次会话的"出发地"未知（只有被服务点=站点），故用**被服务需求点 + 距离衰减**近似需求场：
  站 s 的需求 d_s 视为集中在其坐标，贡献到候选点 x 为 d_s · decay(dist(s,x))；
* 衰减核 decay(d) = 1/(1+ (d/r)^2)，r=CAPTURE_KM 为可配置吸引半径（默认 3km，站内近邻
  p50≈4.1km，取半格量级）；
* 候选点打分 absorb = Σ 需求贡献；空白度 opportunity = absorb × (1 - 现有覆盖折扣)，
  现有覆盖折扣 = 距候选点 r 内已有站的累计服务占比（离得近=已被满足）。

这些是**可复核的假设**；盲评由 `backtest.py`（调用本模块 `loso_predict`）承担：藏站、重建场、预测被藏站的真实需求。
"""

from __future__ import annotations

import numpy as np

EARTH_R_KM = 6371.0


def haversine_km(lat1, lon1, lat2, lon2) -> float:
    """两点大圆距离（km）。标量版，向量化由 distance_matrix 承担。"""
    p1, p2 = np.radians(lat1), np.radians(lat2)
    dphi, dlmb = np.radians(lat2 - lat1), np.radians(lon2 - lon1)
    h = np.sin(dphi / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dlmb / 2) ** 2
    return float(2 * EARTH_R_KM * np.arcsin(np.sqrt(h)))


def distance_matrix(coords_a, coords_b) -> np.ndarray:
    """两组 (lat,lon) 的全对全距离矩阵（km）。coords_* 为 (N,2)/(M,2) 数组。"""
    a = np.atleast_2d(np.asarray(coords_a, dtype=float))
    b = np.atleast_2d(np.asarray(coords_b, dtype=float))
    lat = np.radians(a[:, 0])[:, None]
    dlat = np.radians(b[:, 0])[None, :] - lat
    dlon = np.radians(b[:, 1])[None, :] - np.radians(a[:, 1])[:, None]
    h = np.sin(dlat / 2) ** 2 + np.cos(lat) * np.cos(np.radians(b[:, 0])[None, :]) * np.sin(dlon / 2) ** 2
    return 2 * EARTH_R_KM * np.arcsin(np.sqrt(h))


def decay(dist_km: np.ndarray, radius_km: float) -> np.ndarray:
    """gravity 衰减核 1/(1+(d/r)^2)，值域 (0,1]，d=0 处为 1。"""
    return 1.0 / (1.0 + (np.asarray(dist_km, dtype=float) / float(radius_km)) ** 2)


def field_absorb(candidate_coords, source_coords, source_demand, radius_km) -> np.ndarray:
    """每个候选点的可吸收需求 = Σ_s 需求_s · decay(dist)。source_demand 与 source_coords 对齐。"""
    demand = np.asarray(source_demand, dtype=float)
    D = distance_matrix(candidate_coords, source_coords)
    return decay(D, radius_km) @ demand


def coverage_discount(candidate_coords, served_coords, served_demand, radius_km) -> np.ndarray:
    """候选点 r 半径内**现有站**吃掉的需求占比（0..1）：离已建站越近，越多需求已被满足。

    归属规则：每个已建站的 demand 全部记在其坐标上；候选点的覆盖 = 半径内站点需求 / 全体需求。
    这里"全体需求"是被评估的 source 集，故调用方传进来的 served_* 需与 field_absorb 同一口径。"""
    D = distance_matrix(candidate_coords, served_coords)
    near = D <= radius_km
    demand = np.asarray(served_demand, dtype=float)
    total = demand.sum()
    if total <= 0:
        return np.zeros(len(candidate_coords))
    return (near.astype(float) @ demand) / total


def score_candidates(candidate_coords, source_coords, source_demand, *,
                     radius_km: float = 3.0) -> dict[str, np.ndarray]:
    """返回每个候选点的 absorb（可吸收需求）/ coverage（现有覆盖占比）/ opportunity（空白度）。

    opportunity = absorb × (1 − coverage)：既在需求热点附近、又离现网覆盖远，才值得新设站。
    """
    absorb = field_absorb(candidate_coords, source_coords, source_demand, radius_km)
    coverage = coverage_discount(candidate_coords, source_coords, source_demand, radius_km)
    return {"absorb": absorb, "coverage": coverage, "opportunity": absorb * (1.0 - coverage)}


def grid_candidates(min_lat, max_lat, min_lon, max_lon, step_deg: float) -> np.ndarray:
    """经纬度网格候选点（(N,2) 数组）。step_deg=0.05 ≈ 5.5km，站点近邻 4.1km 同量级。"""
    lats = np.arange(min_lat, max_lat + 1e-9, step_deg)
    lons = np.arange(min_lon, max_lon + 1e-9, step_deg)
    LON, LAT = np.meshgrid(lons, lats)
    return np.column_stack([LAT.ravel(), LON.ravel()])


def loso_predict(coords, demand, *, radius_km: float = 3.0) -> np.ndarray:
    """留一站回测的预测式：对每个站 i，**只用其余站**重建需求场，读 i 坐标处的 absorb。

    返回 predicted[i]。这是外推测试：被藏站不出现在场里，预测完全来自邻居的距离衰减。
    刻意用与其它站同口径的 field_absorb(coords[i] 单点)，保证"同一个模型"。"""
    coords = np.atleast_2d(np.asarray(coords, dtype=float))
    demand = np.asarray(demand, dtype=float)
    predicted = np.zeros(len(coords))
    for i in range(len(coords)):
        keep = np.arange(len(coords)) != i
        one = field_absorb(coords[i][None, :], coords[keep], demand[keep], radius_km)
        predicted[i] = one[0]
    return predicted
