"""评分入口：给一次排队事件算「白排风险 + 预计等待」，并把 TEST 的预测表落盘给人核对。

这条线**没有对外契约端点**（`contracts/` 归负责人，本 PR 只在 README 里提案，不擅自加接口），
所以本脚本的定位是"可复核的离线评分器"：任何一次打分都会先核对发布批次与矩阵哈希，
错配一律 BatchMismatch 拒绝，绝不拿错批次的模型出数。

用法（仓库根目录）：
  python -m data_analysis.ml.queue.predict --self-check
  python -m data_analysis.ml.queue.predict --queue-id QE-000123
  python -m data_analysis.ml.queue.predict --dump-test-table 200
"""

from __future__ import annotations

import argparse
import hashlib
import json
import numpy as np
import pandas as pd

from . import common
from .evaluate import load_bundle
from .train import design

#: 展示与导出时的列顺序；`实际结局` 只在离线复盘表里出现，线上打分不给。
TABLE_COLUMNS = ("queue_id", "station_id", "joined_local", "position_at_join", "open_now",
                 "available_share", "白排风险", "告警", "预计等待分钟", "实际结局", "实际等待分钟")


def score(bundle: dict, row: pd.DataFrame) -> tuple[np.ndarray, np.ndarray]:
    matrix, _ = design(row, bundle["numericFeatures"], bundle["categoricalFeatures"])
    risk = bundle["classifier"].predict_proba(matrix)[:, 1]
    wait = np.clip(bundle["regressor"].predict(matrix), 0.0, None)
    return risk, wait


def one(bundle: dict, frame: pd.DataFrame, queue_id: str) -> int:
    rows = frame[frame["queue_id"] == queue_id]
    if rows.empty:
        known = ", ".join(str(x) for x in frame["queue_id"].head(3))
        raise KeyError(f"矩阵里没有 {queue_id!r}（样例：{known} …）")
    if len(rows) > 1:
        raise AssertionError(f"{queue_id} 命中 {len(rows)} 行，一行一次排队的前提被破坏")
    record = rows.reset_index(drop=True)
    risk, wait = score(bundle, record)
    threshold = float(bundle["operatingPoint"]["threshold"])
    row = record.iloc[0]
    print(f"[predict] {queue_id} 站点={row['station_id']} 排位={int(row['position_at_join'])} "
          f"加入时刻(本地)={row['joined_local']}")
    print(f"[predict] 白排风险={float(risk[0]):.4f} 告警={float(risk[0]) >= threshold}"
          f"（冻结阈值 {threshold:.4f}）预计等待={float(wait[0]):.1f} 分钟")
    print(f"[predict] 实际结局={row['outcome']} 实际等待="
          f"{'—' if pd.isna(row['wait_min']) else f'{row.wait_min:.0f} 分钟'}（离线复盘用，线上不给）")
    print(f"[predict] {common.simulated_note()}")
    return 0


def dump_table(bundle: dict, frame: pd.DataFrame, limit: int) -> int:
    """把 TEST 窗口的预测落成一张可核对的表：预测列在前，答案列在后，一眼能看出没串。"""
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    if limit < 1 or limit > len(test):
        raise SystemExit(f"--dump-test-table 需要在 1..{len(test)} 之间")
    block = test.iloc[:limit].reset_index(drop=True)
    risk, wait = score(bundle, block)
    threshold = float(bundle["operatingPoint"]["threshold"])
    table = pd.DataFrame({
        "queue_id": block["queue_id"], "station_id": block["station_id"],
        "joined_local": block["joined_local"].dt.strftime("%Y-%m-%d %H:%M"),
        "position_at_join": block["position_at_join"], "open_now": block["open_now"],
        "available_share": block["available_share"].round(3),
        "白排风险": np.round(risk, 4), "告警": risk >= threshold,
        "预计等待分钟": np.round(wait, 1), "实际结局": block["outcome"],
        "实际等待分钟": block["wait_min"].round(0),
    })[list(TABLE_COLUMNS)]
    path = common.OUT_DIR / "test_predictions.csv"
    common.write_new_bytes(path, table.to_csv(index=False).encode("utf-8-sig"))
    print(f"[predict] 导出 {len(table)} 行 TEST 预测 → {path}")
    print(f"[predict] 告警 {int(table['告警'].sum())} 条，其中确实白排 "
          f"{int(table.loc[table['告警'], '实际结局'].ne('SERVED').sum())} 条")
    print(table.head(12).to_string(index=False))
    print(f"[predict] {common.simulated_note()}")
    return 0


def self_check(bundle: dict, frame: pd.DataFrame) -> int:
    with open(common.MATRIX_PATH, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    print(f"[predict] 模型包 {bundle['modelId']}+{bundle['waitModelId']} v{bundle['modelVersion']}")
    print(f"[predict] 批次 {bundle['publishedBatchId']} run {bundle['pipelineRunId']}")
    print(f"[predict] 矩阵 sha256 {digest[:16]}… 与模型包记录一致="
          f"{digest == bundle['matrixSha256']}")
    print(f"[predict] 特征 {len(bundle['featureNames'])} 列（数值 "
          f"{len(bundle['numericFeatures'])} / 类别 {len(bundle['categoricalFeatures'])}）"
          f"冻结阈值 {bundle['operatingPoint']['threshold']:.4f} "
          f"(VALIDATION 精确率 {bundle['operatingPoint']['precision']:.4f})")
    print(f"[predict] 行数 {len(frame):,}，切分 "
          f"{json.dumps({k: int(v) for k, v in frame['split'].value_counts().items()}, ensure_ascii=False)}")
    print(f"[predict] {common.simulated_note()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="排队结果与等待预测 · 离线评分器")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-check", action="store_true", help="核对模型与数据配对")
    group.add_argument("--queue-id", help="给某一次排队事件打分")
    group.add_argument("--dump-test-table", type=int, metavar="N", help="导出 N 行 TEST 预测表")
    args = parser.parse_args(argv)
    bundle = load_bundle()
    frame = pd.read_pickle(common.MATRIX_PATH)
    if args.self_check:
        return self_check(bundle, frame)
    if args.queue_id:
        return one(bundle, frame, args.queue_id)
    return dump_table(bundle, frame, args.dump_test_table)


if __name__ == "__main__":
    raise SystemExit(main())
