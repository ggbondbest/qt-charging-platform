"""CLI 入口(仓库根目录运行):
  python -m data_analysis.ml.advisor.driver --prepare          # 生成告警清单+知识索引
  python -m data_analysis.ml.advisor.driver "v5 在 TEST 段误报了几条?"
  python -m data_analysis.ml.advisor.driver --list-tools
  python -m data_analysis.ml.advisor.driver --tool read_test_metrics --args '{"model_id":"v5"}'
后端默认 mock(离线可跑);.env 里写明 key 后自动走 openai/anthropic,或 --backend 强指。
"""
from __future__ import annotations

import argparse
import json
import sys

from . import config


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="advisor", description="AI 运营参谋(只读检索 agent)")
    ap.add_argument("question", nargs="?", help="用自然语言提问")
    ap.add_argument("--backend", choices=["mock", "openai", "anthropic"], default=None)
    ap.add_argument("--prepare", action="store_true", help="生成参谋可读的派生工件(告警清单/FTS 索引)")
    ap.add_argument("--force", action="store_true", help="prepare 时强制重建")
    ap.add_argument("--list-tools", action="store_true")
    ap.add_argument("--tool", help="直接调用单个工具(调试用,不经模型)")
    ap.add_argument("--args", default="{}", help="--tool 的 JSON 参数")
    ap.add_argument("-q", "--quiet", action="store_true")
    ns = ap.parse_args(argv)

    if ns.prepare:
        from .artifacts import prepare
        prepare(force=ns.force)
        return 0
    if ns.list_tools:
        for s in __import__("data_analysis.ml.advisor.tools", fromlist=["SPECS"]).SPECS:
            print(f"{s['name']}: {s['description']}")
        return 0
    if ns.tool:
        from .tools import execute
        print(execute(ns.tool, json.loads(ns.args)))
        return 0
    if not ns.question:
        ap.print_help()
        return 2

    from .agent import ask
    backend = ns.backend or config.backend_name()
    if not ns.quiet:
        print(f"[advisor] backend={backend} model={config.model_id() if backend != 'mock' else '-'}")
    r = ask(ns.question, backend=backend, verbose=not ns.quiet)
    print("\n=== 回答 ===")
    print(r["answer"])
    print(f"\n=== 出处({len(r['citations'])})==={'未携带来源!' if not r['citations'] else ''}")
    for c in r["citations"]:
        print(f"  - {c}")
    if not ns.quiet:
        print(f"\n(rounds={r['rounds']}, 工具调用 {len(r['trace'])} 次, 后端 {r['backend']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
