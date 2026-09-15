"""离线评分器：给"某一天开始的会话"算占桩超时概率与预计占桩分钟，并复核已发布报告。

这条线**没有对外契约端点**（``contracts/`` 归负责人，本线只在 README 里提案，不擅自加接口），
所以本脚本的定位是"可复核的离线评分器/移车提醒名单生成器"：任何一次打分都会先核对发布批次与
特征表/模型包哈希配对，错配一律拒算——绝不用错批次的模型出数。

与第五/六/七线不同，本线的打分对象是**一次会话**，决策时点 = ``started_at``：
给"今天开始充电的这些车"排移车提醒，按当日会话数的一定比例发。

四个入口：
  · ``--self-check``：核对模型与数据配对、冻结阈值、标签口径；
  · ``--verify-report``：新进程里重算 TEST 上的 AUC / 冻结点精确率 / 召回 / 每日预算 10% 档
    的命中，与 ``evaluation_report.json`` 逐项比对（报告四舍五入到 4 位，容差按此放宽）。
    评分路径与评测路径分两个进程跑同一份 bundle 还能对齐，才说明发布数字可复现；
  · ``--session-id ...``：给一场已入表的会话打单分（表里没有就拒绝，不外推）；
  · ``--remind-day ... --budget 0.15``：给某一天生成移车提醒名单（运营真会用的产物）；
  · ``--dump-test-table N``：导出 N 行 TEST 打分表核对（预测列在前、答案列在后）。

用法（仓库根目录）：
  python -m data_analysis.ml.overstay.predict --self-check
  python -m data_analysis.ml.overstay.predict --verify-report
  python -m data_analysis.ml.overstay.predict --session-id CS-XXXX
  python -m data_analysis.ml.overstay.predict --remind-day 2026-05-20 --budget 0.15
  python -m data_analysis.ml.overstay.predict --dump-test-table 40
"""

from __future__ import annotations

import argparse
import json

import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

from . import common
from .evaluate import load_bundle, reminder_budget
from .train import design


def score(bundle: dict, rows: pd.DataFrame) -> tuple[np.ndarray, np.ndarray]:
    """返回 (超时概率, 预计占桩分钟)。两列同一次前向，不重复构特征。"""
    matrix, _ = design(rows, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(matrix)[:, 1]
    minutes = np.clip(bundle["regressor"].predict(matrix), 0.0, None)
    return prob, minutes


def one(bundle: dict, frame: pd.DataFrame, session_id: str) -> int:
    rows = frame[frame["session_id"] == session_id]
    if rows.empty:
        raise KeyError(f"特征表里没有会话 {session_id!r}（共 {frame['session_id'].nunique():,} 场；"
                       f"只给 started_at 已入表的会话打分，未开始的会话请走 --remind-day）")
    if len(rows) > 1:
        raise AssertionError(f"{session_id} 命中 {len(rows)} 行，一场会话一行的前提被破坏")
    record = rows.reset_index(drop=True)
    prob, minutes = score(bundle, record)
    row = record.iloc[0]
    threshold = float(bundle["operatingPoint"]["threshold"])
    print(f"[predict] {session_id} 开始于 {row['started_at']}（北京 {row['started_at'] + pd.Timedelta(hours=8):%Y-%m-%d %H:%M}）"
          f" 站点={row['station_id']}（{row['site_type']}）{row['connector_type']}"
          f" 计划={row['planned_hours']:.2f}h")
    print(f"[predict] 占桩超时概率={prob[0]:.4f} 预计占桩分钟={minutes[0]:.1f} "
          f"发提醒={bool(prob[0] >= threshold)}（冻结阈值 {threshold:.4f}）")
    last = row["user_last_over"]
    print(f"[predict] 该用户此前会话数={int(row['user_sessions_prior'])} "
          f"因果超时率={row['user_over_rate_prior']:.4f} "
          f"上次是否超时={'无历史可看' if pd.isna(last) else int(last)} "
          f"站点超时率先验={row['station_over_rate_prior']:.4f}")
    if row["split"] != "EXCLUDED":
        print(f"[predict] 实际结局 y_over={int(row['y_over'])}（真实占桩 {row['over_min']:.1f} 分钟，"
          f"阈值 {common.OVERSTAY_THRESHOLD_MIN:.0f} 分钟）——split={row['split']}，离线复盘用，线上不给")
    else:
        print("[predict] 该行不在评测窗内，无结局可比对")
    print(f"[predict] {common.data_note()}")
    return 0


def remind_day(bundle: dict, frame: pd.DataFrame, day: str, budget: float) -> int:
    """生成某天的移车提醒名单：当日开始的会话按概率降序取前 budget 比例。"""
    stamp = pd.Timestamp(day)
    rows = frame[frame["business_date"] == stamp].reset_index(drop=True)
    if rows.empty:
        raise KeyError(f"{day} 这一天（北京日）在特征表里没有任何开始的会话")
    prob, minutes = score(bundle, rows)
    k = max(1, int(np.ceil(budget * len(rows))))
    order = np.argsort(-prob, kind="stable")[:k]
    table = rows.iloc[order].copy()
    table.insert(0, "提醒顺位", np.arange(1, len(table) + 1))
    table["占桩超时概率"] = np.round(prob[order], 4)
    table["预计占桩分钟"] = np.round(minutes[order], 1)
    threshold = float(bundle["operatingPoint"]["threshold"])
    table["超过冻结阈值"] = prob[order] >= threshold
    table = table[["提醒顺位", "session_id", "station_id", "site_type", "connector_type",
                   "planned_hours", "占桩超时概率", "预计占桩分钟", "超过冻结阈值",
                   "user_over_rate_prior", "queue_open_at_start", "split", "y_over", "over_min"]]
    print(f"[predict] {day} 当日开始会话 {len(rows):,} 场，预算 {budget:.0%} → 提醒 {k:,} 场")
    print(table.head(20).to_string(index=False))
    if len(table) > 20:
        print(f"[predict] …（其余 {len(table) - 20:,} 行略）")
    if table["split"].iloc[0] != "EXCLUDED":
        hits = int(table["y_over"].sum())
        total = int(rows["y_over"].sum())
        print(f"[predict] 名单里真实超时的会话 {hits}/{k}（精确率 {hits / max(1, k):.4f}）；"
              f"当日全部超时 {total} 场，本名单召回 {hits / max(1, total):.1%}")
    else:
        print("[predict] 该日不在标注窗内（EXCLUDED），不报命中与召回")
    print(f"[predict] 这是离线提醒名单，不是线上接口；{common.data_note()}")
    return 0


def dump_table(bundle: dict, frame: pd.DataFrame, limit: int) -> int:
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    if limit < 1 or limit > len(test):
        raise SystemExit(f"--dump-test-table 需要在 1..{len(test)} 之间")
    block = test.iloc[:limit].reset_index(drop=True)
    prob, minutes = score(bundle, block)
    threshold = float(bundle["operatingPoint"]["threshold"])
    table = pd.DataFrame({
        "business_date": block["business_date"].dt.strftime("%Y-%m-%d"),
        "session_id": block["session_id"], "station_id": block["station_id"],
        "connector_type": block["connector_type"], "site_type": block["site_type"],
        "planned_hours": np.round(block["planned_hours"].to_numpy(dtype=float), 3),
        "占桩超时概率": np.round(prob, 4), "预计占桩分钟": np.round(minutes, 1),
        "发提醒": prob >= threshold, "y_over": block["y_over"],
        "over_min": np.round(block["over_min"].to_numpy(dtype=float), 1)})
    common.write_new_bytes(common.PREDICTIONS_CSV, table.to_csv(index=False).encode("utf-8-sig"))
    flagged = table["发提醒"].to_numpy(dtype=bool)
    precision = "—" if not flagged.any() else f"{table.loc[flagged, 'y_over'].mean():.4f}"
    print(f"[predict] 导出 {len(table)} 行 TEST 打分 → {common.PREDICTIONS_CSV}")
    print(f"[predict] 告警 {int(flagged.sum())} 条，其中确实超时 {int(table.loc[flagged, 'y_over'].sum())} 条"
          f"（精确率 {precision}）")
    print(table.head(12).to_string(index=False))
    print(f"[predict] {common.data_note()}")
    return 0


def verify_report(bundle: dict, frame: pd.DataFrame) -> int:
    """新进程重算关键指标，与已发布的 evaluation_report.json 逐项比对。"""
    with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    y = test["y_over"].to_numpy(dtype=int)
    prob, _ = score(bundle, test)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold
    top10 = reminder_budget(test, {"model": prob}, y, budgets=(0.10,))[0]["model"]
    stored_top10 = next(row for row in report["budgetRanking"]
                        if abs(row["budget"] - 0.10) < 1e-9)["model"]
    point = report["risk"]["frozenOperatingPoint"]
    pairs = [("auc", float(roc_auc_score(y, prob)), float(report["risk"]["auc"])),
             ("baseRate", float(y.mean()), float(report["risk"]["baseRate"])),
             ("alertRate", float(flag.mean()), float(point["alertRate"])),
             ("alerts", float(flag.sum()), float(point["alerts"])),
             ("precision", float(y[flag].mean()) if flag.any() else 0.0, float(point["precision"])),
             ("recall", float(flag[y == 1].sum() / max(1, int(y.sum()))), float(point["recall"])),
             ("top10Recall", float(top10["recall"]), float(stored_top10["recall"])),
             ("top10Precision", float(top10["precision"]), float(stored_top10["precision"]))]
    rows_ok = len(test) == int(report["testRows"])
    print(f"[predict] 行数一致={rows_ok}（重算 {len(test):,} / 报告 {report['testRows']:,}）")
    offenders: dict[str, float] = {}
    for name, got, want in pairs:
        delta = abs(float(got) - float(want))
        if delta > 5e-5:  # 报告里四舍五入到 4 位，比对容差按此放宽
            offenders[name] = delta
        print(f"[predict] {name}: 重算={float(got):.6f} 报告={float(want):.6f} 偏差={delta:.2e}")
    print(f"[predict] 评分路径与评测路径{'一致' if not offenders and rows_ok else '不一致：' + str(offenders)}")
    if offenders or not rows_ok:
        raise AssertionError(f"离线评分与发布报告对不上：{offenders}")
    print(f"[predict] {common.data_note()}")
    return 0


def self_check(bundle: dict, frame: pd.DataFrame) -> int:
    digest = common.sha256_file(common.FEATURES_PATH)
    operating = bundle["operatingPoint"]
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary_meta = json.load(handle)
    print(f"[predict] 模型包 {bundle['modelId']} v{bundle['modelVersion']}，特征组 {bundle['chosenSet']}")
    print(f"[predict] 批次 {bundle['publishedBatchId']} run {bundle['pipelineRunId']}")
    print(f"[predict] 特征表 sha256 {digest[:16]}… 与模型包记录一致={digest == bundle['featuresSha256']}")
    print(f"[predict] 特征 {len(bundle['featureNames'])} 列（数值 {len(bundle['numericFeatures'])} / "
          f"类别 {len(bundle['categoricalFeatures'])}），冻结阈值 {operating['threshold']:.4f}"
          f"（VALIDATION 精确率 {operating['precision']:.4f}，规则：{operating['rule']}）")
    print(f"[predict] 标签口径 {bundle['label']}")
    print(f"[predict] 样本单元 {summary_meta['sampleUnit']}；决策时点 {summary_meta['decisionTime']}；"
          f"答案侧列（over_min/y_over/duration_min）只当标签与 oracle 对照，特征矩阵里一列都没有")
    audit = summary_meta["leakAudit"]
    print(f"[predict] 泄漏审计 {len(audit['checked'])} 个跨轴量 × {audit['sampled']} 行，最大偏差 "
          f"{max(audit['maxAbsDiff'].values())}")
    print(f"[predict] 行数 {len(frame):,}，切分 "
          f"{json.dumps({k: int(v) for k, v in frame['split'].value_counts().items()}, ensure_ascii=False)}")
    print("[predict] 本线无对外契约端点；线上接入需负责人批准，详见 README 的提案段")
    print(f"[predict] {common.data_note()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="会话占桩超时提醒 · 离线评分器 / 提醒名单生成器")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-check", action="store_true", help="核对模型与数据配对")
    group.add_argument("--verify-report", action="store_true", help="重算指标并与发布报告逐项比对")
    group.add_argument("--session-id", help="给一场已入表的会话打单分")
    group.add_argument("--remind-day", metavar="YYYY-MM-DD", help="生成某一天的移车提醒名单")
    group.add_argument("--dump-test-table", type=int, metavar="N", help="导出 N 行 TEST 打分表")
    parser.add_argument("--budget", type=float, default=0.15, help="每日提醒预算比例（默认 15%）")
    args = parser.parse_args(argv)
    bundle = load_bundle()
    frame = pd.read_pickle(common.FEATURES_PATH)
    if args.self_check:
        return self_check(bundle, frame)
    if args.verify_report:
        return verify_report(bundle, frame)
    if args.session_id:
        return one(bundle, frame, args.session_id)
    if args.remind_day:
        return remind_day(bundle, frame, args.remind_day, args.budget)
    return dump_table(bundle, frame, args.dump_test_table)


if __name__ == "__main__":
    raise SystemExit(main())
