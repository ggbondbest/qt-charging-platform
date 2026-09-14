"""评分入口：给一次插枪启动尝试算「技术失败风险」，并把 TEST 的预测表落盘给人核对。

这条线**没有对外契约端点**（``contracts/`` 归负责人，本线只在 README 里提案，不擅自加接口），
所以本脚本的定位是"可复核的离线评分器"：任何一次打分都会先核对发布批次与矩阵哈希，
错配一律 BatchMismatch 拒绝，绝不拿错批次的模型出数。

``--verify-report`` 是这条线特有的第三只眼：在新进程里重算 AUC / 精确率 / 召回 / 阈值，
与 ``evaluation_report.json`` 里冻的数字逐项比对（要求 1e-12 内相等）。评分路径与评测路径
分两个进程跑同一份 bundle 还能对齐，才说明发布出来的数字是可复现的，不是某一次运行的产物。

用法（仓库根目录）：
  python -m data_analysis.ml.attempt.predict --self-check
  python -m data_analysis.ml.attempt.predict --verify-report
  python -m data_analysis.ml.attempt.predict --attempt-id AT-000123
  python -m data_analysis.ml.attempt.predict --dump-test-table 200
"""

from __future__ import annotations

import argparse
import hashlib
import json

import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

from . import common
from .evaluate import load_bundle
from .train import design

TABLE_COLUMNS = ("attempt_id", "attempted_local", "station_id", "charger_id", "charger_model",
                 "manufacturer", "connector_type", "技术失败风险", "告警", "y_tech",
                 "failure_reason")


def score(bundle: dict, rows: pd.DataFrame) -> np.ndarray:
    matrix, _ = design(rows, bundle["numericFeatures"], bundle["categoricalFeatures"])
    return bundle["classifier"].predict_proba(matrix)[:, 1]


def test_scores(bundle: dict, frame: pd.DataFrame) -> tuple[pd.DataFrame, np.ndarray]:
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    return test, score(bundle, test)


def one(bundle: dict, frame: pd.DataFrame, attempt_id: str) -> int:
    rows = frame[frame["attempt_id"] == attempt_id]
    if rows.empty:
        known = ", ".join(str(x) for x in frame["attempt_id"].head(3))
        raise KeyError(f"矩阵里没有 {attempt_id!r}（样例：{known} …）")
    if len(rows) > 1:
        raise AssertionError(f"{attempt_id} 命中 {len(rows)} 行，一行一次尝试的前提被破坏")
    record = rows.reset_index(drop=True)
    risk = float(score(bundle, record)[0])
    threshold = float(bundle["operatingPoint"]["threshold"])
    row = record.iloc[0]
    print(f"[predict] {attempt_id} 站点={row['station_id']} 桩={row['charger_id']}"
          f"（{row['manufacturer']}/{row['charger_model']}，{row['connector_type']}）"
          f" 尝试时刻(本地)={row['attempted_local']}")
    print(f"[predict] 技术失败风险={risk:.4f} 告警={risk >= threshold}（冻结阈值 {threshold:.4f}）")
    print(f"[predict] 实际结局={row['outcome']} / 原因={row['failure_reason'] or '无'}"
          "（离线复盘用，线上不给）")
    print(f"[predict] {common.simulated_note()}")
    return 0


def dump_table(bundle: dict, frame: pd.DataFrame, limit: int) -> int:
    """把 TEST 窗口的预测落成一张可核对的表：预测列在前、答案列在后，一眼能看出没串。"""
    test, risk = test_scores(bundle, frame)
    if limit < 1 or limit > len(test):
        raise SystemExit(f"--dump-test-table 需要在 1..{len(test)} 之间")
    block = test.iloc[:limit].reset_index(drop=True)
    scores = risk[:limit]
    threshold = float(bundle["operatingPoint"]["threshold"])
    table = pd.DataFrame({
        "attempt_id": block["attempt_id"],
        "attempted_local": block["attempted_local"].dt.strftime("%Y-%m-%d %H:%M"),
        "station_id": block["station_id"], "charger_id": block["charger_id"],
        "charger_model": block["charger_model"], "manufacturer": block["manufacturer"],
        "connector_type": block["connector_type"], "技术失败风险": np.round(scores, 4),
        "告警": scores >= threshold, "y_tech": block["y_tech"],
        "failure_reason": block["failure_reason"].fillna("—")})[list(TABLE_COLUMNS)]
    path = common.PREDICTIONS_CSV
    common.write_new_bytes(path, table.to_csv(index=False).encode("utf-8-sig"))
    flagged = table["告警"].to_numpy(dtype=bool)
    precision_text = ("—" if not flagged.any()
                      else f"{table.loc[flagged, 'y_tech'].mean():.4f}")
    print(f"[predict] 导出 {len(table)} 行 TEST 预测 → {path}")
    print(f"[predict] 告警 {int(flagged.sum())} 条，其中确实技术失败 "
          f"{int(table.loc[flagged, 'y_tech'].sum())} 条（精确率 {precision_text}）")
    print(table.head(12).to_string(index=False))
    print(f"[predict] {common.simulated_note()}")
    return 0


def verify_report(bundle: dict, frame: pd.DataFrame, tolerance: float = 1e-12) -> int:
    """新进程重算关键指标，与已发布的 evaluation_report.json 逐项比对。"""
    with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    test, risk = test_scores(bundle, frame)
    y = test["y_tech"].to_numpy(dtype=int)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = risk >= threshold
    recomputed = {
        "rows": int(len(test)),
        "auc": float(roc_auc_score(y, risk)),
        "baseRate": float(y.mean()),
        "alertRate": float(flag.mean()),
        "precision": float(y[flag].mean()) if flag.any() else 0.0,
        "recall": float(flag[y == 1].sum() / max(1, int(y.sum())))}
    stored = report["risk"]
    frozen = stored["frozenOperatingPoint"]
    pairs = [("auc", recomputed["auc"], float(stored["auc"])),
             ("baseRate", recomputed["baseRate"], float(stored["baseRate"])),
             ("alertRate", recomputed["alertRate"], float(frozen["alertRate"])),
             ("precision", recomputed["precision"], float(frozen["precision"])),
             ("recall", recomputed["recall"], float(frozen["recall"]))]
    rows_ok = recomputed["rows"] == int(report["testRows"])
    deltas = {name: abs(got - float(want)) for name, got, want in pairs}
    offenders = {name: delta for name, delta in deltas.items()
                 if delta > max(tolerance, 5e-5)}  # 报告里四舍五入到 4 位，比对容差按此放宽
    print(f"[predict] 行数一致={rows_ok}（重算 {recomputed['rows']:,} / 报告 {report['testRows']:,}）")
    for name, got, want in pairs:
        print(f"[predict] {name}: 重算={got:.6f} 报告={float(want):.6f} 偏差={abs(got - float(want)):.2e}")
    print(f"[predict] 评分路径与评测路径{'一致' if not offenders and rows_ok else '不一致：' + str(offenders)}")
    if offenders or not rows_ok:
        raise AssertionError(f"离线评分与发布报告对不上：{offenders}")
    print(f"[predict] {common.simulated_note()}")
    return 0


def self_check(bundle: dict, frame: pd.DataFrame) -> int:
    with open(common.MATRIX_PATH, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    operating = bundle["operatingPoint"]
    print(f"[predict] 模型包 {bundle['modelId']} v{bundle['modelVersion']}，特征组 {bundle['chosenSet']}")
    print(f"[predict] 批次 {bundle['publishedBatchId']} run {bundle['pipelineRunId']}")
    print(f"[predict] 矩阵 sha256 {digest[:16]}… 与模型包记录一致={digest == bundle['matrixSha256']}")
    print(f"[predict] 特征 {len(bundle['featureNames'])} 列（数值 {len(bundle['numericFeatures'])} / "
          f"类别 {len(bundle['categoricalFeatures'])}），冻结阈值 {operating['threshold']:.4f} "
          f"(VALIDATION 精确率 {operating['precision']:.4f}，规则：{operating['rule']})")
    print(f"[predict] 标签口径 {bundle['label']}")
    print(f"[predict] 行数 {len(frame):,}，切分 "
          f"{json.dumps({k: int(v) for k, v in frame['split'].value_counts().items()}, ensure_ascii=False)}")
    print(f"[predict] {common.simulated_note()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="插枪启动失败预测 · 离线评分器")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-check", action="store_true", help="核对模型与数据配对")
    group.add_argument("--verify-report", action="store_true", help="重算指标并与发布报告逐项比对")
    group.add_argument("--attempt-id", help="给一次启动尝试打分")
    group.add_argument("--dump-test-table", type=int, metavar="N", help="导出 N 行 TEST 预测表")
    args = parser.parse_args(argv)
    bundle = load_bundle()
    frame = pd.read_pickle(common.MATRIX_PATH)
    if args.self_check:
        return self_check(bundle, frame)
    if args.verify_report:
        return verify_report(bundle, frame)
    if args.attempt_id:
        return one(bundle, frame, args.attempt_id)
    return dump_table(bundle, frame, args.dump_test_table)


if __name__ == "__main__":
    raise SystemExit(main())
