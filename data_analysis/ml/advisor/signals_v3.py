"""v3 会话级信号计算器的"定义冻结"副本 —— advisor 自带,不再依赖队友包内符号。

来龙去脉:v5 冻结 bundle 的融合信号里 v1Mean/thermalSpike/currentJump 的定义
原先住在 anomaly/detect_v3.py;队友重构(94d1cc1)把该脚本退役删除,归因链
(explain_session 逐信号 z 复算、artifacts 告警复算)随之断裂,还把含绝对路径的
ImportError 文本一路带回桌宠正文(验收实锤的泄露一号根因)。
盲评纪律不许动冻结工件,那就把**一模一样的定义**(照 94d1cc1~1 版原样搬)
固化在 advisor 里:只读 outputs/ml_anomaly 的 v1 bundle 与特征缓存,永不写。

出处:git show 94d1cc1~1:data_analysis/ml/anomaly/detect_v3.py :: session_signals
"""
from __future__ import annotations

from functools import lru_cache

import joblib
import numpy as np
import pandas as pd

from . import config


@lru_cache(maxsize=1)
def session_signals() -> dict[str, pd.Series]:
    """7 个会话级信号中归因链要用的 3 列(其余列 v5 融合/展示都不碰)。索引 = session_id。"""
    b1 = joblib.load(config.ANOMALY_OUT / "iforest-session-battery-v1.joblib")
    sf = pd.read_pickle(config.ANOMALY_OUT / "session_features.pkl").set_index("session_id")
    x1 = b1["scaler"].transform(sf[b1["features"]].astype(float).fillna(b1["medians"]))
    out: dict[str, pd.Series] = {
        "v1Mean": pd.Series(-b1["model"].score_samples(x1), index=sf.index),
    }
    pts = pd.read_pickle(config.ANOMALY_OUT / "point_features_v2.pkl")
    heat = np.maximum(pts["z_temp_spread"].to_numpy(), pts["z_max_temperature_c"].to_numpy())
    out["thermalSpike"] = pd.Series(heat, index=pts.session_id.to_numpy()).groupby(level=0).max()
    out["currentJump"] = (pts.assign(ad=pts["dcur"].abs())
                          .groupby("session_id", sort=False)["ad"].max())
    return {k: v.reindex(sf.index) for k, v in out.items()}
