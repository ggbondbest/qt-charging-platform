"""离线评分器：给"某台桩的明天"算可靠性风险与预计失败次数，并复核已发布报告。

这条线**没有对外契约端点**（``contracts/`` 归负责人，本线只在 README 里提案，不擅自加接口），
所以本脚本的定位是"可复核的离线评分器/巡检排程生成器"：任何一次打分都会先核对发布批次、
派生数据集哈希与特征表哈希，错配一律拒算——绝不用错批次的模型出数。

四个入口：
  · ``--self-check``：核对模型与数据配对、冻结阈值、标签口径；
  · ``--verify-report``：新进程里重算 TEST 上的 AUC / 精确率 / 召回 / Top-N 召回，与
    ``evaluation_report.json`` 逐项比对（报告四舍五入到 4 位，比对容差按此放宽）。
    评分路径与评测路径分两个进程跑同一份 bundle 还能对齐，才说明发布数字可复现；
  · ``--charger-id ... --date ...``：给单台桩的某个北京日历日打分（日期必须是决策日，
    即用"那天之前"的特征；表里没有就拒绝，不外推）；
  · ``--inspect-day ... --budget 0.15``：给某一天生成 Top-N 巡检清单（运营真会用的产物）；
  · ``--dump-test-table N``：导出 N 行 TEST 打分表核对（预测列在前、答案列在后）。

用法（仓库根目录）：
  python -m data_analysis.ml.reliability.predict --self-check
  python -m data_analysis.ml.reliability.predict --verify-report
  python -m data_analysis.ml.reliability.predict --charger-id CH-BJ-04-1 --date 2026-05-20
  python -m data_analysis.ml.reliability.predict --inspect-day 2026-05-20 --budget 0.15
  python -m data_analysis.ml.reliability.predict --dump-test-table 40
"""

from __future__ import annotations

import argparse
import json

import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

from . import common
from .evaluate import load_bundle
from .train import design


def score(bundle: dict, rows: pd.DataFrame) -> tuple[np.ndarray, np.ndarray]:
    """返回 (失败日概率, 预计失败次数)。两列同一次前向，不重复构特征。"""
    matrix, _ = design(rows, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(matrix)[:, 1]
    count = np.clip(bundle["regressor"].predict(matrix), 0.0, None)
    return prob, count


def one(bundle: dict, frame: pd.DataFrame, charger_id: str, day: str) -> int:
    stamp = pd.Timestamp(day)
    rows = frame[(frame["charger_id"] == charger_id) & (frame["business_date"] == stamp)]
    if rows.empty:
        known = frame.loc[frame["charger_id"] == charger_id, "business_date"]
        raise KeyError(f"特征表里没有 {charger_id} @ {day}"
                       + (f"（该桩在表内的日期范围 {known.min().date()}–{known.max().date()}）"
                          if len(known) else f"（表里没有这台桩，共 {frame['charger_id'].nunique()} 台）"))
    if len(rows) > 1:
        raise AssertionError(f"{charger_id} @ {day} 命中 {len(rows)} 行，一桩一天的前提被破坏")
    record = rows.reset_index(drop=True)
    prob, count = score(bundle, record)
    row = record.iloc[0]
    threshold = float(bundle["operatingPoint"]["threshold"])
    print(f"[predict] {charger_id} @ {stamp.date()} 站点={row['station_id']} "
          f"型号={row['manufacturer']}/{row['charger_model']}（{row['connector_type']}）")
    print(f"[predict] 次日失败日概率={prob[0]:.4f} 预计失败次数={count[0]:.3f} "
          f"告警={bool(prob[0] >= threshold)}（冻结阈值 {threshold:.4f}）")
    print(f"[predict] 昨天是否坏={int(row['prev_day_fail']) if pd.notna(row['prev_day_fail']) else '未知'} "
          f"此前连坏天数={int(row['fail_streak_before'])} "
          f"因果日均用量={row['attempts_per_day_30d']:.2f} "
          f"桩级失败日先验={row['charger_fail_rate_prior']:.4f}")
    if row["split"] != "EXCLUDED":
        print(f"[predict] 实际结局={int(row['y_fail'])}（当日技术失败 {int(row['tech_fails_on_day'])} 次）"
              f"——split={row['split']}，离线复盘用，线上不给")
    else:
        print("[predict] 该行不在评测窗内，无结局可比对")
    print(f"[predict] {common.data_note()}")
    return 0


def inspect_day(bundle: dict, frame: pd.DataFrame, day: str, budget: float) -> int:
    """生成某天的巡检清单：按概率降序取当日在用桩数的前 budget 比例。"""
    stamp = pd.Timestamp(day)
    rows = frame[frame["business_date"] == stamp].reset_index(drop=True)
    if rows.empty:
        raise KeyError(f"{day} 这一天在特征表里没有任何桩日")
    prob, count = score(bundle, rows)
    k = max(1, int(np.ceil(budget * len(rows))))
    order = np.argsort(-prob, kind="stable")[:k]
    table = rows.iloc[order].copy()
    table.insert(0, "巡检顺位", np.arange(1, len(table) + 1))
    table["次日失败日概率"] = np.round(prob[order], 4)
    table["预计失败次数"] = np.round(count[order], 3)
    threshold = float(bundle["operatingPoint"]["threshold"])
    table["超过冻结阈值"] = prob[order] >= threshold
    columns = ["巡检顺位", "charger_id", "station_id", "manufacturer", "charger_model",
               "次日失败日概率", "预计失败次数", "超过冻结阈值", "prev_day_fail", "fail_streak_before",
               "attempts_per_day_30d", "split", "y_fail", "tech_fails_on_day"]
    table = table[columns]
    print(f"[predict] {day} 当日在用桩 {len(rows)} 台，预算 {budget:.0%} → 巡检 {k} 台")
    print(table.to_string(index=False))
    labelled = rows["split"].iloc[0] != "EXCLUDED"
    if labelled:
        hits = int(table["y_fail"].sum())
        total = int(rows["y_fail"].sum())
        print(f"[predict] 清单里实际发生技术失败的桩日 {hits}/{k}（精确率 {hits / max(1, k):.4f}）；"
              f"当日全部失败日 {total} 个，本清单召回 {hits / max(1, total):.1%}")
    else:
        print("[predict] 该日不在标注窗内（EXCLUDED），不报命中与召回")
    print(f"[predict] 这是离线排程清单，不是线上接口；{common.data_note()}")
    return 0


def dump_table(bundle: dict, frame: pd.DataFrame, limit: int) -> int:
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    if limit < 1 or limit > len(test):
        raise SystemExit(f"--dump-test-table 需要在 1..{len(test)} 之间")
    block = test.iloc[:limit].reset_index(drop=True)
    prob, count = score(bundle, block)
    threshold = float(bundle["operatingPoint"]["threshold"])
    table = pd.DataFrame({
        "business_date": block["business_date"].dt.strftime("%Y-%m-%d"),
        "charger_id": block["charger_id"], "station_id": block["station_id"],
        "manufacturer": block["manufacturer"], "charger_model": block["charger_model"],
        "次日失败日概率": np.round(prob, 4), "预计失败次数": np.round(count, 3),
        "告警": prob >= threshold, "y_fail": block["y_fail"],
        "tech_fails_on_day": block["tech_fails_on_day"],
        "attempts_on_day": block["attempts_on_day"]})
    common.write_new_bytes(common.PREDICTIONS_CSV, table.to_csv(index=False).encode("utf-8-sig"))
    flagged = table["告警"].to_numpy(dtype=bool)
    precision = "—" if not flagged.any() else f"{table.loc[flagged, 'y_fail'].mean():.4f}"
    print(f"[predict] 导出 {len(table)} 行 TEST 打分 → {common.PREDICTIONS_CSV}")
    print(f"[predict] 告警 {int(flagged.sum())} 条，其中确实为失败日 "
          f"{int(table.loc[flagged, 'y_fail'].sum())} 条（精确率 {precision}）")
    print(table.head(12).to_string(index=False))
    print(f"[predict] {common.data_note()}")
    return 0


def top_n_recall(test: pd.DataFrame, prob: np.ndarray, y: np.ndarray, budget: float) -> dict:
    """按天取前 budget 比例的召回/精确率——verify 时要能重算出报告里的同一格。"""
    days = test["business_date"].to_numpy()
    hits = alerts = 0
    for day in pd.unique(days):
        mask = days == day
        k = max(1, int(np.ceil(budget * int(mask.sum()))))
        index = np.flatnonzero(mask)[np.argsort(-prob[mask], kind="stable")[:k]]
        hits += int(y[index].sum())
        alerts += len(index)
    return {"recall": hits / max(1, int(y.sum())), "precision": hits / max(1, alerts), "alerts": alerts}


def verify_report(bundle: dict, frame: pd.DataFrame) -> int:
    """新进程重算关键指标，与已发布的 evaluation_report.json 逐项比对。"""
    with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    y = test["y_fail"].to_numpy(dtype=int)
    prob, _ = score(bundle, test)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold
    top = top_n_recall(test, prob, y, budget=0.15)
    stored_point = report["risk"]["frozenOperatingPoint"]
    stored_budget = next(row for row in report["budgetRanking"] if abs(row["budget"] - 0.15) < 1e-9)
    pairs = [("auc", float(roc_auc_score(y, prob)), float(report["risk"]["auc"])),
             ("baseRate", float(y.mean()), float(report["risk"]["baseRate"])),
             ("alertRate", float(flag.mean()), float(stored_point["alertRate"])),
             ("precision", float(y[flag].mean()) if flag.any() else 0.0, float(stored_point["precision"])),
             ("recall", float(flag[y == 1].sum() / max(1, int(y.sum()))), float(stored_point["recall"])),
             ("top15Recall", top["recall"], float(stored_budget["model"]["recall"])),
             ("top15Precision", top["precision"], float(stored_budget["model"]["precision"]))]
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
    print(f"[predict] 模型包 {bundle['modelId']} v{bundle['modelVersion']}，特征组 {bundle['chosenSet']}")
    print(f"[predict] 批次 {bundle['publishedBatchId']} run {bundle['pipelineRunId']}")
    print(f"[predict] 派生集 {bundle['derivedDatasetId']}（逐文件 sha256 已在载入时复查）")
    print(f"[predict] 特征表 sha256 {digest[:16]}… 与模型包记录一致={digest == bundle['featuresSha256']}")
    print(f"[predict] 特征 {len(bundle['featureNames'])} 列（数值 {len(bundle['numericFeatures'])} / "
          f"类别 {len(bundle['categoricalFeatures'])}），冻结阈值 {operating['threshold']:.4f}"
          f"（VALIDATION 精确率 {operating['precision']:.4f}，规则：{operating['rule']}）")
    print(f"[predict] 标签口径 {bundle['label']}")
    print(f"[predict] 行数 {len(frame):,}，切分 "
          f"{json.dumps({k: int(v) for k, v in frame['split'].value_counts().items()}, ensure_ascii=False)}")
    print("[predict] 本线无对外契约端点；线上接入需负责人批准，详见 README 的提案段")
    print(f"[predict] {common.data_note()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="桩级次日可靠性预警 · 离线评分器 / 巡检排程")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-check", action="store_true", help="核对模型与数据配对")
    group.add_argument("--verify-report", action="store_true", help="重算指标并与发布报告逐项比对")
    group.add_argument("--charger-id", help="给某台桩的某一天打分（配合 --date）")
    group.add_argument("--inspect-day", metavar="YYYY-MM-DD", help="生成某一天的 Top-N 巡检清单")
    group.add_argument("--dump-test-table", type=int, metavar="N", help="导出 N 行 TEST 打分表")
    parser.add_argument("--date", default=None, metavar="YYYY-MM-DD", help="与 --charger-id 配对的北京日历日")
    parser.add_argument("--budget", type=float, default=0.15, help="每日巡检预算比例（默认 15%）")
    args = parser.parse_args(argv)
    bundle = load_bundle()
    frame = pd.read_pickle(common.FEATURES_PATH)
    if args.self_check:
        return self_check(bundle, frame)
    if args.verify_report:
        return verify_report(bundle, frame)
    if args.charger_id:
        if not args.date:
            raise SystemExit("--charger-id 需要配 --date YYYY-MM-DD")
        return one(bundle, frame, args.charger_id, args.date)
    if args.inspect_day:
        return inspect_day(bundle, frame, args.inspect_day, args.budget)
    return dump_table(bundle, frame, args.dump_test_table)


if __name__ == "__main__":
    raise SystemExit(main())
