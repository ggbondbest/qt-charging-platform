"""页面动作白名单:navigate/fill 两种,清单在 ai_actions.json(单一事实源)。

纪律(对抗调研定死的三条):
1. 结构化意图只走工具调用,模型正文里不许出现动作 JSON;动作由**代码层**从工具
   返回收集(agent._collect_actions),与 citations 同款"收凭证不信自觉"。
2. fill 是 prefill-only:只写值+发 input/change,永不代为提交(执行器没有
   click 提交按钮的能力,清单里也不存在提交按钮条目)。
3. 动作的 machine 字段(route/section/target/value/id)不过 friendly.scrub
   ——scrub 是给人话正文准备的,拿它洗枚举会静默改数据;守卫改为本模块精确
   集合比对 + valueRule 校验,并由单测钉死"manifest 全部 id 过 scrub 原样不变"。
"""
from __future__ import annotations

import json
import re
from functools import lru_cache
from pathlib import Path

MANIFEST_PATH = Path(__file__).with_name("ai_actions.json")

NAVIGATE_ALIASES = {
    "运营总览": "overview", "总览": "overview", "概览": "overview", "仪表盘": "overview",
    "智能找站": "explore", "找站": "explore",
    "我的行程": "trip", "行程": "trip",
    "智能分析": "lab", "分析": "lab",
    "负荷与空闲预测": "lab-forecast", "负荷预测": "lab-forecast", "空闲预测": "lab-forecast",
    "用户与异常": "lab-insights", "异常筛查": "lab-insights", "回访风险": "lab-insights",
    "到站模型": "lab-arrival", "到站": "lab-arrival",
    "策略对比": "lab-experiments", "配对实验": "lab-experiments",
}

FILL_ALIASES = {
    "编号": "insights-query", "会话编号": "insights-query", "用户编号": "insights-query",
    "分析编号": "insights-query", "查询框": "insights-query",
    "开始日期": "dash-start", "统计开始日期": "dash-start",
    "结束日期": "dash-end", "统计结束日期": "dash-end",
    "预测目标": "forecast-target", "跨度": "forecast-horizon", "预测跨度": "forecast-horizon",
    "起点": "forecast-reference", "预测起点": "forecast-reference",
    "昵称": "session-name", "演示昵称": "session-name",
    "补电量": "energy-kwh", "计划补电": "energy-kwh",
    "最远行驶": "max-eta", "行驶时间": "max-eta",
}


@lru_cache(maxsize=1)
def manifest() -> dict:
    m = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    m["_nav"] = {t["id"]: t for t in m["navigate"]}
    m["_fill"] = {t["id"]: t for t in m["fill"]}
    # SPEC 给模型看的是中文 label(防内部 id 复读),所以 target 两种写法都要认:
    # label 是真实后端的正常路径,id 是 mock/测试的兼容路径
    m["_nav_by_label"] = {t["label"]: t for t in m["_nav"].values()}
    m["_fill_by_label"] = {t["label"]: t for t in m["_fill"].values()}
    return m


def nav_ids() -> list[str]:
    return list(manifest()["_nav"])


def fill_ids() -> list[str]:
    return list(manifest()["_fill"])


def label_of(kind: str, target_id: str) -> str:
    table = manifest()["_nav"] if kind == "navigate" else manifest()["_fill"]
    t = table.get(target_id)
    return t["label"] if t else target_id


def _check_value(rule: dict, value: str) -> str | None:
    if len(value) > rule.get("maxLength", 64):
        return "内容过长"
    enum = rule.get("enum")
    if enum is not None and value not in enum:
        return "不在可选值里(可选项:" + "、".join(enum) + ")"
    pat = rule.get("pattern")
    if pat is not None and not re.match(pat, value):
        return "格式不正确"
    return None


def normalize(raw) -> tuple[dict | None, str]:
    """白名单精确校验;返回 (action | None, 失败原因)。失败原因只回喂模型,不出境。"""
    m = manifest()
    if not isinstance(raw, dict):
        return None, "动作必须是一个对象"
    kind = raw.get("kind")
    tid = str(raw.get("target", "")).strip()
    if kind == "navigate":
        t = m["_nav"].get(tid) or m["_nav_by_label"].get(tid)
        if not t:
            return None, f"未知页面「{tid}」,可跳转:" + "、".join(x["label"] for x in m["_nav"].values())
        return {"kind": "navigate", "target": t["id"], "route": t["route"],
                "section": t.get("section"), "label": t["label"]}, ""
    if kind == "fill":
        t = m["_fill"].get(tid) or m["_fill_by_label"].get(tid)
        if not t:
            return None, f"未知输入框「{tid}」,可预填:" + "、".join(x["label"] for x in m["_fill"].values())
        value = raw.get("value")
        value = "" if value is None else str(value).strip()
        if not value:
            return None, "fill 需要要填入的内容"
        reason = _check_value(t["valueRule"], value)
        if reason:
            return None, f"「{t['label']}」填入失败:{reason}"
        return {"kind": "fill", "target": t["id"], "route": t["route"],
                "section": t.get("section"), "value": value, "label": t["label"]}, ""
    return None, "kind 只能是 navigate 或 fill"


def page_action(kind=None, target=None, value=None) -> str:
    """工具处理器:模型提议动作 → 白名单校验 → 输出规范 ui_actions(前端执行)。

    签名按 tools.execute 的 **kwargs 调用约定逐字命名(未声明字段会被 props 白名单滤掉)。
    本工具不写任何业务数据、不发任何网络请求、不点任何提交按钮;
    navigate 只是切页面,fill 只把值放进输入框,提交永远留给用户亲手点。
    """
    from .tools import _dump, _err  # 局部导入避免循环
    raw = {
        "kind": (kind if isinstance(kind, str) else "").strip().lower(),
        "target": (target if isinstance(target, str) else "").strip(),
        "value": value if isinstance(value, str) else (None if value is None else str(value)),
    }
    act, reason = normalize(raw)
    if act is None:
        return _err(f"页面操作不可执行:{reason}")
    return _dump({"ok": True, "ui_actions": [act],
                  "note": "动作已排队,由页面在用户侧执行;正文用一句人话交代即可"})


# 工具规约(注册进 tools.REGISTRY;描述里只有中文页名,不给内部 id,防复读泄露)
SPEC = {
    "description": ("页面引导(仅当用户明确要求打开/切换某页,或要求把某内容填入某输入框时调用;"
                    "数据问答严禁调用,一次提问至多一次)。kind=navigate 时 target 从这些页面里选:"
                    + "、".join(t["label"] for t in manifest()["_nav"].values())
                    + ";kind=fill 时 target 从这些输入框里选:"
                    + "、".join(t["label"] for t in manifest()["_fill"].values())
                    + ",value 为要填入的内容。它不修改任何数据、不会代为提交查询,提交由用户亲手点。"),
    "parameters": {"type": "object", "properties": {
        "kind": {"type": "string", "enum": ["navigate", "fill"]},
        "target": {"type": "string", "description": "页面或输入框名称,必须逐字来自描述里的清单"},
        "value": {"type": "string", "description": "仅 fill:要填入的内容"}},
        "required": ["kind", "target"]},
}
