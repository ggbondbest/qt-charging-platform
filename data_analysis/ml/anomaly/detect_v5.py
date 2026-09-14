"""v5:气象上下文异常检测 + 可部署固定阈值(盲评链"全面化"的一代)。
用法(仓库根目录):python -m data_analysis.ml.anomaly.detect_v5

两个实锤动机(诊断见 train_metrics 的 weatherDiagnosis 段):
1. 天气盲区:v4 的期望温度表 (charger×电流档) 中位数只在 2-4 月 TRAIN(均温 7.4°C)上学,
   TEST 是 5 月中下旬(均温 23.1°C),温度超额被热浪整体虚抬 +2.9°C/会话——precision 0.48
   的误报大户就是它。v5 把气温直接建进期望表:温度按 (charger×电流档×三档气温)、压差按
   (charger×三档气温) 的 TRAIN 中位数,空格退回低阶表;"真过热"与"天热"从此分离。
   修正结构在 VALIDATION 上三选一时序点级 g/V2 三元组/V3 二维,机制指标(跨窗虚抬)定选,
   证据随表存于 context_meta_v5.pkl 的 adjustmentStudy。异常筛查评的是已发生会话,
   子时刻天气是已观测事实,不涉及"未来预报当已知"的合同红线。
2. 口径债:v1-v4 的可比成绩都借了 split 内秩化(=固定告警预算批筛)。v5 不再 rank:
   信号只在 TRAIN 上做稳健标准化(median/MAD),阈值 = TRAIN 融合分分位数,定死后原样
   套到任何窗口——TEST 告警率是真实部署结果,偏高偏低都如实上报。选型子集与分位数
   只在 VALIDATION 上扫(PERCENTILE_GRID_V5),TEST 依旧只批一次。

信号池 6 个,三类故障机制各有代表(全面化):tempW 过热 / cellW 压差失衡 / curDeficit 压流
+ thermalSpike、currentJump(会话内瞬时,z 口径天然天气不变)+ v1Mean(绝对曲线形态兜底)。
子集=全 2^6−1 组合等权 z 均值(6 信号穷举无贪心的次序依赖),配 percentile 网格,VAL F1 定选。
"""

from __future__ import annotations

import itertools
import json
import time

import joblib
import numpy as np
import pandas as pd

from . import common
from .detect_v3 import SEED, session_signals

SPLITS = ("TRAIN", "VALIDATION", "TEST")
PERCENTILE_GRID_V5 = (95, 96, 97, 98, 99, 99.5)
AMBIENT_BIN_C = 5.0
POOL = ("tempW", "cellW", "curDeficit", "thermalSpike", "currentJump", "v1Mean")
EPS = 1e-9


def _ambient_join(pts: pd.DataFrame) -> pd.DataFrame:
    """点级挂上"子时刻所在城市"的已观测气温/降雨。城市×小时全覆盖,NaN 只可能来自新城市,
    截断/披露双保险。"""
    w = common.load_table("weather_hourly")
    stn = common.load_table("stations")
    chg = common.load_table("chargers")
    w = w.assign(hour=pd.to_datetime(w["recorded_at"]).dt.floor("h"))
    pts = pts.assign(hour=pd.to_datetime(pts["recorded_at"]).dt.floor("h"))
    out = pts.merge(chg[["charger_id", "station_id"]].drop_duplicates(), on="charger_id", how="left") \
             .merge(stn[["station_id", "city_id"]], on="station_id", how="left") \
             .merge(w[["city_id", "hour", "temperature_c", "rainfall_mm"]],
                    on=["city_id", "hour"], how="left")
    if out["temperature_c"].isna().mean() > 0.0:
        print(f"weather join miss rate: {out['temperature_c'].isna().mean():.4f}")
    return out


def _amb3(t: pd.Series) -> pd.Series:
    """三档气温段,右闭:cold≤10 / mid(10,22] / hot>22。选型实锤(建表时复算,存 meta.adjustmentStudy):
    点级 g(5°C bin) 加法修正只砍掉虚抬的一小半(升幅随电流乘性放大),V2 三元组表把
    TEST-TRAIN 虚抬从 +2.11°C(V1) 压到 +0.53°C,格数 704、覆盖 1.0,VAL F1 不降——选 V2。"""
    return pd.cut(t, [-np.inf, 10.0, 22.0, np.inf], labels=["cold", "mid", "hot"])


def weather_signals() -> pd.DataFrame:
    """会话级:tempW/cellW 天气感知超额、curDeficit 压流洼地、tempExcessRaw(v4 口径,仅诊断对照)。
    期望表全部 TRAIN-only;三元组格空退回 (charger×curb) 基表(空数披露)。"""
    if common.WEATHER_CTX_TABLE.exists():
        return pd.read_pickle(common.WEATHER_CTX_TABLE)
    pts = _ambient_join(pd.read_pickle(common.POINT_TABLE)).copy()
    need = ["soc_pct", "charge_current_a", "max_temperature_c", "celldiff"]
    if pts[need].isna().any().any():
        raise RuntimeError("battery_samples 关键列含 NaN:整除分箱会硬崩,先定插补口径(数值变更需升 model_id)")
    pts["socb"] = (pts["soc_pct"] // 10).clip(0, 9)
    pts["curb"] = (pts["charge_current_a"] // 50).astype(int)
    pts["amb3"] = _amb3(pts["temperature_c"])
    tr = pts[pts["split"] == "TRAIN"]

    exp_cur = tr.groupby(["charger_id", "socb"])["charge_current_a"].median().rename("expCur").reset_index()
    exp_t2 = tr.groupby(["charger_id", "curb"])["max_temperature_c"].median().rename("expT2").reset_index()
    exp_t3 = tr.groupby(["charger_id", "curb", "amb3"], observed=True)["max_temperature_c"] \
        .agg(["median", "size"]).rename(columns={"median": "expT3", "size": "nT3"}).reset_index()
    exp_c2 = tr.groupby("charger_id")["celldiff"].median().rename("expC2").reset_index()
    exp_c3 = tr.groupby(["charger_id", "amb3"], observed=True)["celldiff"] \
        .agg(["median", "size"]).rename(columns={"median": "expC3", "size": "nC3"}).reset_index()
    pts = pts.merge(exp_cur, on=["charger_id", "socb"], how="left") \
             .merge(exp_t2, on=["charger_id", "curb"], how="left") \
             .merge(exp_t3, on=["charger_id", "curb", "amb3"], how="left") \
             .merge(exp_c2, on=["charger_id"], how="left") \
             .merge(exp_c3, on=["charger_id", "amb3"], how="left")
    n_t3_fillna = int(pts["expT3"].isna().sum())
    n_c3_fillna = int(pts["expC3"].isna().sum())

    pts["curRatio"] = pts["charge_current_a"] / pts["expCur"].clip(lower=1.0)
    pts["tempExcessW"] = pts["max_temperature_c"] - pts["expT3"].fillna(pts["expT2"])
    pts["tempExcessRaw"] = pts["max_temperature_c"] - pts["expT2"]  # v4 口径,对照诊断
    pts["cellExcessW"] = pts["celldiff"] - pts["expC3"].fillna(pts["expC2"])
    # 选型对照(一次性在建表时算,V1 点级 g):修正结构三选一不靠 TEST,靠跨窗虚抬机制指标
    pts["ambb"] = (pts["temperature_c"] // AMBIENT_BIN_C).astype("Int64")
    _tr = pts[pts["split"] == "TRAIN"]
    g1 = (_tr["max_temperature_c"] - _tr["expT2"]).groupby(_tr["ambb"], dropna=True).median()
    lo1, hi1 = int(g1.index.min()), int(g1.index.max())
    pts["tempExcessV1"] = pts["max_temperature_c"] - pts["expT2"] \
        - pts["ambb"].clip(lo1, hi1).map(g1).fillna(0.0)
    smax = pts.groupby("session_id")[["tempExcessRaw", "tempExcessV1", "tempExcessW"]].max()
    smax["split"] = pts.groupby("session_id")["split"].first()
    smeans = smax.groupby("split").mean().round(4)
    study = {tag: {"trainMean": round(float(smeans[col]["TRAIN"]), 3),
                  "testMean": round(float(smeans[col]["TEST"]), 3),
                  "inflationTestMinusTrain": round(float(smeans[col]["TEST"] - smeans[col]["TRAIN"]), 3)}
             for col, tag in (("tempExcessRaw", "v4_noAdjust"), ("tempExcessV1", "V1_pointG"),
                              ("tempExcessW", "V2_triple_chosen"))}
    g = pts.groupby("session_id", sort=False)
    out = pd.DataFrame({
        "tempW": g["tempExcessW"].max(),
        "tempExcessRaw": g["tempExcessRaw"].max(),
        "cellW": g["cellExcessW"].max(),
        "curDeficit": -g["curRatio"].min(),
    })
    meta = {"table": "charger×curb×amb3(T3) / charger×amb3(C3),fallback T2/C2",
            "amb3Edges": ["-inf", 10.0, 22.0, "inf"],  # 字符串钉,免得 ±inf 流进 json.dump 写出非法 JSON
            "t3Cells": int(len(exp_t3)), "c3Cells": int(len(exp_c3)),
            "t3MinCellN": int(exp_t3["nT3"].min()), "c3MinCellN": int(exp_c3["nC3"].min()),
            "fillnaT3Points": n_t3_fillna, "fillnaC3Points": n_c3_fillna,
            "adjustmentStudy": study,
            "ambientMeanBySplit": pts.groupby("split")["temperature_c"].mean().round(2).to_dict()}
    common.WEATHER_CTX_TABLE.parent.mkdir(parents=True, exist_ok=True)
    pd.to_pickle({"meta": meta}, common.WEATHER_CTX_TABLE.with_name("context_meta_v5.pkl"))
    out.to_pickle(common.WEATHER_CTX_TABLE)
    return out


def robust_z(sig: pd.Series, tr_sig: pd.Series) -> tuple[pd.Series, tuple[float, float]]:
    """TRAIN 定死的稳健标准化:med/MAD 只来自 TRAIN,之后任何窗口原样套用(固定决策函数)。
    缺信号会话 z=0(=训练中等行为),缺失数披露;不用 fillna(0) 钉分位秩底那种 transductive 手法。"""
    med = float(tr_sig.median())
    mad = float((tr_sig - med).abs().median())
    z = ((sig - med) / (1.4826 * mad + EPS)).clip(-10, 10).fillna(0.0)
    return z, (med, mad)


def main() -> int:
    started = time.time()
    pts = pd.read_pickle(common.POINT_TABLE)
    labels = common.load_table("anomaly_labels")
    ses = pts.groupby("session_id", sort=False).agg(split=("split", "first")).reset_index()
    ses["y"] = ses.session_id.isin(set(labels.session_id)).astype(int)
    sid_by_split = {s: ses[ses.split == s].session_id for s in SPLITS}
    y_by_split = {s: ses[ses.split == s].y.to_numpy() for s in SPLITS}

    ctx = weather_signals()
    v3 = session_signals()
    pool = pd.DataFrame(index=pd.Index(ses.session_id))
    for k in ("tempW", "cellW", "curDeficit"):
        pool[k] = ctx[k]
    pool["thermalSpike"] = v3["thermalSpike"]
    pool["currentJump"] = v3["currentJump"]
    pool["v1Mean"] = v3["v1Mean"]
    tr_idx = sid_by_split["TRAIN"]
    z_built = {k: robust_z(pool[k], pool.loc[tr_idx, k]) for k in POOL}
    zs = pd.DataFrame({k: v[0] for k, v in z_built.items()})
    z_const = {k: [round(v[1][0], 4), round(v[1][1], 4)] for k, v in z_built.items()}
    missing = {k: int(pool[k].isna().sum()) for k in POOL}

    # 诊断:天气修正前后,各 split 会话分均值(v4 口径 vs v5)——"全面化"的证据链
    diag_split = pd.DataFrame({"raw": ctx["tempExcessRaw"], "adj": ctx["tempW"]}) \
        .join(ses.set_index("session_id")[["split", "y"]])
    inflation = diag_split.groupby("split")[["raw", "adj"]].mean().round(3).to_dict("index")

    best = None
    grid = [r for n in range(1, len(POOL) + 1) for r in itertools.combinations(POOL, n)]
    for subset in grid:
        comp = zs[list(subset)].mean(axis=1)
        comp_by = {s: comp.reindex(sid_by_split[s]).to_numpy() for s in SPLITS}
        for pct in PERCENTILE_GRID_V5:
            thr = float(np.percentile(comp_by["TRAIN"], pct))
            entry = common.prf(comp_by["VALIDATION"], y_by_split["VALIDATION"], thr)
            key = (entry["f1"], -entry["fp"], -len(subset))  # F1 平手取误报少、结构简的
            if best is None or key > best[0]:
                best = (key, subset, pct, thr, entry)
    _, chosen, chosen_pct, threshold, val_entry = best
    comp = zs[list(chosen)].mean(axis=1)
    comp_by = {s: comp.reindex(sid_by_split[s]).to_numpy() for s in SPLITS}
    flag_rates = {s: round(float((comp_by[s] > threshold).mean()), 4) for s in SPLITS}

    metrics_test = common.prf(comp_by["TEST"], y_by_split["TEST"], threshold)
    test_ses = ses[ses.split == "TEST"]
    flags = comp_by["TEST"] > threshold
    types = labels.groupby("session_id")["anomaly_type"].agg(list)
    tl = [types.get(sid) for sid in test_ses.session_id]
    per_type = {}
    for atype in sorted(labels.anomaly_type.unique()):
        mask = np.array([isinstance(t, list) and atype in t for t in tl])
        n = int(mask.sum())
        per_type[atype] = {"n": n, "recall": round(int((mask & flags).sum()) / n, 4) if n else None}

    # 随机对照:同样告警预算下 VAL 随机打分的 F1 均值(阈值精确取第 n_flag 大,修掉 v2 时代 off-by-one)
    rng = np.random.default_rng(SEED)
    n_flag = int(val_entry["flagged"])
    reps = []
    for _ in range(20):
        r = rng.random(len(y_by_split["VALIDATION"]))
        idx = min(n_flag, len(r) - 1)
        thr_r = np.sort(r)[::-1][idx]
        reps.append(common.prf(r, y_by_split["VALIDATION"], float(thr_r))["f1"])
    rnd = {"randomF1Mean": round(float(np.mean(reps)), 4), "flagBudget": n_flag}

    amb_meta = pd.read_pickle(common.WEATHER_CTX_TABLE.with_name("context_meta_v5.pkl"))["meta"]
    lineage = {}
    for key, path in (("v1", common.TEST_METRICS_PATH), ("v2", common.TEST_METRICS_PATH_V2),
                      ("v3", common.TEST_METRICS_PATH_V3), ("v4", common.TEST_METRICS_PATH_V4)):
        lineage[key + "TestF1"] = json.loads(path.read_text(encoding="utf-8"))["test"]["f1"]

    with open(common.TRAIN_METRICS_PATH_V5, "w", encoding="utf-8") as handle:
        json.dump({"pool": list(POOL), "missingSignalCounts": missing,
                   "chosenSignals": list(chosen), "chosenPercentile": chosen_pct,
                   "fixedThresholdOnTrainScale": round(threshold, 4),
                   "validation": val_entry, "flagRateBySplit": flag_rates,
                   "v4StyleTempExcessInflationBySplit": inflation,
                   "weatherMeta": amb_meta,
                   "thresholdSemantics": "固定阈值 = TRAIN 融合分 p{:.1f};全窗口同一决策函数,TEST 告警率是部署结果".format(chosen_pct),
                   "selectionSpace": {"subsets": len(grid), "percentiles": list(PERCENTILE_GRID_V5)},
                   "seconds": round(time.time() - started, 1)}, handle, ensure_ascii=False, indent=2)

    joblib.dump({"signals": list(chosen), "percentile": chosen_pct, "threshold": threshold,
                 "z_med_mad": z_const,
                 "model_id": common.MODEL_ID_V5, "model_version": common.MODEL_VERSION_V5,
                 "note": "重算入口 detect_v5;天气表已观测、期望表 TRAIN-only",
                 "metadata": {"seed": SEED, "dependencies": {"pandas": pd.__version__, "numpy": np.__version__},
                              "trainedAt": pd.Timestamp.utcnow().isoformat()}},
                common.BUNDLE_PATH_V5)

    report = {
        "modelId": common.MODEL_ID_V5, "datasetId": common.DATASET_ID,
        "note": "气象上下文+固定阈值;标签只在评测关联。TEST 首盲一次,重跑要求语义等价复用",
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "chosenSignals": list(chosen), "chosenPercentile": chosen_pct,
        "validation": val_entry, "randomRef": rnd,
        "test": metrics_test, "testRecallByType": per_type, "lineage": lineage,
        "positives": int(ses.y.sum()), "sessions": int(len(ses)),
    }
    common.write_new_json(common.TEST_METRICS_PATH_V5, report)
    print(json.dumps({"chosen": list(chosen), "pct": chosen_pct, "threshold": round(threshold, 3),
                      "validation": val_entry, "flagRate": flag_rates,
                      "inflation(v4raw vs v5adj means)": inflation,
                      "test": metrics_test, "recallByType": per_type,
                      "lineage": lineage}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
