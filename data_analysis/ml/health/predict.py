"""serve 侧自检 + 单桩查询 + 每日维护排程短名单。**发布包的一部分，不是训练脚本。**

三道门槛缺一不可：
  1. 批次/哈希自检：模型包与特征表必须成对（sha256 互相对上），绑定批次与数据层一致；
  2. 复算自检：对 TEST 全表重打一次分，与已发布 evaluation_report.json 里的 AUC 一致才放行
     ——产物被挪动或半更新过时，这里就会拦住；
  3. 部署资格自检：本脚本能打的分数只来自冻结 bundle 的分类/回归器；oracle 与任何名字里带
     NONDEPLOYABLE 的量一律拒绝（那是报告里的归因对照，不是服务）。

用法（仓库根目录）：
  python -m data_analysis.ml.health.predict --self-check
  python -m data_analysis.ml.health.predict --charger-id CHG_xxx --date 2026-05-10
  python -m data_analysis.ml.health.predict --schedule-day 2026-05-10
  python -m data_analysis.ml.health.predict --dump-test-table   # 独占写 test_predictions.csv
"""

from __future__ import annotations

import argparse
import json
import sys

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

from . import common
from .train import MAX_ALERT_SHARE, baseline_scores, design, usable_frame


def load_and_check() -> tuple[pd.DataFrame, dict]:
    """模型包 ↔ 特征表 ↔ 批次三方互验，任一不符直接抛错，不做"尽力而为"的降级。"""
    if not (common.BUNDLE_PATH.exists() and common.FEATURES_PATH.exists()):
        raise FileNotFoundError("模型包或特征表缺失：请先跑 features 再跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    digest = common.sha256_file(common.FEATURES_PATH)
    if summary.get("featuresSha256") != digest:
        raise common.BatchMismatch("特征表哈希与 features_summary 不一致（表被改过？）")
    if digest != bundle["featuresSha256"]:
        raise common.BatchMismatch("模型包与特征表不是同一轮产物，必须成对重发布")
    banned = [name for name in bundle["featureNames"]
              if "oracle" in name.lower() or "nond" in name.lower()]
    if banned:
        raise AssertionError(f"服务特征里混进了不可部署列 {banned}——oracle 只允许活在报告里")
    return pd.read_pickle(common.FEATURES_PATH), bundle


def score_frame(frame: pd.DataFrame, bundle: dict) -> tuple[np.ndarray, np.ndarray]:
    """按冻结 design 打分：概率 + 预计工单张数（截负）。缺列即抛，不静默补零。"""
    x, _ = design(frame, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x)[:, 1]
    tickets = np.clip(bundle["regressor"].predict(x), 0.0, None)
    return prob, tickets


def self_check(frame: pd.DataFrame, bundle: dict) -> dict:
    """复算 TEST 的 AUC 与已发布报告比对；同时复核基线表的口径没被挪动。"""
    if not common.TEST_REPORT_PATH.exists():
        raise FileNotFoundError("evaluation_report.json 缺失：盲测还没发布，自检无从比对")
    with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    test = usable_frame(frame, "TEST")
    prob, _ = score_frame(test, bundle)
    auc = float(roc_auc_score(test["y_ticket7"].to_numpy(dtype=int), prob))
    published = float(report["risk"]["auc"])
    if abs(auc - published) > 1e-4:
        raise AssertionError(f"复算 TEST AUC {auc:.6f} 与已发布 {published:.6f} 不一致，产物不同步")
    oracle_free = all(not key.endswith("NONDEPLOYABLE") or not block["deployable"]
                      for key, block in report["scoreboard"].items())
    prior = baseline_scores(test, bundle["baselines"], np.full(len(test), np.nan))
    return {"batch": bundle["publishedBatchId"] == common.EXPECTED_PUBLISHED_BATCH_ID,
            "pairHash": common.sha256_file(common.FEATURES_PATH) == bundle["featuresSha256"],
            "aucMatchesReport": True, "aucRecomputed": round(auc, 4),
            "aucPublished": round(published, 4),
            "servedFeatureCount": len(bundle["featureNames"]),
            "noNondeployableInServing": True, "reportKeepsNondeployableFlagged": oracle_free,
            "thresholdInSync": abs(bundle["operatingPoint"]["threshold"]
                                   - report["risk"]["frozenOperatingPoint"]["threshold"]) < 1e-5,
            "chargerPriorScoreRange": (float(np.nanmin(prior["asOfChargerPriorOnly"])),
                                       float(np.nanmax(prior["asOfChargerPriorOnly"])))}


def query_charger(frame: pd.DataFrame, bundle: dict, charger_id: str, date: str) -> None:
    panel = frame[(frame["charger_id"] == charger_id)
                  & (frame["business_date"] == pd.Timestamp(date))]
    if panel.empty:
        valid = frame.loc[frame["charger_id"] == charger_id, "business_date"]
        span = (f"该桩可打分日历 {valid.min().date()}..{valid.max().date()}" if len(valid)
                else "该桩不在 75 台在册清单里")
        raise SystemExit(f"查无此桩·日：{charger_id} @ {date}。{span}（特征只铺到工单数据右端）")
    prob, tickets = score_frame(panel, bundle)
    row = panel.iloc[0]
    threshold = float(bundle["operatingPoint"]["threshold"])
    label_state = ("标签未闭合（7日窗伸出数据右端，仅供打分）" if row["censored"] else
                   "标签完整" if not row["purged"] else "purge 行（训练/盲测都不用）")
    print(f"[predict] {charger_id} @ {row['business_date'].date()}  "
          f"P(未来7天来修)={prob[0]:.4f}  预计工单张数={tickets[0]:.2f}  "
          f"{'建议进维护窗口' if prob[0] >= threshold else '不进维护窗口'}"
          f"（冻结阈值 {threshold:.4f}）")
    print(f"[predict] 上下文：站 {row['station_id']}（{row['site_type']}）  "
          f"近90天来单 {row['tickets_90d'] if pd.notna(row['tickets_90d']) else '未起算'} 张  "
          f"桩级shrunk率 {row['charger_ticket_rate_shrunk']:.4f}  "
          f"距上次报修 {row['days_since_last_report'] if pd.notna(row['days_since_last_report']) else '∞'} 天")
    print(f"[predict] 行状态：{label_state}")
    print("[predict] " + common.data_note())


def schedule_day(frame: pd.DataFrame, bundle: dict, date: str) -> None:
    day = frame[frame["business_date"] == pd.Timestamp(date)]
    if day.empty:
        span = (frame["business_date"].min().date(), frame["business_date"].max().date())
        raise SystemExit(f"{date} 不在可打分日历 {span[0]}..{span[1]} 内")
    prob, tickets = score_frame(day, bundle)
    threshold = float(bundle["operatingPoint"]["threshold"])
    budget = int(np.ceil(MAX_ALERT_SHARE * len(day)))
    order = np.argsort(-prob, kind="stable")
    top = day.iloc[order[:budget]].assign(prob=prob[order[:budget]], expected=tickets[order[:budget]])
    above = int((prob >= threshold).sum())
    print(f"[schedule] {date}：在册 {len(day)} 台，预算 {budget} 个巡检名额，"
          f"冻结阈值命中 {above} 台（超预算时按分数取前 {budget} 台）")
    print(f"[schedule] {'桩ID':<14} {'站ID':<12} P(7日来修) 预计张数 距上次报修 近90天来单")
    for _, row in top.iterrows():
        gap = "∞" if pd.isna(row["days_since_last_report"]) else f"{row['days_since_last_report']:.0f}天"
        t90 = "—" if pd.isna(row["tickets_90d"]) else f"{row['tickets_90d']:.0f}"
        print(f"[schedule] {row['charger_id']:<14} {row['station_id']:<12} "
              f"{row['prob']:>8.4f} {row['expected']:>7.2f} {gap:>8} {t90:>8}")
    print("[schedule] " + common.data_note())


def dump_test_table(frame: pd.DataFrame, bundle: dict) -> None:
    """TEST 全表分数落 CSV（真实标签一起给——离线核对用，serve 上没有这个口径）。"""
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(
        common.FEATURES_PATH.name, common.BUILD_SUMMARY_PATH.name, common.BUNDLE_PATH.name,
        common.TRAIN_METRICS_PATH.name, common.TEST_REPORT_PATH.name,
        common.TEST_REPORT_MD.name))
    test = usable_frame(frame, "TEST")
    prob, tickets = score_frame(test, bundle)
    table = test[["charger_id", "station_id", "business_date", "y_ticket7",
                  "tickets_next7d"]].copy()
    table["prob_ticket7"] = np.round(prob, 6)
    table["expected_tickets"] = np.round(tickets, 4)
    table["flagged"] = prob >= float(bundle["operatingPoint"]["threshold"])
    common.write_new_csv(common.PREDICTIONS_CSV, table)
    print(f"[predict] {len(table):,} 行 -> {common.PREDICTIONS_CSV.name}（独占创建）")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="python -m data_analysis.ml.health.predict",
                                     description="桩-日来修预测 serve 自检与查询")
    parser.add_argument("--self-check", action="store_true", help="三方互验 + TEST 复算比对")
    parser.add_argument("--charger-id", help="单桩查询：桩 ID")
    parser.add_argument("--date", help="北京日历日 YYYY-MM-DD")
    parser.add_argument("--schedule-day", metavar="DATE", help="当日维护排程短名单")
    parser.add_argument("--dump-test-table", action="store_true", help="导出 TEST 分数表")
    args = parser.parse_args(argv)
    if not any([args.self_check, args.charger_id, args.schedule_day, args.dump_test_table]):
        parser.print_help()
        return
    frame, bundle = load_and_check()
    if args.self_check:
        result = self_check(frame, bundle)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if not all(value for key, value in result.items() if isinstance(value, bool)):
            raise SystemExit(1)
        print("[predict] 自检通过：批次/配对哈希/复算 AUC/不可部署列隔离 全部一致")
    if args.charger_id:
        if not args.date:
            raise SystemExit("--charger-id 需要配 --date YYYY-MM-DD")
        query_charger(frame, bundle, args.charger_id, args.date)
    if args.schedule_day:
        schedule_day(frame, bundle, args.schedule_day)
    if args.dump_test_table:
        dump_test_table(frame, bundle)


if __name__ == "__main__":
    main(sys.argv[1:])
