"""把"内部工件话"翻译给运营用户:正文去技术化,出处变人类标签。

纪律不变:引用收集仍在代码层(agent.ask 的 citations 是真凭据);本模块只做
**展示层翻译**,driver CLI 仍可看原始路径(调试用),HTTP 出口(桌宠)默认只发友好名。
ML_ADVISOR_SERVE_DEBUG=1 时附带 raw 出处,供答辩后自审。
"""
from __future__ import annotations

import os
import re

# 出处前缀 → 运营看得懂的标签(最长前缀优先)
SOURCE_LABELS = {
    "outputs/ml_anomaly/test_metrics_": "异常检测·盲评考试记录",
    "outputs/ml_churn/test_metrics_": "用户流失·盲评考试记录",
    "outputs/ml_load/test_metrics_": "负荷预测·盲评考试记录",
    "outputs/ml_advisor/alerts_v5.csv": "异常告警清单",
    "outputs/ml_anomaly/context-weather-fixedthr-v5.joblib": "异常检测模型配置(冻结)",
    "outputs/ml_anomaly/context_signals_v5.pkl": "会话信号台账",
    "data_analysis/ml/README.md": "模型登记簿",
    "clean/weather_hourly": "天气观测记录",
    "clean/calendar": "运营日历",
    "clean/maintenance_tickets": "维修工单记录",
    "clean/queue_entries": "排队记录",
    "clean/charging_sessions": "充电会话记录",
    "clean/charging_attempts": "充电尝试记录",
    "clean/stations": "电站档案",
    "clean/chargers": "电桩台账",
    "clean/reviews": "用户评价",
    "clean/tariffs": "分时电价表",
    "clean/campaigns": "运营活动档案",
}

# 模型技术名 → 人话(短版本号 v5/v4 团队口头也用,保留)
MODEL_NAMES = {
    "context-weather-fixedthr-v5": "现行异常检测模型(v5)",
    "context-baseline-rankfuse-v4": "上一版异常检测模型(v4)",
    "iforest-session-rankfuse-v3": "异常检测早期版本(v3)",
    "iforest-pointmax-battery-v2": "异常检测早期版本(v2)",
    "iforest-session-battery-v1": "异常检测初版(v1)",
    "gbdt-churn-user-v2": "流失风险模型",
    "hgb-q50-history24-v1": "负荷预测模型",
}


def _file_label(name: str) -> str:
    """按文件名猜人话标签(test_metrics_xxx.json / alerts_v5.csv / *.joblib 这类)。"""
    if "test_metrics" in name:
        line = ("异常检测" if "context-" in name or "iforest" in name
                else "用户流失" if "churn" in name
                else "负荷预测" if "hgb" in name else "模型")
        return f"{line}·盲评考试记录"
    if name.endswith(".joblib"):
        return "模型配置(冻结)"
    if name.endswith(".pkl"):
        return "会话信号台账"
    if "alerts" in name:
        return "异常告警清单"
    if name.endswith(".md"):
        return "平台文档"
    return "平台留档"


def label_for(source: str) -> str:
    s = str(source).replace("\\", "/")
    best, best_len = None, -1
    for prefix, label in SOURCE_LABELS.items():
        if s.startswith(prefix) and len(prefix) > best_len:
            best, best_len = label, len(prefix)
    if best:
        return best
    tail = s.rsplit("/", 1)[-1]
    if re.search(r"test_metrics|\.json$|\.csv$|\.pkl$|\.joblib$|\.db$|\.parquet$", tail):
        return _file_label(tail)
    tail = re.sub(r"\.(md|txt|py)$", "", tail)
    return scrub(tail) or "平台留档"  # 兜底也不放技术名出门


def _path_pat() -> re.Pattern:
    dirs = "|".join(re.escape(d) for d in ("outputs", "datasets", "raw", "clean", "data_analysis"))
    return re.compile(rf"(?:{dirs})/[\w\-./]+")


_PATH_RE = _path_pat()


# 无目录前缀的裸文件名/评测件名:也要翻译,不许原样出境
_BARE_FILE_RE = re.compile(r"[\w\-./]*\b(?:test_metrics[\w\-\.]*|[\w\-]+\.(?:json|csv|pkl|joblib|db|parquet))\b")
_CITE_TAG_RE = re.compile(r"[\[（(]\s*(?:来源|出处)\s*(?:[:：][^\]）)]{0,120})?[\]）)]")

# 验收补交的三层防线:
# ① Windows 绝对路径与点分模块名(异常原文里解释器自带的,含用户名/桌面目录)
_ABS_WIN_RE = re.compile(r"[A-Za-z]:[\\/][^\s\"'`,;)\]（）]+")
# 同族的 POSIX 绝对路径(WSL/Linux/Mac 队友机器上的异常原文同样含用户名/目录结构)
_ABS_NIX_RE = re.compile(r"/(?:home|Users|root|mnt|var|tmp|opt)/[^\s\"'`,;)\]（）]*")
_MODULE_RE = re.compile(
    r"\b(?:data_analysis|outputs|datasets|clean|raw|advisor|anomaly)(?:\.[\w\-]+)+\b")
# ② 模型复读禁规清单时吐出的裸词:扩展名/内部别名/条款编号/环境变量/供应商/凭据残片
_BARE_EXT_RE = re.compile(r"(?<![\w.\\-])\.(?:json|csv|pkl|joblib|db|parquet|txt|yaml|yml|md|py)\b")
_ALIAS_RE = re.compile(r"\b(?:hgb|gbdt|iforest|context|detect|train|valid)[-_][\w-]+")
_RULENUM_RE = re.compile(r"(?:规则|条款)\s*\d+\s*[a-z]?(?:[:：]?\s*项)?")
_ENVKEY_RE = re.compile(r"\bML_ADVISOR_[A-Z_]+\b")
_VENDOR_RE = re.compile(r"\b(?:deepseek|openai|anthropic)\b", re.I)
_KEYMASK_RE = re.compile(r"\bsk-[A-Za-z0-9*.…\-]{2,}")
_README_RE = re.compile(r"\bREADME(?:\.\w+)?\b", re.I)


def scrub(text: str) -> str:
    """正文安全网:模型没听话时,由代码替它守规矩。"""
    out = text
    out = _ABS_WIN_RE.sub("本机路径(已隐去)", out)
    out = _ABS_NIX_RE.sub("本机路径(已隐去)", out)
    out = _PATH_RE.sub(lambda m: label_for(m.group(0).rstrip(".")), out)
    out = _BARE_FILE_RE.sub(lambda m: _file_label(m.group(0)), out)
    out = _MODULE_RE.sub("内部代码模块", out)
    for mid, cname in MODEL_NAMES.items():
        out = out.replace(mid, cname)
    out = _ALIAS_RE.sub("模型技术编号(已隐去)", out)
    out = _BARE_EXT_RE.sub("受控格式文件", out)
    out = _ENVKEY_RE.sub("内部配置项", out)
    out = _VENDOR_RE.sub("在线模型服务", out)
    out = _KEYMASK_RE.sub("(凭据已隐去)", out)
    out = _README_RE.sub("平台文档", out)
    out = _RULENUM_RE.sub("平台规则", out)
    # 正文里的行内出处标注剥掉——出处走气泡下方的专用层,不混在话里
    out = _CITE_TAG_RE.sub("", out)
    out = re.sub(r"[ \t]{2,}", " ", out)
    out = re.sub(r"[ \t]+$", "", out, flags=re.MULTILINE)
    return out


def _narrow_action(a: dict) -> dict | None:
    """页面动作出境终检:重查白名单+收窄字段。machine 字段刻意**不过 scrub**
    (scrub 会改数据不遮数据,破坏语义);label 一律由注册表 resolve,不信模型措辞。"""
    from . import actions as _act
    if not isinstance(a, dict):
        return None
    m = _act.manifest()
    if a.get("kind") == "navigate" and a.get("target") in m["_nav"]:
        t = m["_nav"][a["target"]]
        if a.get("route") != t["route"] or a.get("section") != t.get("section"):
            return None  # 与注册表不一致(被中途篡改)→ 整条丢
        return {"kind": "navigate", "target": t["id"], "route": t["route"],
                "section": t.get("section"), "label": t["label"]}
    if a.get("kind") == "fill" and a.get("target") in m["_fill"]:
        t = m["_fill"][a["target"]]
        v = a.get("value")
        if not isinstance(v, str) or _act._check_value(t["valueRule"], v):
            return None
        return {"kind": "fill", "target": t["id"], "route": t["route"],
                "section": t.get("section"), "value": v, "label": t["label"]}
    return None


def _cap_actions(seq) -> list[dict]:
    out: list[dict] = []
    nav = fill = 0
    for x in seq:
        if not x:
            continue
        if x["kind"] == "navigate":
            nav += 1
            if nav > 1:
                continue
        else:
            fill += 1
            if fill > 2:
                continue
        out.append(x)
    return out


def humanize(result: dict) -> dict:
    """agent.ask 的返回 → 桌宠可安全展示的形态(默认不带 raw 路径/后端名/模型名)。"""
    backend = result.get("backend", "")
    out = {
        "answer": scrub(result.get("answer", "")),
        "sources": list(dict.fromkeys(label_for(c) for c in result.get("citations", []))),
        # 出境侧再封一次顶(与 agent 收集层同一承诺:navigate≤1、fill≤2)。
        # 防线不叠不能算完——收集层若被绕过,这里兜住,前端只会看到 ≤3 条芯片。
        "actions": _cap_actions(_narrow_action(a) for a in (result.get("actions") or [])),
        "rounds": result.get("rounds"),
        "mode": "offline" if backend == "mock" else "online",
        "tools": [{"round": t.get("round"), "tool": t.get("tool")} for t in result.get("trace", [])],
    }
    if os.environ.get("ML_ADVISOR_SERVE_DEBUG"):
        out["debug"] = {"backend": backend, "rawCitations": result.get("citations", [])}
    return out
