"""Prepare local data-cleaning acceptance evidence without modifying raw data.

Run from the repository root. This executes real Spark, not canned reports.
HDFS is verified separately against the teacher's actual Hadoop environment.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import sys


def local_roots(input_root, output_root):
    if "://" in str(input_root) or "://" in str(output_root):
        raise ValueError("Use local paths here; use pipeline/acceptance_analysis/export for HDFS")
    source_arg, root_arg = Path(input_root), Path(output_root)
    if source_arg.is_symlink() or root_arg.is_symlink():
        raise ValueError("Input/output roots must not be symlinks")
    source, root = source_arg.resolve(), root_arg.resolve()
    if not source.is_dir() or not (source / "manifest.json").is_file():
        raise ValueError("Input must be a dataset directory containing manifest.json")
    if root.exists():
        raise FileExistsError("Output already exists; choose a fresh run directory")
    if root.is_relative_to(source) or source.is_relative_to(root):
        raise ValueError("Input/output must be independent, non-nested directories")
    return source, root


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def validate_evidence(root, quality, analysis):
    """The success report references real, consistent evidence, not just counters."""
    root = Path(root)
    audit_path = root / "processed" / "reports" / "cleaning_audit.json"
    rules_path = root / "processed" / "reports" / "cleaning_rules.json"
    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    rules = json.loads(rules_path.read_text(encoding="utf-8"))
    rule_hash = hashlib.sha256(json.dumps(rules["rules"], ensure_ascii=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")).hexdigest()
    for key in ("dataset_id", "pipeline_run_id", "source_manifest_sha256"):
        if audit["lineage"].get(key) != quality.get(key):
            raise ValueError("Cleaning audit belongs to another batch")
    if (audit.get("rules_sha256") != rule_hash or rules.get("rules_sha256") != rule_hash or
            audit.get("rule_version") != rules.get("rule_version") or
            audit["row_conservation"].get("passed") is not True):
        raise ValueError("Cleaning evidence rules or row conservation mismatch")
    if (analysis.get("datasetId") != quality["dataset_id"] or
            analysis.get("pipelineRunId") != quality["pipeline_run_id"] or
            analysis.get("sourceManifestSha256") != quality["source_manifest_sha256"]):
        raise ValueError("Analysis evidence belongs to another batch")
    actual_dimensions = {dimension for item in analysis["analyses"].values() for dimension in item["dimensions"]}
    actual_comparisons = len({tuple(item["dimensions"]) for item in analysis["analyses"].values()
        if len(set(item["dimensions"])) >= 2})
    if (len(actual_dimensions) < 8 or actual_comparisons < 2 or
            len(actual_dimensions) != analysis.get("semanticDimensionCount") or
            actual_comparisons != analysis.get("comparisonCount")):
        raise ValueError("Analysis evidence does not meet the declared acceptance dimensions")
    for folder in (root / "processed", root / "analysis"):
        if not (folder / "_SUCCESS").is_file() or (folder / "_RUNNING").exists():
            raise ValueError("Evidence references an incomplete output")
    audit_hash = hashlib.sha256(audit_path.read_bytes()).hexdigest()
    summary = quality.get("cleaning_audit_summary", {})
    if (summary.get("audit_sha256") != audit_hash or
            summary.get("rules_file_sha256") != hashlib.sha256(rules_path.read_bytes()).hexdigest()):
        raise ValueError("Cleaning evidence file hashes differ from quality report")
    return {"ruleVersion": rules["rule_version"], "rulesSha256": rule_hash, "auditSha256": audit_hash,
        "semanticDimensions": len(actual_dimensions), "comparisons": actual_comparisons}


def report_markdown(report):
    quality = report["cleaning"]
    lines = ["# 数据清洗与准备验收记录", "",
        "本文件由实际运行结果生成。业务数据为模拟数据，不代表真实城市经营结论。", "",
        f"- 数据集：{report['datasetId']}",
        f"- Spark 批次：{report['pipelineRunId']}",
        f"- 查询发布批次：{report['publishedBatchId']}",
        f"- 原始表数：{quality['tableCount']}；原始总行数：{quality['inputRows']}",
        f"- 清洗后充电会话：{quality['cleanSessions']}；隔离及去重记录：{quality['rejectedSessions']}",
        f"- 会话行数守恒：{'通过' if quality['rowsConserved'] else '未通过'}", "",
        "## 展示顺序", "",
        "1. 原始数据 manifest.json、preview/ 与脏数据注入计划（若本轮启用）。",
        "2. processed/reports/cleaning_rules.json：处理规则、依据和边界。",
        "3. processed/reports/cleaning_audit.json：字段探查、六维质量指标、修正/隔离样例和前后对比。",
        "4. processed/clean 与 rejected：实际 Spark Parquet 结果，原始层保持不变。",
        "5. analysis/：按不同维度统计的 CSV、双维对比、字段单位和预览。",
        "6. export/：网页查询及 ML 特征/标签交接；analytics.sqlite3：只读查询快照。",
        "7. 启动 FastAPI 接口，调用 health、overview、charts；Vue 3 + DataV 网页由网页组接入。", "",
        "## 本轮清洗原因统计", "", "| 原因 | 行数 |", "| --- | ---: |"]
    for reason, count in sorted(quality["rejectionReasons"].items()):
        lines.append(f"| {reason} | {count} |")
    if not quality["rejectionReasons"]:
        lines.append("| 无隔离记录 | 0 |")
    lines += ["", "## 尚需现场验证的内容", "",
        "- 本轮只验证本地 Spark，不是 HDFS 或多节点集群验收。",
        "- 在老师的 Hadoop 3.x 环境运行 verify_hdfs，保存独立验证报告。",
        "- 网页的 Vue 3、DataV、图表交互及 Node.js 版本需网页组实际验收。",
        "- 此处只准备 ML 特征和独立标签，未宣称模型训练、预测精度已完成。",
        "- 缺失的关键 ID/金额不靠均值填充；异常高负荷不因偏离均值而直接删除。", ""]
    return "\n".join(lines)


def prepare_acceptance(spark, input_root, output_root, *, inject_rate=None, seed=42):
    source, root = local_roots(input_root, output_root)
    if inject_rate is not None and (type(inject_rate) not in (float, int) or
            not math.isfinite(inject_rate) or not 0 <= inject_rate <= 1):
        raise ValueError("inject-rate must be a finite number between 0 and 1")
    if type(seed) is not int or not 0 <= seed <= 2**32 - 1:
        raise ValueError("seed must be an integer between 0 and 2**32-1")
    from data_analysis.charging_data.inject_dirty import inject_dataset
    from data_analysis.spark_jobs.pipeline import run_pipeline
    from data_analysis.spark_jobs.acceptance_analysis import export_acceptance_analysis
    from data_analysis.spark_jobs.export import export_data
    from data_analysis.publishing.publish import publish_dataset

    original_manifest_hash = hashlib.sha256((source / "manifest.json").read_bytes()).hexdigest()
    root.mkdir(parents=True, exist_ok=False)
    (root / "_RUNNING").touch(exist_ok=False)
    try:
        raw = source
        if inject_rate is not None:
            raw = root / "dirty_input"
            inject_dataset(source, raw, seed=seed, rate=inject_rate)
        quality = run_pipeline(spark, str(raw), str(root / "processed"))
        analysis = export_acceptance_analysis(spark, str(raw), str(root / "processed"), str(root / "analysis"))
        evidence = validate_evidence(root, quality, analysis)
        serving = export_data(spark, str(raw), str(root / "processed"), str(root / "export"))
        publication = publish_dataset(root / "export", root / "analytics.sqlite3")
        conserved = (quality["input_rows"]["charging_sessions"] ==
            quality["clean_session_rows"] + quality["rejected_session_rows"])
        if not conserved:
            raise ValueError("Session row conservation failed")
        if hashlib.sha256((source / "manifest.json").read_bytes()).hexdigest() != original_manifest_hash:
            raise ValueError("Source manifest changed during preparation")
        if serving["pipelineRunId"] != quality["pipeline_run_id"]:
            raise ValueError("Serving and cleaning run identities differ")
        report = {
            "reportVersion": "acceptance-1.0.0", "status": "LOCAL_DATA_READY",
            "generatedAt": datetime.now(timezone.utc).isoformat(),
            "datasetId": quality["dataset_id"], "pipelineRunId": quality["pipeline_run_id"],
            "publishedBatchId": serving["publishedBatchId"],
            "source": "SIMULATED", "pythonVersion": sys.version.split()[0], "sparkVersion": spark.version,
            "originalManifestSha256": original_manifest_hash,
            "sourceManifestSha256": quality["source_manifest_sha256"],
            "dirtyInjection": {"enabled": inject_rate is not None, "rate": inject_rate, "seed": seed},
            "cleaning": {"tableCount": len(quality["input_rows"]),
                "inputRows": sum(quality["input_rows"].values()),
                "cleanSessions": quality["clean_session_rows"], "rejectedSessions": quality["rejected_session_rows"],
                "rejectionReasons": quality["rejection_reasons"], "rowsConserved": conserved,
                "rulesPath": "processed/reports/cleaning_rules.json",
                "auditPath": "processed/reports/cleaning_audit.json"},
            "analysis": analysis, "evidence": evidence, "publication": publication,
            "hdfs": {"status": "NOT_VERIFIED", "reason": "Local run; execute verify_hdfs in target environment"},
            "frontend": {"status": "NOT_VERIFIED", "required": "Vue 3 + DataV, Node.js >= 23"},
            "machineLearning": {"status": "FEATURES_READY_MODELS_NOT_TRAINED"},
        }
        write_json(root / "acceptance_report.json", report)
        with (root / "acceptance_report.md").open("x", encoding="utf-8") as stream:
            stream.write(report_markdown(report))
        (root / "_RUNNING").unlink()
        (root / "_SUCCESS").touch(exist_ok=False)
        return report
    except Exception as exc:
        write_json(root / "failure_report.json", {"status": "FAILED", "errorType": type(exc).__name__,
            "message": str(exc), "note": "Preserved for diagnosis. Choose a new output directory for retry."})
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--inject-rate", type=float, default=None,
        help="Optional extra dirty injection; original sample already contains dirty records")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--master", default="local[2]")
    parser.add_argument("--shuffle-partitions", type=int, default=8)
    parser.add_argument("--driver-memory", default=None)
    args = parser.parse_args(argv)
    try:
        local_roots(args.input, args.output)
        if args.inject_rate is not None and (not math.isfinite(args.inject_rate) or not 0 <= args.inject_rate <= 1):
            raise ValueError("inject-rate must be between 0 and 1")
        if not 0 <= args.seed <= 2**32 - 1:
            raise ValueError("seed must be between 0 and 2**32-1")
        from data_analysis.spark_jobs.pipeline import create_spark
        spark = create_spark("charging-cleaning-acceptance", args.master, args.shuffle_partitions, args.driver_memory)
    except (ValueError, FileExistsError) as exc:
        parser.error(str(exc))
    try:
        report = prepare_acceptance(spark, args.input, args.output, inject_rate=args.inject_rate, seed=args.seed)
        print(json.dumps({"status": report["status"], "datasetId": report["datasetId"],
            "report": str(Path(args.output).resolve() / "acceptance_report.md"), "hdfs": report["hdfs"]["status"]}, ensure_ascii=False))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
