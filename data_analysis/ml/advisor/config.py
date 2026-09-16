"""advisor 配置:路径 + 后端选择。零新依赖,自带迷你 .env 解析(仅本目录 .env)。

后端选择的防呆设计:只有 advisor 自己的 .env 里显式写了 key,或 env 显式给了
ML_ADVISOR_BACKEND,才会离开 mock。原因——开发机上常挂着别的工具链的
ANTHROPIC_*/OPENAI_* 环境变量,误用别人的凭据既不安全也不可控,别猜,要显式。
"""
from __future__ import annotations

import os
from pathlib import Path

ADVISOR_DIR = Path(__file__).resolve().parent
DA_ROOT = ADVISOR_DIR.parents[1]                       # data_analysis/
REPO_ROOT = DA_ROOT.parent
OUT_DIR = DA_ROOT / "outputs" / "ml_advisor"           # gitignored(data_analysis/.gitignore: outputs/)
ANOMALY_OUT = DA_ROOT / "outputs" / "ml_anomaly"
CHURN_OUT = DA_ROOT / "outputs" / "ml_churn"
LOAD_OUT = DA_ROOT / "outputs" / "ml_load"
CLEAN_DIR = DA_ROOT / "datasets" / "analytics_full_180d_v1" / "clean"
ALERTS_CSV = OUT_DIR / "alerts_v5.csv"
KNOWLEDGE_DB = OUT_DIR / "knowledge.db"

_DOTENV_KEYS: set[str] = set()


def load_dotenv(path: Path | None = None) -> None:
    """极简 .env:KEY=VALUE,# 注释;setdefault 不覆盖真 env。幂等。"""
    p = path or (ADVISOR_DIR / ".env")
    if not p.exists():
        return
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip().strip('"').strip("'")
        if k and k not in _DOTENV_KEYS:
            _DOTENV_KEYS.add(k)
            os.environ.setdefault(k, v)


def backend_name() -> str:
    load_dotenv()
    explicit = (os.environ.get("ML_ADVISOR_BACKEND") or "").strip().lower()
    if explicit:
        return explicit
    if "OPENAI_API_KEY" in _DOTENV_KEYS:
        return "openai"
    if "ANTHROPIC_API_KEY" in _DOTENV_KEYS or "ANTHROPIC_AUTH_TOKEN" in _DOTENV_KEYS:
        return "anthropic"
    return "mock"


def model_id() -> str:
    load_dotenv()
    return os.environ.get("ML_ADVISOR_MODEL", "deepseek-v4-pro")


def max_steps() -> int:
    load_dotenv()
    try:
        return max(1, int(os.environ.get("ML_ADVISOR_MAX_STEPS", "6")))
    except ValueError:
        return 6
