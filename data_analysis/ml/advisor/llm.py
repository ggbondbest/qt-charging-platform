"""协议层:中立对话状态 ↔ OpenAI / Anthropic 两种线上格式的双向渲染 + 后端。

中立 transcript 约定(agent/mock 只见这个结构):
  {"role":"user","content":str}
  {"role":"assistant","content":str|None,"tool_calls":[{"id","name","arguments":dict}]}
  {"role":"tool","tool_call_id":str,"name":str,"content":str}
差异都消化在本文件:tool_call_id vs tool_use_id、arguments 的 JSON 字符串 vs dict、
system 位置、并行 tool_result 的合并规则。SDK 全懒加载:没装 openai/anthropic
也能跑 mock 与工具层。
"""
from __future__ import annotations

import json
import os
import uuid

from . import config


def render_openai(system: str, transcript: list[dict], specs: list[dict]) -> tuple[list, list]:
    msgs: list[dict] = [{"role": "system", "content": system}]
    for t in transcript:
        if t["role"] == "user":
            msgs.append({"role": "user", "content": t["content"]})
        elif t["role"] == "assistant":
            m: dict = {"role": "assistant", "content": t.get("content")}
            if t.get("tool_calls"):
                m["tool_calls"] = [{"id": c["id"], "type": "function",
                                    "function": {"name": c["name"],
                                                 "arguments": json.dumps(c["arguments"], ensure_ascii=False)}}
                                   for c in t["tool_calls"]]
            msgs.append(m)
        elif t["role"] == "tool":
            msgs.append({"role": "tool", "tool_call_id": t["tool_call_id"], "content": t["content"]})
    tools = [{"type": "function", "function": {"name": s["name"], "description": s["description"],
                                               "parameters": s["parameters"]}} for s in specs]
    return msgs, tools


def render_anthropic(system: str, transcript: list[dict], specs: list[dict]) -> tuple[list, list]:
    msgs: list[dict] = []
    for t in transcript:
        if t["role"] == "user":
            msgs.append({"role": "user", "content": t["content"]})
        elif t["role"] == "assistant":
            blocks: list[dict] = []
            if t.get("content"):
                blocks.append({"type": "text", "text": t["content"]})
            for c in t.get("tool_calls", []):
                blocks.append({"type": "tool_use", "id": c["id"], "name": c["name"],
                               "input": c["arguments"]})
            msgs.append({"role": "assistant", "content": blocks})
        elif t["role"] == "tool":
            prev = msgs[-1] if msgs else None
            block = {"type": "tool_result", "tool_use_id": t["tool_call_id"], "content": t["content"]}
            if prev and prev["role"] == "user" and isinstance(prev["content"], list) \
                    and all(b.get("type") == "tool_result" for b in prev["content"]):
                prev["content"].append(block)  # 并行工具结果合并进同一 user 轮
            else:
                msgs.append({"role": "user", "content": [block]})
    tools = [{"name": s["name"], "description": s["description"], "input_schema": s["parameters"]}
             for s in specs]
    return msgs, tools


class OpenAIBackend:
    name = "openai"

    def __init__(self):
        from openai import OpenAI  # 懒加载
        config.load_dotenv()
        self.model = config.model_id()
        self.client = OpenAI(api_key=os.environ.get("OPENAI_API_KEY"),
                             base_url=os.environ.get("OPENAI_BASE_URL") or None)

    def generate(self, system: str, transcript: list[dict], specs: list[dict]) -> dict:
        msgs, tools = render_openai(system, transcript, specs)
        # deepseek-v4-pro 默认思考:小 max_tokens 会被 reasoning 吃光(finish=length 空正文)。
        # 文档推荐工具调用关思考;网关若不认 thinking 参数则降级重发(配置可留 enabled)。
        kw = {"model": self.model, "messages": msgs, "tools": tools,
              "tool_choice": "auto", "max_tokens": 4096}
        thinking = os.environ.get("ML_ADVISOR_THINKING", "disabled")
        if thinking == "disabled":
            try:
                r = self.client.chat.completions.create(extra_body={"thinking": {"type": "disabled"}}, **kw)
            except Exception as e:
                if "thinking" in str(e).lower():
                    r = self.client.chat.completions.create(**kw)
                else:
                    raise
        else:
            r = self.client.chat.completions.create(**kw)
        choice = r.choices[0]
        msg = choice.message
        calls = []
        for c in (msg.tool_calls or []):
            try:  # length 截断的半截参数 JSON 不许炸循环:空参交给工具,tool_error 帧让模型自纠
                args = json.loads(c.function.arguments or "{}")
            except json.JSONDecodeError:
                args = {}
            calls.append({"id": c.id, "name": c.function.name, "arguments": args})
        return {"content": msg.content, "tool_calls": calls,
                "truncated": choice.finish_reason == "length"}


class AnthropicBackend:
    name = "anthropic"

    def __init__(self):
        from anthropic import Anthropic  # 懒加载
        config.load_dotenv()
        self.model = config.model_id()
        self.client = Anthropic(api_key=os.environ.get("ANTHROPIC_API_KEY") or None,
                                auth_token=os.environ.get("ANTHROPIC_AUTH_TOKEN") or None,
                                base_url=os.environ.get("ANTHROPIC_BASE_URL") or None)

    def generate(self, system: str, transcript: list[dict], specs: list[dict]) -> dict:
        msgs, tools = render_anthropic(system, transcript, specs)
        kw: dict = {"model": self.model, "system": system, "messages": msgs,
                    "tools": tools, "max_tokens": 4096}
        if os.environ.get("ML_ADVISOR_THINKING", "disabled") == "disabled":
            kw["thinking"] = {"type": "disabled"}  # DeepSeek Anthropic 口官方支持;工具调用更稳
        r = self.client.messages.create(**kw)
        text = "".join(b.text for b in r.content if b.type == "text")
        calls = [{"id": b.id, "name": b.name, "arguments": b.input}
                 for b in r.content if b.type == "tool_use"]
        return {"content": text or None, "tool_calls": calls,
                "truncated": r.stop_reason == "max_tokens"}


def get_backend(force: str | None = None):
    kind = (force or config.backend_name()).lower()
    if kind == "mock":
        from .mock import MockBackend
        return MockBackend()
    if kind == "openai":
        return OpenAIBackend()
    if kind == "anthropic":
        return AnthropicBackend()
    raise ValueError(f"未知后端 {kind}(可选 mock,在线后端请按 advisor/.env 配置)")
