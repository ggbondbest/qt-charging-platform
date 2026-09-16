"""参谋的"可查账本"生成器:告警清单 CSV + FTS5 知识索引。产物全在 outputs/ml_advisor/。

纪律:
- 只读冻结工件(bundle/信号缓存/clean 表),绝不写 outputs/ml_anomaly 等冻结区;
- 告警清单 = 模型输出侧(session/score/threshold),**不关联 anomaly_labels**——
  参谋看真值就是作弊,分型命中只能从冻结评测 json 的汇总字段读;
- 知识索引排除含逐会话真值的文件(anomaly_labels 相关),README/模型卡/评论/维修备注可进。
"""
from __future__ import annotations

import json
import sqlite3
from pathlib import Path

import joblib
import pandas as pd

from . import config


def export_alerts(force: bool = False) -> Path:
    """用 v5 冻结 bundle 的常数(med/MAD/阈值/所选信号)在缓存信号上复算融合分并列出告警。
    纯读+派生输出;TEST 段告警数应与冻结件 test.flagged=91 一致,打印对照仅作自检。"""
    if config.ALERTS_CSV.exists() and not force:
        print(f"复用已生成:{config.ALERTS_CSV}")
        return config.ALERTS_CSV
    from . import signals_v3  # 定义冻结副本,sklearn 链懒加载;不追队友包内符号
    try:
        b = joblib.load(config.ANOMALY_OUT / "context-weather-fixedthr-v5.joblib")
        ctx = pd.read_pickle(config.ANOMALY_OUT / "context_signals_v5.pkl")
    except FileNotFoundError as e:
        # 新克隆上没有冻结件(outputs/ml_anomaly 是 gitignored 的本地产物)——给明确出路
        raise FileNotFoundError(
            f"缺少 v5 冻结工件({config.ANOMALY_OUT}):需要 context-weather-fixedthr-v5.joblib "
            f"与 context_signals_v5.pkl。请先在异常检测线跑通 train/eval 生成,或向队友索取冻结件。"
        ) from e
    v3 = signals_v3.session_signals()
    allsig = {k: ctx[k] for k in ("tempW", "cellW", "curDeficit") if k in ctx.columns}
    allsig.update({k: v3[k] for k in ("thermalSpike", "currentJump", "v1Mean")})
    zs = []
    for name in b["signals"]:
        med, mad = b["z_med_mad"][name]
        zs.append(((allsig[name] - med) / (1.4826 * mad + 1e-9)).clip(-10, 10).fillna(0.0))
    comp = pd.concat(zs, axis=1).mean(axis=1).rename("score")
    pts = pd.read_pickle(config.ANOMALY_OUT / "point_features_v2.pkl")
    meta = pts.groupby("session_id").agg(charger_id=("charger_id", "first"),
                                         started_at=("started_at", "first"),
                                         split=("split", "first")).reset_index()
    chg = pd.concat([pd.read_parquet(p) for p in sorted((config.CLEAN_DIR / "chargers").glob("*.parquet"))])
    stn = pd.concat([pd.read_parquet(p) for p in sorted((config.CLEAN_DIR / "stations").glob("*.parquet"))])
    meta = meta.merge(chg[["charger_id", "station_id"]].drop_duplicates(), on="charger_id", how="left") \
               .merge(stn[["station_id", "city_id"]].drop_duplicates(), on="station_id", how="left")
    df = meta.merge(comp.rename("score"), left_on="session_id", right_index=True, how="left")
    df["threshold"] = b["threshold"]
    df["date"] = pd.to_datetime(df["started_at"]).dt.strftime("%Y-%m-%d")
    df = df[df["score"] > df["threshold"]].copy()
    config.OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = df[["session_id", "station_id", "charger_id", "city_id", "date", "split", "score", "threshold"]]
    out.to_csv(config.ALERTS_CSV, index=False, encoding="utf-8")
    counts = out.groupby("split").size().to_dict()
    print(f"告警清单 {len(out)} 条 → {config.ALERTS_CSV};按段:{counts}")
    frozen = config.ANOMALY_OUT / "test_metrics_context-weather-fixedthr-v5.json"
    if frozen.exists():
        exp = json.loads(frozen.read_text(encoding="utf-8"))["test"].get("flagged")
        got = counts.get("TEST")
        print(f"自检:TEST 段复算 {got} vs 冻结件 {exp} → {'一致' if got == exp else '不一致!!查因,勿动冻结件'}")
    return config.ALERTS_CSV


DOCS = ["ml/README.md", "ml/anomaly/README.md", "ml/churn/README.md", "ml/load/README.md",
        "ml/load/EVALUATION.md", "ml/FUNCTION_POOL.md", "ml/WORKLIST.md", "ml/advisor/README.md",
        "contracts/README.md"]


def _iter_files(root: Path):
    for rel in DOCS:
        p = root / rel
        if p.exists():
            yield p, rel


def _chunks(text: str, size: int = 900):
    head = ""
    for para in text.split("\n\n"):
        para = para.strip()
        if not para:
            continue
        if para.startswith("#"):
            head = para.splitlines()[0][:60]
        buf = para
        while buf:
            yield buf[:size]
            buf = buf[size:]


def _index_rows(db: Path) -> int:
    """库里有多少文档块;库不存在/被中途打断成半成品(空表、缺表)一律算 0。"""
    try:
        con = sqlite3.connect(db)
        try:
            return int(con.execute("SELECT COUNT(*) FROM docs").fetchone()[0])
        finally:
            con.close()
    except sqlite3.Error:
        return 0


def build_index(force: bool = False) -> Path:
    if config.KNOWLEDGE_DB.exists() and not force and _index_rows(config.KNOWLEDGE_DB) > 0:
        print(f"复用已生成:{config.KNOWLEDGE_DB}")
        return config.KNOWLEDGE_DB
    # 走到这里要么库不存在,要么 force,要么上次建到一半留下空表/坏库——
    # 三种都重建;直接 CREATE 会在已存在库上炸 "table docs already exists"(评审实锤)。
    from .tools import _seg
    con = sqlite3.connect(config.KNOWLEDGE_DB)
    con.execute("DROP TABLE IF EXISTS docs_fts")
    con.execute("DROP TABLE IF EXISTS docs")
    con.execute("CREATE TABLE docs(id INTEGER PRIMARY KEY, source TEXT, text TEXT)")
    con.execute("CREATE VIRTUAL TABLE docs_fts USING fts5(text, content=docs, content_rowid=id)")
    rows = []
    for p, rel in _iter_files(config.DA_ROOT):
        for ch in _chunks(p.read_text(encoding="utf-8", errors="ignore")):
            rows.append((f"data_analysis/{rel}", ch))
    # 评论与维修备注:非结构化"人话"语料(逐条截断,只取有文字的)
    try:
        rv = pd.concat([pd.read_parquet(p) for p in sorted((config.CLEAN_DIR / "reviews").glob("*.parquet"))])
        for r in rv.dropna(subset=["comment"]).itertuples():
            rows.append((f"clean/reviews:{r.review_id}@{r.station_id}", str(r.comment)[:220]))
        me = pd.concat([pd.read_parquet(p) for p in sorted((config.CLEAN_DIR / "maintenance_events").glob("*.parquet"))])
        for r in me.dropna(subset=["note"]).itertuples():
            rows.append((f"clean/maintenance_events:{r.event_id}", str(r.note)[:220]))
    except Exception as e:
        print(f"评论/备注语料跳过:{e}")
    con.executemany("INSERT INTO docs(source, text) VALUES(?, ?)", rows)
    con.executemany("INSERT INTO docs_fts(rowid, text) VALUES(?, ?)",
                    [(i + 1, _seg(t)) for i, (_, t) in enumerate(rows)])
    con.commit()
    n = con.execute("SELECT COUNT(*) FROM docs").fetchone()[0]
    con.close()
    print(f"知识索引 {n} 块 → {config.KNOWLEDGE_DB}")
    return config.KNOWLEDGE_DB


def prepare(force: bool = False) -> None:
    config.OUT_DIR.mkdir(parents=True, exist_ok=True)
    export_alerts(force=force)
    build_index(force=force)


if __name__ == "__main__":
    prepare(force=True)
