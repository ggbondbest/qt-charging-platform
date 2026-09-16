"""Actual snapshot/advanced rollups and ForecastService over tiny test fixtures."""
import asyncio
import importlib.util
import json
import os
import sqlite3
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
from http.client import IncompleteRead
import unittest
from unittest.mock import patch

HAS_DEPS = all(importlib.util.find_spec(name) for name in ("fastapi", "httpx", "pandas", "numpy", "sklearn", "joblib", "pyarrow"))
if HAS_DEPS:
    from fastapi import FastAPI
    from fastapi.testclient import TestClient
    from data_analysis.backend.errors import ApiError
    from data_analysis.delivery.advisor import AdvisorRunner, AdvisorRequest
    from data_analysis.delivery.app import create_app
    from data_analysis.delivery.models import ForecastService
    from data_analysis.chargepilot.settings import Settings
    from data_analysis.tests.test_analytics_api import fixture
    from data_analysis.tests.test_advanced_api import write_bundle
    from data_analysis.tests.test_delivery_forecasts import FakeAdapter
    from data_analysis.ml.advisor import config, llm, service

PATH = "/api/v1/intelligence/advisor"


class TinyInsights:
    def report(self):
        return {"status": "READY", "anomaly": {"test": {"precision": .5, "recall": .25}}}

    def list_anomalies(self, limit=3):
        return {"total": 1, "inspectedSessions": 4, "items": [self.inspect_session("SES-001")]}

    def inspect_session(self, entity):
        if entity != "SES-001":
            raise KeyError(entity)
        return {"sessionId": entity, "anomalyScore": .8, "threshold": .7, "flagged": True,
                "features": {"private": "never-send"}}


@unittest.skipUnless(HAS_DEPS, "Install unified delivery dependencies")
class DeliveryAdvisorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="delivery-advisor-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / "fixture.sqlite"
        self.meta = fixture(self.db)
        write_bundle(self.root, self.meta)
        self.env = patch.dict(os.environ, {"ANALYTICS_ADVANCED_BUNDLE": str(self.root), "ML_ADVISOR_ONLINE_ENABLED": "0",
            "ML_ADVISOR_ENV_FILE": str(self.root / "not-configured.env")})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.network = patch.object(llm, "build_opener", side_effect=AssertionError("HTTP fixtures must never contact an external model"))
        self.network.start()
        self.addCleanup(self.network.stop)
        self.online = config.OnlineSettings("fake-key", "https://model.example/v1", "configured", "测试模型")
        self.provider = ForecastService(FakeAdapter(), FakeAdapter("availability"), TinyInsights(),
            SimpleNamespace(catalog=[], metadata={}), self.meta)
        self.app = create_app(settings=Settings(), database_path=self.db, provider=self.provider, operational_app=FastAPI())
        self.addCleanup(self.app.state.advisor_runner.close)
        self.client = TestClient(self.app)
        self.addCleanup(self.client.close)

    def ask(self, question, **changes):
        body = dict(question=question, datasetId=self.meta["datasetId"], publishedBatchId=self.meta["publishedBatchId"])
        body.update(changes)
        return self.client.post(PATH, json=body)

    def data(self, response):
        self.assertEqual(response.status_code, 200, response.text)
        payload = response.json()
        self.assertEqual(payload["meta"]["publishedBatchId"], "batch-1")
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(payload["data"]["scope"]["dataKind"], "SIMULATED")
        return payload["data"]

    def test_configuration_is_available_without_model_or_database(self):
        result = self.client.get(PATH).json()["data"]
        self.assertFalse(result["onlineAvailable"])
        self.assertEqual(result["defaultMode"], "offline")
        self.assertEqual(len(result["supportedQuestions"]), 4)
        for disclosed in ("问题", "历史", "聚合", "知识"):
            self.assertIn(disclosed, result["disclosure"])
        with patch.object(config, "online_settings", return_value=self.online), patch.object(self.app.state, "database_path", self.root / "missing.sqlite"):
            configured = self.client.get(PATH).json()["data"]
        self.assertTrue(configured["onlineAvailable"])
        self.assertEqual(configured["defaultMode"], "online")
        self.assertEqual(configured["onlineProvider"], "测试模型")
        self.assertNotIn("fake-key", json.dumps(configured))

    def test_real_advanced_bottleneck_values_and_reproducible_source(self):
        with patch.object(llm, "plan_query", side_effect=AssertionError("offline must not plan with model")), \
                patch.object(llm, "generate_answer", side_effect=AssertionError("offline must not generate with model")):
            result = self.data(self.ask("充电服务瓶颈是什么？"))
        evidence = {row["id"]: row for row in result["evidence"]}
        self.assertEqual(evidence["attempts"]["value"], 3)
        self.assertEqual(evidence["failed"]["value"], 1)
        self.assertEqual(evidence["failure_share"]["value"], 100)
        response = self.client.get(evidence["attempts"]["source"]["endpoint"])
        self.assertEqual(response.json()["data"]["service"]["attemptCount"], evidence["attempts"]["value"])
        self.assertNotIn("actions", result)
        self.assertEqual(result["citations"], [])
        self.assertEqual(result["knowledge"], [])

    def test_behavior_and_station_comparison_use_real_rollup(self):
        behavior = self.data(self.ask("不同用户类型的补能间隔和单次电量？"))
        values = {row["id"]: row["value"] for row in behavior["evidence"]}
        self.assertEqual(values["intervals"], 1)
        self.assertEqual(values["first_observed"], 1)
        self.assertEqual(values["segment_0_interval"], 1)
        stations = self.data(self.ask("比较各站的成功率与等待"))
        values = {row["id"]: row["value"] for row in stations["evidence"]}
        self.assertEqual(values["station_count"], 1)
        self.assertEqual(values["comparable_station_count"], 0)
        self.assertNotIn("station_0_success", values)
        self.assertIn("样本太少", stations["answer"])

    def test_city_mentioned_in_prose_must_match_actual_page_filter(self):
        with sqlite3.connect(self.db) as db:
            db.execute("UPDATE cities SET city_name='北京市' WHERE city_id='C1'")
            db.execute("UPDATE cities SET city_name='上海市' WHERE city_id='C2'")
        for question, filters in [("北京市运营概况", {}), ("北京电量", {"cityId": "C2"}),
                                  ("天津市运营概况", {}), ("北京和上海电量", {"cityId": "C1"}),
                                  ("天津电量是多少", {}), ("天津电量是多少", {"cityId": "C1"}),
                                  ("北京和天津市电量是多少", {"cityId": "C1"}),
                                  ("在未发布地区的电量是多少", {})]:
            result = self.data(self.ask(question, **filters))
            self.assertEqual(result["status"], "unsupported")
            self.assertEqual(result["evidence"], [])
            self.assertIn("城市与页面筛选", result["answer"])
        self.assertEqual(self.data(self.ask("北京市运营概况", cityId="C1"))["status"], "answered")
        self.assertEqual(self.data(self.ask("北京电量", stationId="S1"))["status"], "answered")

    def test_chinese_adjacent_dates_and_identifiers_are_not_ignored(self):
        for question in ("2025-12-02的电量是多少", "查询2025-12-01电量", "2025-12-01至2025-12-04电量", "2025年12月1日电量"):
            result = self.data(self.ask(question))
            self.assertEqual(result["status"], "unsupported")
            self.assertEqual(result["evidence"], [])
            self.assertIn("日期控件", result["answer"])
        self.assertEqual(self.ask("解释异常会话SES-999").status_code, 404)
        self.assertEqual(self.ask("解释异常会话SES-001的情况").status_code, 200)
        for question in ("ST-BJ-01的电量", "查询ST-BJ-01电量", "异常会话SES-001和SES-999"):
            result = self.data(self.ask(question))
            self.assertEqual(result["status"], "unsupported")
            self.assertEqual(result["evidence"], [])

    def test_small_samples_excluded_from_priority_and_bottlenecks_include_context(self):
        data = service.advanced.analyze(self.meta, dict(start="2025-12-01", end="2025-12-04", city=None, station=None))
        base = data["stations"][0]
        data["stations"] = [dict(base, attemptCount=90, successRate=2/3),
                            dict(base, stationId="S2", stationName="单次失败站", attemptCount=1, successRate=0)]
        cell = next(row for row in data["service"]["cells"] if row["attemptCount"])
        data["service"]["cells"] = [dict(cell, attemptCount=90, successfulAttempts=60, successRate=2/3),
                                     dict(cell, hour=1, attemptCount=1, successfulAttempts=0, successRate=0)]
        data["behavior"]["segments"] = [dict(data["behavior"]["segments"][0], userSegment=kind)
                                        for kind in ("COMMUTER", "FAMILY", "RIDE_HAILING", "FLEET")]
        with patch.object(service.advanced, "analyze", return_value=data):
            priority = self.data(self.ask("当前范围内，哪些电站需要优先关注？"))
            evidence = {row["id"]: row for row in priority["evidence"]}
            self.assertEqual(evidence["station_0_attempts"]["value"], 90)
            self.assertNotIn("单次失败站", priority["answer"])
            self.assertEqual(evidence["comparable_station_count"]["value"], 1)
            bottleneck = self.data(self.ask("服务瓶颈"))
            evidence = {row["id"]: row for row in bottleneck["evidence"]}
            self.assertEqual(evidence["cell_failed"]["value"], 30)
            self.assertEqual(evidence["cell_wait"]["value"], 5)
            self.assertEqual(evidence["cell_utilization"]["value"], 30)
            self.assertIn("办公园区 · 00:00", evidence["cell_attempts"]["label"])
            self.assertLess(len(bottleneck["answer"]), 350)
            self.assertGreaterEqual(len(bottleneck["evidence"]), 13)
            self.assertNotIn("完整观测站点小时", bottleneck["answer"])
            self.assertIn("90次尝试", bottleneck["answer"])
            self.assertIn("66.67%", bottleneck["answer"])
            behavior = self.data(self.ask("用户类型补能行为"))
            labels = " ".join(row["label"] for row in behavior["evidence"])
            for label in ("家庭用户", "网约车用户", "营运车队", "通勤用户"):
                self.assertIn(label, labels)
            self.assertNotIn("RIDE_HAILING", labels)
            self.assertLess(len(behavior["answer"]), 260)
            self.assertLessEqual(behavior["answer"].count("单次电量"), 2)
            self.assertGreaterEqual(len(behavior["evidence"]), 11)

    def test_overview_uses_snapshot_service_and_missing_scope_is_honest(self):
        result = self.data(self.ask("运营概况"))
        values = {row["id"]: row["value"] for row in result["evidence"]}
        self.assertEqual(values["energy"], 13)
        empty = self.data(self.ask("补能行为", stationId="S2"))
        self.assertEqual(empty["status"], "no_evidence")
        empty_stations = self.data(self.ask("比较各站成功率", stationId="S2"))
        self.assertEqual(empty_stations["status"], "no_evidence")
        unknown = self.data(self.ask("今天光伏发电量是多少？"))
        self.assertEqual(unknown["status"], "unsupported")
        self.assertEqual(unknown["evidence"], [])

    def test_real_forecast_provider_batch_check_and_historical_model_scope(self):
        models = self.data(self.ask("目前有哪些模型？", stationId="S1", startDate="2025-12-02"))
        self.assertTrue(any("不改变模型" in note for note in models["limitations"]))
        self.assertIn("precision", {row["id"] for row in models["evidence"]})
        anomaly = self.data(self.ask("解释异常会话 SES-001"))
        self.assertEqual(anomaly["evidence"][0]["value"], .8)
        self.assertNotIn("never-send", json.dumps(anomaly))
        self.assertEqual(self.ask("解释异常会话 SES-999").status_code, 404)
        with patch.object(self.provider.insights, "list_anomalies", return_value={"total": 0, "inspectedSessions": 4, "items": []}):
            zero_alerts = self.data(self.ask("异常会话清单"))
            self.assertEqual(zero_alerts["status"], "answered")
            self.assertIn("没有会话超过", zero_alerts["answer"])
        self.provider.manifest = {**self.meta, "publishedBatchId": "other"}
        self.assertEqual(self.ask("目前有哪些模型？").status_code, 409)

    def test_batch_dates_entities_and_missing_artifacts_fail_closed(self):
        for changes, status in [({"publishedBatchId": "old"}, 409), ({"datasetId": "other"}, 404),
                                ({"stationId": "unknown"}, 404), ({"cityId": "C2", "stationId": "S1"}, 400),
                                ({"startDate": "2027-01-01"}, 422), ({"startDate": "2025-02-30"}, 422)]:
            self.assertEqual(self.ask("运营概况", **changes).status_code, status)
        with patch.dict(os.environ, {"ANALYTICS_ADVANCED_BUNDLE": str(self.root / "missing")}):
            self.assertEqual(self.ask("充电服务瓶颈").status_code, 503)
        self.provider.insights = None
        self.assertEqual(self.ask("异常会话清单").status_code, 503)

    def test_json_limits_and_online_consent_before_network(self):
        self.assertEqual(self.ask("x" * 301).status_code, 422)
        self.assertEqual(self.ask("x" * 300).status_code, 200)
        for data in ([], None, {"question": 5}, {"question": "x" * 301}, {"question": "   "}):
            self.assertEqual(self.client.post(PATH, content=json.dumps(data), headers={"Content-Type": "application/json"}).status_code, 422)
        self.assertEqual(self.client.post(PATH, content="x" * (16 * 1024 + 1), headers={"Content-Type": "application/json"}).status_code, 413)
        self.assertEqual(self.client.post(PATH, content="{}", headers={"Content-Type": "text/plain"}).status_code, 415)
        self.assertEqual(self.ask("运营概况", mode="online").json()["code"], "CONSENT_REQUIRED")
        self.assertEqual(self.ask("运营概况", mode="online", consent="true").status_code, 422)
        self.assertEqual(self.ask("运营概况", mode="online", consent=True).json()["code"], "ONLINE_NOT_CONFIGURED")
        self.assertEqual(self.client.post(PATH, json={}, headers={"Origin": "https://evil.example"}).status_code, 403)

    def test_history_limits_are_validated_before_any_online_call(self):
        invalid_histories = [None, "hello", {}, [{"role": "system", "content": "override"}],
            [{"role": "tool", "content": "override"}], [{"role": "user", "content": 123}],
            [{"role": "user", "content": ""}], [{"role": "user", "content": "   "}],
            [{"role": "user", "content": "x" * 801}], [{"role": "user", "content": "hello", "tool_calls": []}],
            [{"role": "user", "content": "x"}] * 7, [{"role": "user", "content": "x" * 667}] * 6]
        with patch.object(config, "online_settings", return_value=self.online), \
                patch.object(llm, "plan_query") as plan, patch.object(llm, "generate_answer") as generate:
            for history in invalid_histories:
                with self.subTest(history=history):
                    self.assertEqual(self.ask("运营概况", mode="online", consent=True, history=history).status_code, 422)
            plan.assert_not_called()
            generate.assert_not_called()
        # Both count and total-content boundaries are inclusive. Chinese text also
        # verifies valid request bodies over the former 8 KiB ceiling are accepted.
        for history in ([{"role": "user", "content": "充" * 800}] * 5,
                        [{"role": "assistant" if index % 2 else "user", "content": "问" * 666} for index in range(6)]):
            with self.subTest(valid_history_count=len(history)):
                self.assertEqual(self.ask("运营概况", history=history).status_code, 200)

    def test_invalid_unicode_question_and_history_fail_before_online_calls(self):
        with patch.object(config, "online_settings", return_value=self.online), \
                patch.object(llm, "plan_query") as plan, patch.object(llm, "generate_answer") as generate:
            for field in ("question", "history"):
                for codepoint in (0xD83D, 0xDE00):
                    with self.subTest(field=field, codepoint=hex(codepoint)):
                        body = dict(question="运营概况", datasetId=self.meta["datasetId"],
                            publishedBatchId=self.meta["publishedBatchId"], mode="online", consent=True)
                        body[field] = chr(codepoint) if field == "question" else [{"role": "user", "content": chr(codepoint)}]
                        # Send literal JSON escapes so the request reaches the API;
                        # encoding a Python surrogate directly would fail in the client.
                        response = self.client.post(PATH, content=json.dumps(body).encode("ascii"),
                            headers={"Content-Type": "application/json"})
                        self.assertEqual(response.status_code, 422)
            plan.assert_not_called()
            generate.assert_not_called()

    def test_online_consent_and_configuration_guard_both_model_stages(self):
        with patch.object(llm, "plan_query") as plan, patch.object(llm, "generate_answer") as generate:
            with patch.object(config, "online_settings", return_value=self.online):
                for consent in (False, None, "true"):
                    with self.subTest(consent=consent):
                        response = self.ask("你好", mode="online", consent=consent,
                            history=[{"role": "user", "content": "运营概况"}])
                        self.assertEqual(response.status_code, 422)
                self.assertEqual(self.ask("你好", mode="online").json()["code"], "CONSENT_REQUIRED")
            self.assertEqual(self.ask("你好", mode="online", consent=True).json()["code"], "ONLINE_NOT_CONFIGURED")
            plan.assert_not_called()
            generate.assert_not_called()

    def test_online_retrieves_real_aggregates_and_knowledge_with_verifiable_citations(self):
        history = [{"role": "user", "content": "我想了解当前筛选的服务表现"},
                   {"role": "assistant", "content": "可以从充电尝试和失败原因开始。"}]
        def generated(settings, payload, **kwargs):
            self.assertTrue(payload["knowledge"])
            return {"answer": "当前范围有3次充电尝试，未成功开始1次；请结合失败原因分母复核。",
                    "citations": ["bottlenecks.attempts", payload["knowledge"][0]["id"]]}
        with patch.object(config, "online_settings", return_value=self.online), \
                patch.object(llm, "plan_query", return_value={"kind": "analysis", "topics": ["bottlenecks", "overview"]}) as plan, \
                patch.object(llm, "generate_answer", side_effect=generated) as generate:
            result = self.data(self.ask("充电服务瓶颈是什么？", mode="online", consent=True, history=history))
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["mode"], "online")
        self.assertEqual(plan.call_args.args[1:3], ("充电服务瓶颈是什么？", history))
        safe_scope = plan.call_args.args[3]
        for key in ("datasetId", "publishedBatchId", "cityId", "stationId"):
            self.assertNotIn(key, safe_scope)
        payload = generate.call_args.args[1]
        self.assertEqual(set(payload), {"question", "history", "scope", "kind", "evidence", "knowledge"})
        self.assertEqual(payload["question"], "充电服务瓶颈是什么？")
        self.assertEqual(payload["history"], history)
        self.assertNotIn("never-send", json.dumps(payload, ensure_ascii=False))
        self.assertTrue(all("publishedBatchId" not in row["source"]["endpoint"] for row in payload["evidence"]))
        evidence = {row["id"]: row for row in result["evidence"]}
        self.assertEqual(evidence["bottlenecks.attempts"]["value"], 3)
        self.assertEqual(evidence["bottlenecks.failed"]["value"], 1)
        self.assertEqual(evidence["overview.energy"]["value"], 13)
        source = self.client.get(evidence["bottlenecks.attempts"]["source"]["endpoint"])
        self.assertEqual(source.json()["data"]["service"]["attemptCount"], 3)
        knowledge = {row["id"]: row for row in result["knowledge"]}
        self.assertEqual(set(result["citations"]), {"bottlenecks.attempts", payload["knowledge"][0]["id"]})
        self.assertTrue(set(result["citations"]) <= evidence.keys() | knowledge.keys())
        for row in knowledge.values():
            self.assertEqual(set(row), {"id", "title", "text", "source"})
            self.assertTrue(all(isinstance(value, str) and value for value in row.values()))
        self.assertNotIn("actions", result)

    def test_online_explains_retrieved_knowledge_without_inventing_data_evidence(self):
        question = "充电利用率是怎么计算的？缺测为什么不能算零？"
        def generated(settings, payload, **kwargs):
            self.assertEqual(payload["evidence"], [])
            self.assertIn("knowledge.utilization", {row["id"] for row in payload["knowledge"]})
            return {"answer": "利用率按完整小时的充电样本与全部桩样本计算；缺失遥测不填零。",
                    "citations": ["knowledge.utilization"]}
        with patch.object(config, "online_settings", return_value=self.online), \
                patch.object(llm, "plan_query", return_value={"kind": "explanation", "topics": []}), \
                patch.object(llm, "generate_answer", side_effect=generated):
            result = self.data(self.ask(question, mode="online", consent=True))
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["evidence"], [])
        self.assertEqual(result["citations"], ["knowledge.utilization"])
        self.assertTrue(result["knowledge"])

    def test_online_chat_uses_model_reply_without_claiming_statistical_evidence(self):
        with patch.object(config, "online_settings", return_value=self.online), \
                patch.object(llm, "plan_query", return_value={"kind": "chat", "topics": []}), \
                patch.object(llm, "generate_answer", return_value={"answer": "你好！想先了解哪方面的运营情况？", "citations": []}) as generate, \
                patch.object(service.advanced, "analyze", side_effect=AssertionError("chat must not query aggregates")):
            result = self.data(self.ask("你好", mode="online", consent=True))
        self.assertEqual(result["status"], "chat")
        self.assertEqual(result["answer"], "你好！想先了解哪方面的运营情况？")
        for key in ("evidence", "knowledge", "citations"):
            self.assertEqual(result[key], [])
        self.assertEqual(generate.call_args.args[1]["kind"], "chat")

    def test_online_rejects_unknown_or_missing_citations_instead_of_publishing_claims(self):
        def generated(payload, case):
            citations = {"unknown": ["not-retrieved"], "missing": [],
                         "knowledge_only": [payload["knowledge"][0]["id"]]}[case]
            return {"answer": "未经核验的结论", "citations": citations}
        for case in ("unknown", "missing", "knowledge_only"):
            with self.subTest(case=case), patch.object(config, "online_settings", return_value=self.online), \
                    patch.object(llm, "plan_query", return_value={"kind": "analysis", "topics": ["bottlenecks"]}), \
                    patch.object(llm, "generate_answer", side_effect=lambda settings, payload, **kwargs: generated(payload, case)):
                response = self.ask("充电服务瓶颈是什么？", mode="online", consent=True)
            self.assertEqual(response.status_code, 502)
            self.assertEqual(response.json()["code"], "ONLINE_INVALID_RESPONSE")
            self.assertNotIn("未经核验的结论", response.text)

    def test_online_timeout_at_either_model_stage_is_safe(self):
        for stage in ("plan_query", "generate_answer"):
            with self.subTest(stage=stage), patch.object(config, "online_settings", return_value=self.online), \
                    patch.object(llm, "plan_query", return_value={"kind": "analysis", "topics": ["bottlenecks"]}), \
                    patch.object(llm, "generate_answer", return_value={"answer": "unused", "citations": []}), \
                    patch.object(llm, stage, side_effect=ApiError(504, "ADVISOR_TIMEOUT", "在线模型响应超时")):
                response = self.ask("充电服务瓶颈", mode="online", consent=True)
            self.assertEqual(response.status_code, 504)
            self.assertEqual(response.json()["code"], "ADVISOR_TIMEOUT")

    def test_online_read_failure_does_not_become_snapshot_unavailable(self):
        settings = config.OnlineSettings("private-key", "https://model.example/v1", "configured", "测试模型")
        for error in (IncompleteRead(b"private-key", 10), ConnectionResetError("private-key")):
            with patch.object(config, "online_settings", return_value=settings), patch.object(llm, "build_opener") as opener:
                opener.return_value.open.return_value.__enter__.return_value.read.side_effect = error
                response = self.ask("充电服务瓶颈", mode="online", consent=True)
            self.assertEqual(response.status_code, 502)
            self.assertEqual(response.json()["code"], "ONLINE_UNAVAILABLE")
            self.assertNotIn("private-key", response.text)

    def test_timeout_retains_busy_slot_until_actual_worker_exits(self):
        async def scenario():
            runner = AdvisorRunner(workers=1, timeout=.03)
            gate = threading.Event()
            try:
                with self.assertRaises(ApiError) as first:
                    await runner.run(lambda: gate.wait(1))
                self.assertEqual(first.exception.status, 504)
                with self.assertRaises(ApiError) as second:
                    await runner.run(lambda: 1)
                self.assertEqual(second.exception.status, 429)
            finally:
                gate.set()
                runner.close()
        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
