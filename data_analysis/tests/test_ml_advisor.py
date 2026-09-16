"""advisor 离线测试:工具规约合法、答案必带来源、真值不泄露、mock 端到端、协议渲染结构。
不依赖网络与 key;冻结工件(outputs/ml_anomaly)缺失时自跳(CI 无产物环境)。
真后端冒烟是显式选项:ML_ADVISOR_LIVE=1 才跑(会计费)。
"""

import json
import os
import unittest

try:
    import pandas  # noqa: F401

    from data_analysis.ml.advisor import actions, agent, artifacts, config, friendly, mock, tools
    from data_analysis.ml.advisor import llm

    HAVE_DEPS = True
except ImportError:
    HAVE_DEPS = False

NEED_ARTIFACTS = HAVE_DEPS and config.ANOMALY_OUT.exists()


@unittest.skipUnless(NEED_ARTIFACTS, "outputs/ml_anomaly artifacts absent")
class ToolContract(unittest.TestCase):
    def test_specs_wellformed(self):
        for s in tools.SPECS:
            self.assertTrue(s["name"] and s["description"])
            self.assertEqual(s["parameters"]["type"], "object")
            self.assertIn(s["name"], tools.REGISTRY)

    def test_read_test_metrics_v5_with_source(self):
        out = json.loads(tools.execute("read_test_metrics", {"model_id": "v5"}))
        self.assertIn("test", out)
        frozen = json.loads((config.ANOMALY_OUT / "test_metrics_context-weather-fixedthr-v5.json")
                            .read_text(encoding="utf-8"))
        self.assertEqual(out["test"]["f1"], frozen["test"]["f1"])
        self.assertIn("test_metrics_context-weather-fixedthr-v5.json", out["source"])

    def test_unknown_args_are_stripped_not_crash(self):
        out = json.loads(tools.execute("read_test_metrics",
                                       {"model_id": "v5", "rm_rf": True}))  # 模型瞎塞字段
        self.assertNotIn("error", out)

    def test_bad_model_id_becomes_guidance(self):
        out = json.loads(tools.execute("read_test_metrics", {"model_id": "no-such-model"}))
        self.assertIn("error", out)
        self.assertIn("无依据", out["advice"])


@unittest.skipUnless(NEED_ARTIFACTS, "outputs/ml_anomaly artifacts absent")
class GroundTruthLeak(unittest.TestCase):
    """参谋必须只看模型输出侧:告警行结构里不允许出现任何标签列。"""

    @classmethod
    def setUpClass(cls):
        artifacts.prepare()

    def test_alerts_has_no_label_columns(self):
        out = json.loads(tools.execute("query_alerts", {"split": "TEST"}))
        if "error" in out:
            self.fail("prepare 之后告警清单应可读")
        allowed = {"session_id", "station_id", "charger_id", "city_id", "date",
                   "split", "score", "threshold"}
        for row in out["rows"]:
            self.assertLessEqual(set(row), allowed)  # 白名单即硬边界:标签列不可能出现
            for k in ("anomaly", "label", "truth", "severity"):
                self.assertFalse(any(k in col.lower() for col in row))


@unittest.skipUnless(NEED_ARTIFACTS, "outputs/ml_anomaly artifacts absent")
class KnowledgeSearch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        artifacts.prepare()

    def test_hit_carries_source(self):
        out = json.loads(tools.execute("search_knowledge", {"query": "气象上下文"}))
        self.assertTrue(out.get("hits"), "语料里必有『气象上下文』(anomaly README)")
        self.assertTrue(all("source" in h for h in out["hits"]))

    def test_miss_returns_advice(self):
        out = json.loads(tools.execute("search_knowledge", {"query": "股票期货外汇"}))
        self.assertIn("error", out)


@unittest.skipUnless(NEED_ARTIFACTS, "outputs/ml_anomaly artifacts absent")
class MockEndToEnd(unittest.TestCase):
    """mock 后端走完整 agent 循环:数字只允许来自工具结果,且答案带出处。"""

    @classmethod
    def setUpClass(cls):
        artifacts.prepare()

    def test_numbers_come_from_artifacts(self):
        frozen = json.loads((config.ANOMALY_OUT / "test_metrics_context-weather-fixedthr-v5.json")
                            .read_text(encoding="utf-8"))
        r = agent.ask("v5 在 TEST 段误报了几条?", backend=mock.MockBackend())
        self.assertIn(f"误报 {frozen['test']['fp']}", r["answer"])
        self.assertIn("[来源]", r["answer"])
        self.assertTrue(r["citations"])

    def test_multistep_drilldown(self):
        r = agent.ask("TEST 段告警清单给我,顺便下钻", backend=mock.MockBackend())
        called = [t["tool"] for t in r["trace"]]
        self.assertIn("query_alerts", called)
        self.assertIn("explain_session", called)  # 第二轮自动下钻 = 多步链成立

    def test_no_evidence_says_so(self):
        r = agent.ask("明天的股票行情怎么样?", backend=mock.MockBackend())
        self.assertIn("无依据", r["answer"])


@unittest.skipUnless(HAVE_DEPS, "advisor deps missing")
class ProtocolRender(unittest.TestCase):
    """中立 transcript → 两种线上格式的结构差异必须在这里消化,别漏到循环里。"""

    TX = [
        {"role": "user", "content": "v5 误报?"},
        {"role": "assistant", "content": None,
         "tool_calls": [{"id": "a1", "name": "read_test_metrics", "arguments": {"model_id": "v5"}},
                        {"id": "a2", "name": "list_models", "arguments": {}}]},
        {"role": "tool", "tool_call_id": "a1", "name": "read_test_metrics", "content": "{\"ok\":1}"},
        {"role": "tool", "tool_call_id": "a2", "name": "list_models", "content": "{\"ok\":2}"},
    ]

    def test_openai_shape(self):
        msgs, tools_ = llm.render_openai("SYS", self.TX, [])
        self.assertEqual(msgs[0]["role"], "system")
        asst = msgs[2]
        self.assertEqual(asst["tool_calls"][0]["function"]["arguments"], '{"model_id": "v5"}')
        self.assertEqual(msgs[3]["role"], "tool")
        self.assertTrue(all(t["type"] == "function" for t in tools_))

    def test_anthropic_shape_and_parallel_merge(self):
        msgs, tools_ = llm.render_anthropic("SYS", self.TX, [])
        asst = msgs[1]
        self.assertEqual(asst["content"][0]["type"], "tool_use")
        user = msgs[2]  # 两条并行 tool_result 合并进同一 user 轮(Anthropic 要求)
        self.assertEqual(user["role"], "user")
        self.assertEqual(len(user["content"]), 2)
        self.assertEqual(user["content"][0]["type"], "tool_result")
        self.assertTrue(all("input_schema" in t for t in tools_))


@unittest.skipUnless(HAVE_DEPS, "advisor deps missing")
class FriendlyDisplay(unittest.TestCase):
    """展示层安全网:内部路径/评测件名/model_id 技术名一律不得出境。"""

    LEAKS = ("outputs/", "clean/", "data_analysis/", ".json", ".csv", ".joblib",
             "context-weather", "context-baseline", "test_metrics", "[来源]")

    def test_scrub_covers_mock_output(self):
        r = agent.ask("v5 模型在 TEST 段误报了几条?", backend="mock")
        out = friendly.scrub(r["answer"])
        for leak in self.LEAKS:
            self.assertNotIn(leak, out, f"mock 正文出境泄露 {leak}:\n{out}")

    def test_scrub_bare_filename_and_tags(self):
        dirty = ("详见 test_metrics_context-weather-fixedthr-v5.json,"
                 "config 在 outputs/ml_anomaly/alerts 与 alerts_v5.csv。[来源]完了")
        clean = friendly.scrub(dirty)
        for leak in ("test_metrics", ".json", ".csv", "context-weather", "outputs/", "[来源]"):
            self.assertNotIn(leak, clean)
        self.assertIn("盲评考试记录", clean)

    def test_label_mapping(self):
        self.assertEqual(friendly.label_for("outputs/ml_advisor/alerts_v5.csv"), "异常告警清单")
        self.assertEqual(friendly.label_for("clean/queue_entries"), "排队记录")
        self.assertEqual(friendly.label_for("data_analysis/ml/README.md"), "模型登记簿")
        self.assertIn("盲评", friendly.label_for("test_metrics_context-weather-fixedthr-v5.json"))

    def test_humanize_drops_backend_identity(self):
        h = friendly.humanize({"answer": "见 outputs/ml_anomaly/x.json 与 [来源]",
                               "citations": ["outputs/ml_anomaly/test_metrics_context-weather-fixedthr-v5.json"],
                               "rounds": 2, "backend": "openai", "trace": []})
        self.assertNotIn("backend", h)  # 供应商身份不下发(仅 DEBUG 模式的 debug 字段里才有)
        self.assertEqual(h["mode"], "online")
        self.assertEqual(h["sources"], ["异常检测·盲评考试记录"])

    def test_scrub_windows_paths_and_traceback_text(self):
        # 验收泄露一号:解释器 ImportError 原文(绝对路径含用户名/中文目录)不得出境
        dirty = ("cannot import name 'detect_v3' from 'data_analysis.ml.anomaly' "
                 "(C:\\Users\\ling\\Desktop\\某目录\\data_analysis\\ml\\anomaly\\__init__.py)")
        clean = friendly.scrub(dirty)
        for leak in ("C:\\", "Users", "ling", "data_analysis", "__init__", "detect_v3"):
            self.assertNotIn(leak, clean, clean)

    def test_scrub_rule_parrot_words(self):
        # 验收泄露二号:模型复读禁规清单时的裸词也要拦(扩展名/别名/条款号/供应商/env/key 残片)
        dirty = ("规则 1a:禁用 .json/.csv、hgb-q50、context 前缀技术名;"
                 "调 deepseek 或 openai;改 ML_ADVISOR_MAX_STEPS;详见 README;key sk-ab***cd")
        clean = friendly.scrub(dirty)
        for leak in (".json", ".csv", "hgb-q50", "deepseek", "openai",
                     "ML_ADVISOR", "README", "规则 1", "sk-"):
            self.assertNotIn(leak, clean, clean)


@unittest.skipUnless(NEED_ARTIFACTS, "outputs/ml_anomaly artifacts absent")
class ExplainSessionChain(unittest.TestCase):
    """归因链体检:signals_v3 定义冻结副本重算的分,必须与冻结告警清单逐条对得上。"""

    @classmethod
    def setUpClass(cls):
        artifacts.prepare()

    def test_recompute_matches_frozen_alerts(self):
        import pandas as pd

        df = pd.read_csv(config.ALERTS_CSV)
        top = df.sort_values("score", ascending=False).iloc[0]
        out = json.loads(tools.execute("explain_session", {"session_id": top["session_id"]}))
        self.assertNotIn("error", out)
        self.assertAlmostEqual(out["score"], float(top["score"]), places=3)
        self.assertTrue(out["alerted"])
        self.assertIn("outputs/ml_anomaly/context-weather-fixedthr-v5.joblib", out["sources"])

    def test_humanized_sources_are_friendly(self):
        import pandas as pd

        sid = str(pd.read_csv(config.ALERTS_CSV).iloc[0]["session_id"])
        payload = json.loads(tools.execute("explain_session", {"session_id": sid}))
        h = friendly.humanize({"answer": json.dumps(payload, ensure_ascii=False),
                               "citations": payload["sources"],
                               "rounds": 2, "backend": "mock", "trace": []})
        self.assertTrue(any("模型配置" in s for s in h["sources"]), h["sources"])
        self.assertTrue(any("充电会话" in s for s in h["sources"]), h["sources"])
        for leak in ("outputs/", "clean/", "context-weather", ".joblib"):
            self.assertNotIn(leak, h["answer"])


@unittest.skipUnless(HAVE_DEPS, "advisor deps missing")
class PageActions(unittest.TestCase):
    """页面动作白名单:校验、防劫持、出境终检与队友 DOM 对账(不依赖冻结工件)。"""

    def test_specs_registered(self):
        names = {s["name"] for s in tools.SPECS}
        self.assertIn("page_action", names)
        spec = next(s for s in tools.SPECS if s["name"] == "page_action")
        self.assertEqual(spec["parameters"]["required"], ["kind", "target"])

    def test_normalize_whitelist(self):
        act, err = actions.normalize({"kind": "navigate", "target": "lab-insights"})
        self.assertEqual(err, "")
        self.assertEqual(act, {"kind": "navigate", "target": "lab-insights",
                               "route": "lab", "section": "insights", "label": "智能分析·用户与异常"})
        # 模型照 SPEC 描述传中文 label 也是正路
        by_label, _ = actions.normalize({"kind": "navigate", "target": "智能分析·用户与异常"})
        self.assertEqual(by_label, act)
        self.assertEqual(actions.normalize({"kind": "navigate", "target": "admin"})[0], None)
        self.assertEqual(actions.normalize({"kind": "navigate", "target": "dashboard; rm -rf"})[0], None)
        self.assertEqual(actions.normalize({"kind": "click", "target": "overview"})[0], None)
        # route 永远由注册表定:normalize 根本不读外部 route 字段
        self.assertEqual(actions.normalize({"kind": "navigate", "target": "lab", "route": "admin"})[1], "")

    def test_fill_value_rules(self):
        ok, _ = actions.normalize({"kind": "fill", "target": "dash-start", "value": "2026-01-01"})
        self.assertIsNotNone(ok)
        for bad in ({"kind": "fill", "target": "dash-start", "value": "2026/01/01"},
                    {"kind": "fill", "target": "forecast-target", "value": "power"},
                    {"kind": "fill", "target": "energy-kwh", "value": "7"},
                    {"kind": "fill", "target": "insights-query", "value": ""},
                    {"kind": "fill", "target": "session-name", "value": "a\nb"},
                    {"kind": "fill", "target": "insights-query", "value": "x" * 41}):
            act, reason = actions.normalize(bad)
            self.assertIsNone(act, bad)
            self.assertTrue(reason)

    def test_machine_fields_survive_scrub(self):
        """scrub 会改数据不遮数据,动作 machine 字段必须逐字穿过——钉死不变量。"""
        m = actions.manifest()
        words = [t[k] for t in list(m["_nav"].values()) + list(m["_fill"].values())
                 for k in ("id", "route", "label", "section") if t.get(k)]
        words += [v for t in m["_fill"].values() for v in t["valueRule"].get("enum", [])]
        words += ["SES-00111676", "2026-01-01T08:30", "load", "availability"]
        for w in words:
            self.assertEqual(friendly.scrub(w), w, f"scrub 改动了动作字段:{w!r}")

    def test_dangerous_words_never_in_manifest_or_specs(self):
        blob = actions.MANIFEST_PATH.read_text(encoding="utf-8") + actions.SPEC["description"]
        blob += "".join(s["description"] for s in tools.SPECS)
        for w in ("推进时钟", "保存策略", "运行实验", "应用筛选", "连接控制台",
                  "管理员", "令牌", "token", "password", "删除", "提交按钮"):
            self.assertNotIn(w, blob, f"白名单清单出现危险词 {w}")

    def test_page_action_handler_shapes(self):
        out = json.loads(tools.execute("page_action", {"kind": "navigate", "target": "overview"}))
        self.assertTrue(out["ok"])
        self.assertEqual(out["ui_actions"][0]["route"], "dashboard")
        bad = json.loads(tools.execute("page_action", {"kind": "navigate", "target": "admin-console"}))
        self.assertIn("error", bad)

    def test_mock_navigate_end_to_end(self):
        r = agent.ask("打开智能分析的用户与异常页", backend=mock.MockBackend())
        self.assertEqual([a["target"] for a in r["actions"]], ["lab-insights"])  # 页名链取最深段
        self.assertIn("用户与异常", r["answer"])
        self.assertNotIn("无依据", r["answer"])
        self.assertEqual([t["tool"] for t in r["trace"]], ["page_action"])

    def test_mock_nav_intent_never_hijacks_data_questions(self):
        for q in ("我前往的站还要等多久?", "打开后顺便看看负荷预测精度如何",
                  "v5 在 TEST 段误报了几条?"):
            r = agent.ask(q, backend=mock.MockBackend())
            self.assertEqual(r["actions"], [], f"数据问句被页面意图劫持:{q}")

    def test_mock_fill_end_to_end_verbatim_value(self):
        r = agent.ask("把SES-00111676填入查询框", backend=mock.MockBackend())
        self.assertEqual(len(r["actions"]), 1)
        a = r["actions"][0]
        self.assertEqual((a["kind"], a["target"], a["value"]),
                         ("fill", "insights-query", "SES-00111676"))  # 逐字,不被改写
        self.assertIn("未提交", r["answer"])
        # 正文刻意不内联 value:此刻还没填(点芯片才执行),且正文过 scrub、
        # 动作不过——内联会造成"说的"与芯片"填的"分叉(评审实锤)。值由芯片逐字展示。
        self.assertNotIn("SES-00111676", r["answer"])
        self.assertIn("芯片", r["answer"])

    def test_humanize_narrows_and_drops_tampered(self):
        good = {"kind": "navigate", "target": "lab", "route": "lab",
                "section": None, "label": "智能分析"}
        tampered_route = dict(good, route="admin")
        fake_label = {"kind": "fill", "target": "insights-query", "route": "lab",
                      "section": "insights", "value": "张三", "label": "恶意注入"}
        bad_value = {"kind": "fill", "target": "energy-kwh", "route": "explore",
                     "section": None, "value": "999", "label": "计划补电(kWh)"}
        ghost = {"kind": "navigate", "target": "admin", "route": "admin", "label": "控制台"}
        h = friendly.humanize({"answer": "好", "citations": [], "actions":
                               [good, tampered_route, fake_label, bad_value, ghost],
                               "rounds": 1, "backend": "mock", "trace": []})
        # 串 route 丢、值域外丢、未知 target 丢;label 不许信模型措辞,由注册表覆盖
        self.assertEqual(h["actions"],
                         [good, {"kind": "fill", "target": "insights-query", "route": "lab",
                                 "section": "insights", "value": "张三", "label": "分析编号查询框"}])
        self.assertEqual(friendly.humanize({"answer": "x", "actions": None,
                                            "citations": [], "rounds": 1,
                                            "backend": "mock", "trace": []})["actions"], [])

    def test_frontend_manifest_agreement(self):
        """防漂移:advisorActions.ts 与 manifest 的 route/section 集合必须同源(读源码对账)。"""
        import re as _re
        from pathlib import Path
        ts = (Path(actions.__file__).parent.parent.parent / "frontend" / "src" / "advisorActions.ts"
              ).read_text(encoding="utf-8")
        m = actions.manifest()
        ts_routes = set(_re.findall(r'route:\s*"([\w-]+)"', ts))
        self.assertEqual(ts_routes, {t["route"] for t in m["_nav"].values()} |
                         {t["route"] for t in m["_fill"].values()})
        self.assertNotIn("admin", ts_routes)  # 管理员页永不在前端导航注册表
        # navigate:前端用 TAB_ORDER/SEC_ORDER 序号拼 nth-child,校验 manifest 序号与顺序一致
        tabs = _re.findall(r'"([\w-]+)"', ts.split("TAB_ORDER")[1].split("]")[0])
        secs = _re.findall(r'"([\w-]+)"', ts.split("SEC_ORDER")[1].split("]")[0])
        self.assertNotIn("admin", tabs)
        for t in m["_nav"].values():
            # TS 对象键可省引号(overview: {...}),带引号/不带都算认
            self.assertRegex(ts, rf'"?{t["id"]}"?\s*:\s*{{', f"前端注册表缺 {t['id']}")
            self.assertIn(t["label"], ts, f"前端 label 漂移:{t['label']}")
            n = int(_re.search(r"nth-child\((\d+)\)", t["selector"]).group(1))
            order = tabs if "main-nav" in t["selector"] else secs
            self.assertEqual(order[n - 1], t.get("section") or t["route"],
                             f"nth-child({n}) 与前端顺序对不上:{t['id']}")
        # fill:前端逐条携带 selector,直接字面比对
        for t in m["_fill"].values():
            self.assertIn(f'"{t["id"]}":', ts, f"前端注册表缺 {t['id']}")
            self.assertIn(t["label"], ts, f"前端 label 漂移:{t['label']}")
            self.assertIn(t["selector"], ts, f"前端 selector 漂移:{t['id']}")
            self.assertIn(f'"{t["event"]}"', ts)

    def test_app_vue_tab_ids_match_manifest(self):
        """队友 App.vue 的 tabs/labSections 字面量是 selector 的事实源,漂移即红。"""
        import re as _re
        from pathlib import Path
        app = (Path(actions.__file__).parent.parent.parent / "frontend" / "src" / "App.vue"
               ).read_text(encoding="utf-8")
        tabs_list = _re.findall(r'id:\s*"([\w-]+)"', app.split("const tabs")[1].split("];")[0])
        labs = set(_re.findall(r"id:\s*'([\w-]+)'", app.split("const labSections")[1].split("];")[0]))
        m = actions.manifest()
        self.assertEqual(set(tabs_list) - {"admin"}, {t["route"] for t in m["_nav"].values()})
        self.assertEqual(labs, {t["section"] for t in m["_nav"].values() if t.get("section")})
        self.assertNotIn("admin", json.dumps(m["_nav"], ensure_ascii=False)
                         + json.dumps(m["_fill"], ensure_ascii=False))
        # 顶层页签 nth-child 必须对上 App.vue tabs 的**实际顺序**(不是集合)——
        # #76 删 trip 页签时 lab 从第 4 位移到第 3 位,只有顺序断言抓得住
        for t in m["_nav"].values():
            if "main-nav" in t["selector"]:
                n = int(_re.search(r"nth-child\((\d+)\)", t["selector"]).group(1))
                self.assertEqual(tabs_list[n - 1], t["route"],
                                 f"页签顺序漂移:{t['id']} 指第 {n} 位,{tabs_list}")
        # 分区按钮序号对 App.vue labSections 顺序
        labs_list = _re.findall(r"id:\s*'([\w-]+)'", app.split("const labSections")[1].split("];")[0])
        for t in m["_nav"].values():
            if "workspace-tabs" in t["selector"]:
                n = int(_re.search(r"nth-child\((\d+)\)", t["selector"]).group(1))
                self.assertEqual(labs_list[n - 1], t["section"],
                                 f"分区顺序漂移:{t['id']} 指第 {n} 位,{labs_list}")


    def test_serve_http_guards(self):
        """评审实锤的 CSRF 面:Origin 闸、Content-Type 锁 JSON、backend 只许降不许升、
        GET 便车删净——四条各钉一枚(真回环随机端口,不触发 LLM 请求)。"""
        import threading
        from urllib.error import HTTPError
        from urllib.request import Request, urlopen
        from data_analysis.ml.advisor import serve
        # 纯函数层
        self.assertTrue(serve.origin_allowed(None))          # 非浏览器客户端(curl/driver)
        self.assertTrue(serve.origin_allowed("http://localhost:5173"))
        self.assertTrue(serve.origin_allowed("http://127.0.0.1:8765"))
        self.assertFalse(serve.origin_allowed("https://evil.example"))
        self.assertFalse(serve.origin_allowed("http://localhost:5173.evil.com"))  # 后缀拼接坑
        self.assertFalse(serve.origin_allowed("null"))
        self.assertEqual(serve.request_backend("mock"), "mock")
        for bad in ("openai", "anthropic", "MOCK", "", None, 42):  # 只认逐字 "mock"
            self.assertIsNone(serve.request_backend(bad), bad)     # 不许被外部强制起在线计费
        # 路由层(起真壳打请求)
        with serve._NoDoubleBind(("127.0.0.1", 0), serve.Handler) as httpd:
            port = httpd.server_address[1]
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            base = f"http://127.0.0.1:{port}"
            try:
                self.assertEqual(urlopen(base + "/health").status, 200)
                with self.assertRaises(HTTPError) as e:
                    urlopen(base + "/advisor/ask?q=hi")
                self.assertEqual(e.exception.code, 405)            # GET 便车已删
                with self.assertRaises(HTTPError) as e:
                    urlopen(Request(base + "/advisor/ask", data=b'{"question":"hi"}',
                                    headers={"Content-Type": "text/plain"}))
                self.assertEqual(e.exception.code, 415)            # form 简单请求拼不进来
                with self.assertRaises(HTTPError) as e:
                    urlopen(Request(base + "/advisor/ask", data=b'{"question":"hi"}',
                                    headers={"Content-Type": "application/json",
                                             "Origin": "https://evil.example"}))
                self.assertEqual(e.exception.code, 403)
            finally:
                httpd.shutdown()

    def test_collect_actions_caps_and_dedup(self):
        """收集层:等值去重 + 封顶(跳转≤1、预填≤2);畸形输入一律不收。"""
        def payload(acts):
            return json.dumps({"ok": True, "ui_actions": acts}, ensure_ascii=False)
        nav = {"kind": "navigate", "target": "lab", "route": "lab", "section": None, "label": "智能分析"}
        nav2 = {"kind": "navigate", "target": "explore", "route": "explore", "section": None, "label": "智能找站"}
        f1 = {"kind": "fill", "target": "dash-start", "route": "dashboard", "section": None,
              "value": "2026-01-01", "label": "统计开始日期"}
        f2 = dict(f1, target="dash-end", value="2026-01-31", label="统计结束日期")
        f3 = dict(f1, target="insights-query", value="SES-1", route="lab",
                  section="insights", label="分析编号查询框")
        sink: list[dict] = []
        agent._collect_actions(payload([nav]), sink)
        agent._collect_actions(payload([nav]), sink)              # 跨轮重复调用:去重
        self.assertEqual(sink, [nav])
        agent._collect_actions(payload([nav2]), sink)             # 第二条跳转:封顶丢弃
        self.assertEqual([a["target"] for a in sink], ["lab"])
        agent._collect_actions(payload([f1, f2, f3]), sink)       # 预填收两条即封
        self.assertEqual([a["kind"] for a in sink], ["navigate", "fill", "fill"])
        self.assertEqual([a["target"] for a in sink][1:], ["dash-start", "dash-end"])
        sink.clear()
        agent._collect_actions("not-json", sink)
        agent._collect_actions(json.dumps({"ok": True}), sink)                       # 缺键
        agent._collect_actions(json.dumps({"ok": True, "ui_actions": "x"}), sink)    # 非列表
        agent._collect_actions(payload(["string", 42]), sink)  # 非 dict 元素不收
        self.assertEqual(sink, [])
        # 缺 target 的 dict 会进 sink——设计如此:收集层只收 page_action 工具回执形状,
        # 语义终闸在 actions.normalize(工具层已拦)与 friendly._narrow_action(出境必丢,
        # 见 test_humanize_narrows_and_drops_tampered),这里不做重复校验
        agent._collect_actions(payload([{"kind": "navigate"}]), sink)
        self.assertEqual(sink, [{"kind": "navigate"}])

    def test_humanize_caps_actions(self):
        """出境层再封一次顶:绕过收集层也最多 1 跳转 + 2 预填;label 永远注册表版。"""
        nav = {"kind": "navigate", "target": "lab", "route": "lab",
               "section": None, "label": "点这里领 100 元"}
        fills = [{"kind": "fill", "target": "insights-query", "route": "lab",
                  "section": "insights", "value": f"SES-{i}"} for i in range(4)]
        h = friendly.humanize({"answer": "x", "citations": [],
                               "actions": [nav, dict(nav), *fills],
                               "rounds": 1, "backend": "mock", "trace": []})
        self.assertEqual([a["kind"] for a in h["actions"]], ["navigate", "fill", "fill"])
        self.assertEqual(h["actions"][0]["label"], "智能分析")

    def test_tools_error_texts_no_bare_table_names(self):
        """工具 error 会被 mock/真模型逐字转述进正文(scrub 不遮裸表名)——
        错误文案必须人话化,这条钉死三个重灾区。"""
        for tool, args in (("queue_summary", {"station_id": "ST-BJ-99"}),
                           ("lookup_weather_calendar", {"city_id": "C9", "date": "2026-01-01"}),
                           ("review_feedback", {"station_id": "ST-BJ-99"})):
            err = json.loads(tools.execute(tool, args)).get("error", "")
            self.assertTrue(err, f"{tool} 应返回 error 文案")
            for bare in ("queue_entries", "weather_hourly", "reviews", "clean/"):
                self.assertNotIn(bare, err, f"{tool} 错误文案泄露裸表名:{err}")

    def test_scrub_posix_absolute_paths(self):
        # 评审补刀:绝对路径网不能只捞 Windows 盘符,WSL/Linux 队友的报错同样含用户名
        dirty = ("Traceback: File \"/home/ling/.venv/lib/site.py\" line 9, in <module>\n"
                 "打开 /mnt/c/Users/ling/Desktop/某文件 失败")
        clean = friendly.scrub(dirty)
        for leak in ("/home/", "/mnt/", "ling", ".venv", "site.py"):
            self.assertNotIn(leak, clean, clean)

    def test_frontend_dom_anchors_match_manifest(self):
        """selector 对账补洞(评审 major):三拷贝字符串互证只钉住 JSON/TS/App.vue
        脚本数组;真正拥有 DOM 的是子组件模板——队友下一次 #76 式改名必须让 CI 红,
        而不是让用户运行时得到"输入框还没出现"。"""
        import re
        from pathlib import Path
        fe = Path(actions.__file__).parent.parent.parent / "frontend" / "src"
        blob = "\n".join(p.read_text(encoding="utf-8") for p in sorted(fe.rglob("*.vue")))
        m = actions.manifest()
        for anchor in ("primary-header", "main-nav", "workspace-tabs", "智能分析分区", "app-shell"):
            self.assertIn(anchor, blob, f"跳转容器锚点在模板里查无:{anchor}")
        for t in list(m["_nav"].values()) + list(m["_fill"].values()):
            for tok in re.findall(r"[.#]([\w-]+)", t["selector"]):
                self.assertIn(tok, blob, f"{t['id']} 的 selector 锚点 {tok!r} 在 .vue 源文本里查无此物")
            for lab in re.findall(r'aria-label="([^"]+)"', t["selector"]):
                self.assertIn(lab, blob, f"{t['id']} 的 aria-label 漂移:{lab}")
        # 执行器依赖的交互锚点(scrollAfter 与就绪门控,清单不携带,单独钉)
        for anchor in ("paired-experiment-results", "insights-grid", "预测电站", "intelligence-inline-error"):
            self.assertIn(anchor, blob, f"交互锚点漂移:{anchor}")


@unittest.skipUnless(NEED_ARTIFACTS and os.environ.get("ML_ADVISOR_LIVE") == "1",
                     "set ML_ADVISOR_LIVE=1 (and .env key) to spend real tokens")
class LiveSmoke(unittest.TestCase):
    def test_deepseek_roundtrip(self):
        r = agent.ask("v5 模型在 TEST 段的精确率/召回是多少?给出出处。", backend="openai")
        self.assertTrue(r["answer"].strip())
        self.assertTrue(r["citations"], "真实模型也必须带出处")


if __name__ == "__main__":
    unittest.main()
