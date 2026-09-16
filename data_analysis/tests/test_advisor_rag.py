"""No-key RAG integration: real analysis builder over bounded published DTOs."""
from copy import deepcopy
from itertools import combinations
import json
from pathlib import Path
import re
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from urllib.parse import parse_qs, urlsplit

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import analysis, knowledge, llm, rag, service
from data_analysis.tests.test_advisor_analysis import advanced_fixture


class PublishedDimensions:
    metadata = dict(datasetId="test-dataset-private", publishedBatchId="test-batch-private", source="SIMULATED",
                    startDate="2025-12-01", endDate="2025-12-04")
    cities = [{"city_id": "CITY-01", "city_name": "北京市"}, {"city_id": "CITY-02", "city_name": "上海市"}]
    stations = [{"station_id": "ST-BJ-01", "station_name": "北京北站", "city_id": "CITY-01"},
                {"station_id": "ST-SH-01", "station_name": "上海南站", "city_id": "CITY-02"}]

    def rows(self, sql, parameters=()):
        if sql == "SELECT city_id, city_name FROM cities":
            return self.cities
        if sql == "SELECT station_id, station_name, city_id FROM station_snapshot":
            return self.stations
        raise AssertionError("Only published dimensions may be read by retrieval: " + sql)

    def one(self, sql, parameters=()):
        if sql == "SELECT city_id FROM cities WHERE city_id = ?":
            return next((row for row in self.cities if row["city_id"] == parameters[0]), None)
        if sql == "SELECT city_id FROM station_snapshot WHERE station_id = ?":
            return next((row for row in self.stations if row["station_id"] == parameters[0]), None)
        raise AssertionError("Unexpected dimension lookup: " + sql)


class KnowledgeRetrievalTests(unittest.TestCase):
    def test_bm25_ranks_question_specific_passages_and_respects_bounds(self):
        utilization = knowledge.retrieve("充电利用率怎么算？完整小时缺测和分母", ["overview"])
        behavior = knowledge.retrieve("补能间隔首次观察前序历史和用户类型", ["behavior"])
        self.assertEqual(utilization[0]["id"], "knowledge.utilization")
        self.assertEqual(behavior[0]["id"], "knowledge.behavior")
        self.assertNotEqual([row["id"] for row in utilization], [row["id"] for row in behavior])
        self.assertEqual(knowledge.retrieve("xyzzzzquux", ["models"]), [])
        self.assertLessEqual(len(knowledge.retrieve("充电站点用户模型运营数据", limit=999)), 4)
        self.assertEqual(knowledge.retrieve("充电", limit=0), [])

    def test_every_passage_has_real_document_provenance_no_fixed_business_totals(self):
        root = Path(__file__).resolve().parents[2]
        for row in knowledge.PASSAGES:
            self.assertLessEqual(len(row["text"]), 1200)
            self.assertTrue((root / row["source"].split(" · ", 1)[0]).is_file())
            self.assertNotIn("/Users/", row["text"])
        result = knowledge.retrieve("模型")
        result[0]["text"] = "altered"
        self.assertNotEqual(knowledge.retrieve("模型")[0]["text"], "altered")


class RagTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = PublishedDimensions()
        self.snapshot.metadata = dict(self.snapshot.metadata)
        self.snapshot.stations = deepcopy(self.snapshot.stations)
        self.settings = SimpleNamespace(api_key="private-provider-key")
        self.question = dict(question="能不能帮我看一下充电运营情况，我想了解利用率？", mode="online", history=[],
            datasetId=self.snapshot.metadata["datasetId"], publishedBatchId=self.snapshot.metadata["publishedBatchId"],
            startDate=None, endDate=None, cityId=None, stationId=None)
        self.plan = patch.object(llm, "plan_query", create=True,
            return_value={"kind": "analysis", "topics": ["overview"]}).start()
        self.generate = patch.object(llm, "generate_answer", create=True,
            side_effect=lambda settings, payload, **kwargs: {"answer": "模型生成的有来源回答。", "citations":
                [payload["evidence"][0]["id"]] if payload["evidence"] else
                [payload["knowledge"][0]["id"]] if payload["knowledge"] else []}).start()
        self.overview = patch.object(service.service, "overview", return_value={"metrics": {
            "observedHours": 2, "startedSessions": 3, "energyWh": 13000,
            "chargingUtilizationRate": .25, "queueMeanWaitSeconds": 120, "netPaidCents": 12345,
            "paidCents": 13345, "refundCents": 1000, "activeUsers": 2, "completeHours": 1,
            "stationCount": 2, "chargerCount": 5}}).start()
        self.advanced_data = advanced_fixture()
        # The builder reads published dashboard DTOs, not raw files or SQL.
        # Keep real station identifiers in public sources to exercise outbound
        # replacement and source-query minimization in the RAG boundary.
        dimensions = [("ST-BJ-01", "北京北站", "CITY-01"), ("ST-BJ-02", "北京中站", "CITY-01"),
                      ("ST-SH-01", "上海南站", "CITY-02"), ("ST-SH-02", "上海西站", "CITY-02"),
                      ("ST-BJ-03", "北京小样本站", "CITY-01")]
        self.snapshot.stations = []
        for row, (station, name, city) in zip(self.advanced_data["stations"], dimensions):
            row.update(stationId=station, stationName=name)
            self.snapshot.stations.append(dict(station_id=station, station_name=name, city_id=city))
        self.advanced = patch.object(analysis.advanced, "analyze", return_value=self.advanced_data).start()
        self.build = patch.object(analysis, "build_analysis", wraps=analysis.build_analysis).start()
        self.addCleanup(patch.stopall)

    def ask(self, *, provider=None, **changes):
        return service.answer(self.snapshot, provider, SimpleNamespace(**{**self.question, **changes}), settings=self.settings)

    def comparable_period(self):
        self.snapshot.metadata["endDate"] = "2026-01-01"
        self.question.update(startDate="2025-12-11", endDate="2025-12-16")

    def assert_complete_groups(self, evidence):
        rows = {row["id"]: row for row in evidence}
        self.assertEqual(len(rows), len(evidence))
        for row in rows.values():
            if row["source"]["field"].startswith("derived:"):
                for dependency in re.findall(r"(?:overview|bottlenecks|stations|behavior)\.[a-z0-9_]+", row["source"]["field"]):
                    self.assertIn(dependency, rows, row["id"])
        for prefix, suffixes in (
            *((f"stations.station_{index}_", ("attempts", "success", "wait", "utilization")) for index in range(3)),
            ("stations.control_0_", ("attempts", "success", "wait", "utilization")),
            *((f"behavior.segment_{index}_", ("sessions", "interval_count", "interval", "energy")) for index in range(4)),
            ("bottlenecks.peak_", ("attempts", "success", "wait", "wait_count", "utilization", "hours")),
            *((f"bottlenecks.failure_{index}_", ("count", "share")) for index in range(3)),
            *((f"bottlenecks.entry_{index}_", ("attempts", "success")) for index in range(3))):
            found = {key for key in rows if key.startswith(prefix)}
            self.assertIn(found, (set(), {prefix + suffix for suffix in suffixes}))
        for group in ({"bottlenecks.attempts", "bottlenecks.successful", "bottlenecks.failed"},
                      {"behavior.sessions", "behavior.intervals", "behavior.first_observed"},
                      {"overview.active_users", "overview.sessions"}, {"overview.observed_hours", "overview.complete_hours"},
                      {"overview.station_count", "overview.charger_count"}, {"overview.paid", "overview.refund"}):
            self.assertIn(rows.keys() & group, (set(), group))
        for key in ("overview.energy", "overview.net_paid", "overview.utilization", "overview.success_rate", "bottlenecks.success_rate"):
            if key + "_previous" in rows:
                self.assertTrue({key, key + "_previous", key + "_change"} <= rows.keys())
                if key in {"overview.energy", "overview.net_paid"}:
                    self.assertIn(key + "_change_pct", rows)

    def test_natural_question_uses_model_plan_canonical_aggregates_and_generated_answer(self):
        result = self.ask()
        self.assertEqual(result["answer"], "模型生成的有来源回答。")
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["citations"], ["overview.energy"])
        self.assertEqual(result["evidence"][0]["value"], 13)
        self.assertEqual(result["scope"]["publishedBatchId"], "test-batch-private")
        self.assertIn("publishedBatchId=test-batch-private", result["evidence"][0]["source"]["endpoint"])
        self.overview.assert_called_once()
        self.build.assert_called_once()
        self.assertIs(self.build.call_args.args[0], self.snapshot)
        self.advanced.assert_called_once()
        self.assertIs(self.advanced.call_args.kwargs["snapshot"], self.snapshot)
        payload = self.generate.call_args.args[1]
        self.assertEqual(set(payload), {"question", "history", "scope", "kind", "evidence", "knowledge"})
        self.assertNotIn("test-batch-private", json.dumps(payload))
        self.assertNotIn("test-dataset-private", json.dumps(payload))
        self.assertIn("limitations", payload["scope"])

    def test_greetings_and_followups_are_generated_not_local_templates(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        self.generate.return_value = {"answer": "你好！我是模型这次实际生成的问候。", "citations": []}
        self.generate.side_effect = None
        for question in ("你好", "你好，今天心情不错"):
            with self.subTest(question=question):
                result = self.ask(question=question, history=[{"role": "user", "content": "你好"}])
                self.assertEqual(result["status"], "chat")
                self.assertEqual(result["answer"], self.generate.return_value["answer"])
                self.assertEqual(result["evidence"], [])
                self.assertEqual(result["knowledge"], [])
                self.assertEqual(self.plan.call_args.args[2], [{"role": "user", "content": "你好"}])
        self.assertEqual(self.generate.call_count, 2)
        self.overview.assert_not_called()
        self.build.assert_not_called()
        self.advanced.assert_not_called()

    def test_overview_revenue_users_and_inventory_copy_published_metrics_and_units(self):
        result = self.ask(question="这段筛选范围内收了多少钱，活跃用户和桩有多少？")
        rows = {item["id"]: item for item in result["evidence"]}
        for key, value, unit, field in (
            ("net_paid", 123.45, " 元", "netPaidCents × 0.01"), ("paid", 133.45, " 元", "paidCents / 100"),
            ("refund", 10, " 元", "refundCents / 100"), ("active_users", 2, " 人", "activeUsers"),
            ("observed_hours", 2, " 站点小时", "observedHours"), ("complete_hours", 1, " 站点小时", "completeHours"),
            ("station_count", 2, " 站", "stationCount"), ("charger_count", 5, " 个", "chargerCount")):
            with self.subTest(metric=key):
                self.assertEqual(rows["overview." + key]["value"], value)
                self.assertEqual(rows["overview." + key]["unit"], unit)
                self.assertEqual(rows["overview." + key]["source"]["field"], "metrics." + field)
        self.assertTrue(any("不是利润" in note and "静态快照" in note for note in result["limitations"]))

    def test_payment_only_window_still_has_evidence_but_static_inventory_alone_does_not(self):
        self.overview.return_value = {"metrics": {"observedHours": 0, "startedSessions": 0,
            "paidCents": 2000, "refundCents": 300, "netPaidCents": 1700, "stationCount": 2, "chargerCount": 5}}
        self.assertEqual(self.ask(question="当前净收款多少？")["status"], "answered")
        self.overview.return_value = {"metrics": {"observedHours": 0, "startedSessions": 0,
            "paidCents": 0, "refundCents": 0, "netPaidCents": 0, "stationCount": 2, "chargerCount": 5}}
        self.assertEqual(self.ask(question="当前范围的业务情况如何？")["status"], "no_evidence")

    def test_explanation_can_retrieve_project_knowledge_without_querying_metrics(self):
        self.plan.return_value = {"kind": "explanation", "topics": []}
        result = self.ask(question="充电利用率是怎么计算的？缺测为什么不能算零？")
        self.assertEqual(result["status"], "answered")
        self.assertTrue(result["citations"][0].startswith("knowledge."))
        self.assertEqual(result["evidence"], [])
        self.assertLessEqual(len(result["knowledge"]), 4)
        self.overview.assert_not_called()
        self.advanced.assert_not_called()

    def test_dates_cities_and_station_mismatches_do_not_become_scope_totals(self):
        cases = [("2025年12月1日电量", {}), ("最近7天电量", {}), ("北京电量", {}),
                 ("北京市电量", {"cityId": "CITY-02"}), ("天津市电量", {}),
                 ("ST-BJ-01电量", {}), ("北京北站电量", {"stationId": "ST-SH-01"})]
        for question, changes in cases:
            with self.subTest(question=question):
                result = self.ask(question=question, **changes)
                self.assertEqual(result["status"], "unsupported")
                self.assertEqual(result["evidence"], [])
        self.overview.assert_not_called()
        self.generate.assert_not_called()
        self.assertEqual(self.ask(question="北京市电量", cityId="CITY-01")["status"], "answered")
        self.assertEqual(self.ask(question="北京北站的利用率", stationId="ST-BJ-01")["status"], "answered")

    def test_ordinary_chinese_wording_is_not_rejected_by_the_old_vocabulary(self):
        for question in ("我们该如何理解这些充电运营数字呢", "站点比较让我有点糊涂，能耐心讲讲利用率吗", "怎样让站点更省电？"):
            with self.subTest(question=question):
                self.assertEqual(self.ask(question=question)["status"], "answered")

    def test_scope_cannot_be_broadened_by_ids_chat_labels_or_history_references(self):
        for question in ("CITY-02电量是多少？", "全网净收款是多少？", "所有城市的利用率是多少？"):
            with self.subTest(question=question):
                self.assertEqual(self.ask(question=question, cityId="CITY-01")["status"], "unsupported")
        self.assertEqual(self.ask(question="全市电量是多少？", stationId="ST-BJ-01")["status"], "unsupported")
        self.assertEqual(self.ask(question="那收入呢？", cityId="CITY-01", history=[
            {"role": "user", "content": "上海的电量是多少？"}])["status"], "unsupported")
        self.assertEqual(self.ask(question="那当前筛选的收入呢？", cityId="CITY-01", history=[
            {"role": "user", "content": "上海的电量是多少？"}])["status"], "answered")
        self.plan.return_value = {"kind": "chat", "topics": []}
        self.assertEqual(self.ask(question="你好，上海电量多少？", cityId="CITY-01")["status"], "unsupported")

    def test_same_scope_metric_question_cannot_skip_retrieval_with_a_chat_plan(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        for question, filters in (("当前范围的净收款是多少？", {}),
                                  ("你好，北京电量多少？", {"cityId": "CITY-01"})):
            with self.subTest(question=question), self.assertRaises(ApiError) as caught:
                self.ask(question=question, **filters)
            self.assertEqual((caught.exception.status, caught.exception.code), (502, "ONLINE_INVALID_RESPONSE"))
        self.overview.assert_not_called()
        self.generate.assert_not_called()

    def test_individual_records_never_reach_a_model_and_history_is_redacted(self):
        for question in ("分析异常会话SES-secret-001", "用户U-BJ-000001的历史", "车辆V-BJ-000001"):
            result = self.ask(question=question)
            self.assertEqual(result["status"], "unsupported")
        self.plan.assert_not_called()
        self.ask(question="ST-BJ-01运营概况", stationId="ST-BJ-01", history=[
            {"role": "user", "content": "旧记录 SES-secret-001；/Users/person/secret.txt；api_key=abc123；private-provider-key"}])
        payload = json.dumps(self.generate.call_args.args[1], ensure_ascii=False)
        for secret in ("SES-secret-001", "/Users/person", "abc123", "private-provider-key", "ST-BJ-01", "CITY-01"):
            self.assertNotIn(secret, payload)

    def test_history_citations_are_removed_only_from_outbound_context_without_changing_text_or_display_history(self):
        history = [
            {"role": "user", "content": "第[1]项：看12.5 kWh【overview.energy】。"},
            {"role": "assistant", "content": "旧值99999[overview.energy, knowledge.utilization]。范围[0,1]，保留备注[说明]。"},
        ]
        original = deepcopy(history)
        expected = [
            {"role": "user", "content": "第[1]项：看12.5 kWh。"},
            {"role": "assistant", "content": "旧值99999。范围[0,1]，保留备注[说明]。"},
        ]
        self.ask(question="当前范围的电量如何？", history=history)
        self.assertEqual(self.plan.call_args.args[2], expected)
        self.assertEqual(self.generate.call_args.args[1]["history"], expected)
        self.assertEqual(history, original)
        self.assertIn("overview.energy", history[1]["content"])

    def test_current_question_knowledge_is_selected_first_and_history_only_fills_remaining_slots(self):
        question = "怎样设计小步试点、对照和验证指标？"
        history = [{"role": "assistant", "content": "旧的利用率介绍" * 100 + "[overview.energy]"}]

        def passage(identifier):
            return dict(id="knowledge." + identifier, title=identifier, text="模拟方法说明", source="repo/methods.md")

        current = [passage("current_method"), passage("current_verification")]
        old = [passage("current_method"), passage("old_one"), passage("old_two"), passage("old_three")]
        with patch.object(knowledge, "retrieve", side_effect=lambda query, topics: deepcopy(current if query == question else old)) as retrieve:
            result = self.ask(question=question, history=history)
        expected = ["knowledge.current_method", "knowledge.current_verification", "knowledge.old_one", "knowledge.old_two"]
        self.assertEqual([row["id"] for row in result["knowledge"]], expected)
        self.assertEqual([row["id"] for row in self.generate.call_args.args[1]["knowledge"]], expected)
        self.assertEqual(retrieve.call_count, 2)
        self.assertEqual(retrieve.call_args_list[0].args, (question, ["overview"]))
        self.assertEqual(retrieve.call_args_list[1].args, ("旧的利用率介绍" * 100, ["overview"]))
        self.assertEqual(len(current), 2)
        self.assertIn("[overview.energy]", history[0]["content"])

    def test_four_current_question_passages_cannot_be_displaced_by_long_history(self):
        current = [dict(id=f"knowledge.current_{index}", title="当前方法", text="当前问题的说明", source="repo/current.md")
                   for index in range(4)]
        with patch.object(knowledge, "retrieve", return_value=deepcopy(current)) as retrieve:
            result = self.ask(question="当前问题怎样验证？", history=[{"role": "assistant", "content": "历史说明" * 800}])
        self.assertEqual(result["knowledge"], current)
        retrieve.assert_called_once_with("当前问题怎样验证？", ["overview"])

    def test_chat_summary_of_prior_statistics_fetches_fresh_current_evidence_not_historical_values(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        history = [
            {"role": "user", "content": "当前页面的电量如何？"},
            {"role": "assistant", "content": "旧答案电量99999 kWh[overview.energy]，旧指标88888[overview.obsolete_metric]。"},
        ]
        original = deepcopy(history)

        def fresh_answer(settings, payload, **kwargs):
            energy = next(row for row in payload["evidence"] if row["id"] == "overview.energy")
            return {"answer": f"重新核对当前范围：{energy['value']:g} kWh。[overview.energy]", "citations": ["overview.energy"]}

        self.generate.side_effect = fresh_answer
        result = self.ask(question="谢谢，帮我总结一下刚才的结论。", history=history)
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["intent"], "overview")
        self.assertEqual(result["answer"], "重新核对当前范围：13 kWh。[overview.energy]")
        self.overview.assert_called_once()
        self.advanced.assert_called_once()
        self.assertEqual(self.build.call_args.args[2], ["overview"])
        payload = self.generate.call_args.args[1]
        self.assertEqual(payload["kind"], "analysis")
        self.assertEqual(next(row["value"] for row in payload["evidence"] if row["id"] == "overview.energy"), 13)
        self.assertNotIn("overview.obsolete_metric", json.dumps(payload))
        self.assertEqual(payload["history"][1]["content"], "旧答案电量99999 kWh，旧指标88888。")
        self.assertEqual(self.plan.call_args.args[2], payload["history"])
        self.assertEqual(history, original)
        public_energy = next(row for row in result["evidence"] if row["id"] == "overview.energy")
        self.assertIn("publishedBatchId=test-batch-private", public_energy["source"]["endpoint"])
        self.assertIn("metrics.energyWh", public_energy["source"]["field"])

    def test_summary_uses_only_the_latest_assistant_with_known_topics_and_caps_them_at_three(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        history = [
            {"role": "assistant", "content": "更早的模型记录[models.load_status]。"},
            {"role": "user", "content": "用户文字不能选择主题[models.load_status]。"},
            {"role": "assistant", "content": "最近分析[behavior.old][stations.old][overview.old][behavior.duplicate][bottlenecks.old][models.old]。"},
            {"role": "assistant", "content": "最近寒暄只有未知主题[unknown.metric]。"},
        ]
        result = self.ask(question="请精简一下前面的结论。", history=history)
        self.assertEqual(result["status"], "answered")
        self.assertEqual(self.build.call_args.args[2], ["behavior", "stations", "overview"])
        self.assertEqual(result["intent"], "behavior+stations+overview")
        self.assertFalse(any(row["id"].startswith("models.") for row in result["evidence"]))
        outgoing = json.dumps(self.generate.call_args.args[1]["history"])
        for identifier in ("models.load_status", "behavior.old", "stations.old", "overview.old", "unknown.metric"):
            self.assertNotIn(identifier, outgoing)

    def test_pure_thanks_and_nonreferential_requests_remain_chat_despite_prior_statistics(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        self.generate.side_effect = None
        self.generate.return_value = {"answer": "不客气，可以继续提问。", "citations": []}
        for question in ("谢谢", "谢谢你，很耐心。", "帮我总结你的能力。", "谢谢，请解释这个成语守株待兔。"):
            with self.subTest(question=question):
                result = self.ask(question=question, history=[{"role": "assistant", "content": "旧电量99999[overview.energy]。"}])
                self.assertEqual(result["status"], "chat")
                self.assertEqual(result["evidence"], [])
                self.assertEqual(self.generate.call_args.args[1]["kind"], "chat")
        self.build.assert_not_called()
        self.overview.assert_not_called()
        self.advanced.assert_not_called()

    def test_summary_does_not_invent_topics_from_unknown_knowledge_or_user_references(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        self.generate.side_effect = None
        self.generate.return_value = {"answer": "请明确希望总结哪方面。", "citations": []}
        for history in ([{"role": "assistant", "content": "99999[unknown.metric]。"}],
                        [{"role": "assistant", "content": "知识口径[knowledge.utilization]。"}],
                        [{"role": "user", "content": "只在用户文字中[overview.energy]。"}],
                        [{"role": "assistant", "content": "未引用的 overview 旧数字99999。"}]):
            with self.subTest(history=history):
                result = self.ask(question="请总结一下刚才的结论。", history=history)
                self.assertEqual(result["status"], "chat")
                self.assertEqual(result["evidence"], [])
        self.build.assert_not_called()
        self.overview.assert_not_called()
        self.advanced.assert_not_called()

    def test_promoted_summary_still_rejects_conflicting_current_or_historical_scope(self):
        self.plan.return_value = {"kind": "chat", "topics": []}
        cases = (
            ("谢谢，请总结刚才上海的结论。", "当前范围的情况？"),
            ("谢谢，帮我总结一下刚才的结论。", "上海的电量是多少？"),
            ("请简化一下之前的结论。", "2025年12月1日电量是多少？"),
        )
        for question, previous in cases:
            with self.subTest(question=question, previous=previous):
                result = self.ask(question=question, cityId="CITY-01", history=[
                    {"role": "user", "content": previous}, {"role": "assistant", "content": "旧数据99999[overview.energy]。"}])
                self.assertEqual(result["status"], "unsupported")
                self.assertEqual(result["evidence"], [])
        self.build.assert_not_called()
        self.overview.assert_not_called()
        self.generate.assert_not_called()

    def test_generated_answer_redacts_credentials_entities_paths_and_contacts_but_keeps_citations(self):
        sensitive = ("private-provider-key", "sk-test-secret42", "U-DEMO-42", "/Users/person/secret.txt",
                     "/home/person/token.txt", "13800138000", "+86 13900139000", "operator@example.test")
        self.generate.side_effect = None
        self.generate.return_value = {"answer": "净收款为123.45元[overview.net_paid]。" + "；".join(sensitive),
                                      "citations": ["overview.net_paid"]}
        result = self.ask(question="当前净收款多少？")
        for secret in sensitive:
            with self.subTest(secret=secret):
                self.assertNotIn(secret, result["answer"])
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["citations"], ["overview.net_paid"])
        self.assertIn("123.45元[overview.net_paid]", result["answer"])
        self.assertEqual(next(row["value"] for row in result["evidence"] if row["id"] == "overview.net_paid"), 123.45)

    def test_multi_topic_evidence_is_deduplicated_bounded_and_source_ids_are_private(self):
        self.plan.return_value = {"kind": "analysis", "topics": ["overview", "stations", "behavior"]}
        self.comparable_period()
        result = self.ask()
        self.assertEqual(len(result["evidence"]), 48)
        self.assert_complete_groups(result["evidence"])
        self.assertEqual({row["id"].split(".")[0] for row in result["evidence"]}, {"overview", "stations", "behavior"})
        self.build.assert_called_once()
        self.assertEqual((self.overview.call_count, self.advanced.call_count), (2, 2))
        self.assertTrue(any("48条" in note for note in result["limitations"]))
        self.assertTrue(any("ST-BJ-01" in row["source"]["field"] for row in result["evidence"]))
        self.assertTrue(any("publishedBatchId=test-batch-private" in row["source"]["endpoint"] for row in result["evidence"]))
        payload = json.dumps(self.generate.call_args.args[1], ensure_ascii=False)
        for secret in ("ST-BJ-01", "ST-BJ-02", "ST-SH-01", "CITY-01", "test-dataset-private", "test-batch-private"):
            self.assertNotIn(secret, payload)
        for row in self.generate.call_args.args[1]["evidence"]:
            self.assertLessEqual(set(parse_qs(urlsplit(row["source"]["endpoint"]).query)), {"startDate", "endDate"})

    def test_all_three_business_topic_combinations_keep_groups_and_dependencies_within_48(self):
        self.comparable_period()
        for topics in combinations(analysis.TOPICS, 3):
            with self.subTest(topics=topics):
                self.plan.return_value = {"kind": "analysis", "topics": list(topics)}
                result = self.ask()
                self.assertEqual(result["status"], "answered")
                self.assertLessEqual(len(result["evidence"]), 48)
                self.assertEqual({row["id"].split(".")[0] for row in result["evidence"]}, set(topics))
                self.assert_complete_groups(result["evidence"])
                self.assertEqual([row["id"] for row in self.generate.call_args.args[1]["evidence"]],
                                 [row["id"] for row in result["evidence"]])

    def test_previous_period_sources_values_and_caveats_reach_generation_without_batch_ids(self):
        self.comparable_period()
        current = deepcopy(self.overview.return_value)
        previous = deepcopy(current)
        previous["metrics"].update(energyWh=10000, netPaidCents=10000, chargingUtilizationRate=.1)
        self.overview.side_effect = lambda snapshot, filters: current if filters["start"] == "2025-12-11" else previous
        result = self.ask(cityId="CITY-01")
        rows = {row["id"]: row for row in self.generate.call_args.args[1]["evidence"]}
        self.assertAlmostEqual(rows["overview.energy_change"]["value"], 3)
        self.assertAlmostEqual(rows["overview.energy_change_pct"]["value"], 30)
        self.assertAlmostEqual(rows["overview.utilization_change"]["value"], 15)
        self.assertEqual(rows["overview.utilization_change"]["unit"], " 个百分点")
        self.assertEqual(parse_qs(urlsplit(rows["overview.energy"]["source"]["endpoint"]).query),
                         {"startDate": ["2025-12-11"], "endDate": ["2025-12-16"]})
        self.assertEqual(parse_qs(urlsplit(rows["overview.energy_previous"]["source"]["endpoint"]).query),
                         {"startDate": ["2025-12-06"], "endDate": ["2025-12-11"]})
        limitations = self.generate.call_args.args[1]["scope"]["limitations"]
        self.assertTrue(any("紧邻等长前期" in note and "百分点" in note for note in limitations))
        self.assertTrue(any("2025-12-06" in row["source"]["endpoint"] and "cityId=CITY-01" in row["source"]["endpoint"]
                            for row in result["evidence"]))
        self.assert_complete_groups(result["evidence"])

    def model_provider(self):
        return SimpleNamespace(registry=Mock(return_value={
            "load": {"status": "READY"}, "availability": {"status": "READY"},
            "insights": {"status": "READY", "anomaly": {"test": {"precision": .875, "recall": .625}}},
            "arrival": {"status": "NOT_READY"}}))

    def test_real_model_registry_appends_all_six_rows_without_slicing_business_groups(self):
        self.comparable_period()
        self.plan.return_value = {"kind": "analysis", "topics": ["overview", "bottlenecks", "models"]}
        provider = self.model_provider()
        result = self.ask(provider=provider)
        self.assertEqual(result["status"], "answered")
        business = [row for row in result["evidence"] if not row["id"].startswith("models.")]
        models = {row["id"]: row for row in result["evidence"] if row["id"].startswith("models.")}
        self.assertEqual(len(business), 46)
        self.assertEqual(len(result["evidence"]), 52)
        self.assert_complete_groups(business)
        self.assertEqual(set(models), {"models.load_status", "models.availability_status", "models.insights_status",
                                       "models.arrival_status", "models.precision", "models.recall"})
        self.assertEqual(models["models.precision"]["value"], 87.5)
        self.assertEqual(models["models.recall"]["value"], 62.5)
        self.assertEqual(models["models.arrival_status"]["value"], "未就绪")
        provider.registry.assert_called_once_with(self.snapshot.metadata)
        self.build.assert_called_once()
        payload = self.generate.call_args.args[1]
        self.assertEqual([row["id"] for row in payload["evidence"]], [row["id"] for row in result["evidence"]])
        for row in payload["evidence"]:
            if row["id"].startswith("models."):
                self.assertEqual(row["source"]["endpoint"], "/api/v1/intelligence/models")
        self.assertTrue(any("不改变模型训练和评测范围" in note for note in payload["scope"]["limitations"]))
        self.assertTrue(any(item["target"] == "models" for item in result["suggestions"]))

    def test_models_only_uses_registry_not_business_queries_and_missing_registry_stays_explicit(self):
        self.plan.return_value = {"kind": "analysis", "topics": ["models"]}
        result = self.ask(provider=self.model_provider(), question="目前有哪些模型？")
        self.assertEqual(result["status"], "answered")
        self.assertEqual(len(result["evidence"]), 6)
        self.assertTrue(all(row["id"].startswith("models.") for row in result["evidence"]))
        self.overview.assert_not_called()
        self.advanced.assert_not_called()
        self.generate.reset_mock()
        with self.assertRaises(ApiError) as caught:
            self.ask(question="目前有哪些模型？")
        self.assertEqual(caught.exception.code, "MODEL_NOT_READY")
        self.generate.assert_not_called()

    def test_48_business_rows_plus_model_groups_accept_64_but_never_truncate_overflow(self):
        self.plan.return_value = {"kind": "analysis", "topics": ["overview", "models"]}
        # Exercise the composition guard independently of today's six-row
        # model registry; future registry growth must fail, never slice a group.
        def row(key):
            return dict(id=key, label="模拟聚合证据", value=1, unit=" 次",
                        source=dict(endpoint="/api/v1/dashboard/overview", field="metrics.startedSessions"))
        packet = dict(evidence=[row(f"overview.synthetic_{i}") for i in range(48)],
                      limitations=[], suggestions=[], observed=True, observedTopics=["overview"])
        for count in (16, 17):
            canonical = dict(status="answered", evidence=[row(f"group_{i // 4}_{i % 4}") for i in range(count)],
                             limitations=["固定历史模型评测，不随日期变化"], suggestions=[])
            self.generate.reset_mock()
            with self.subTest(model_rows=count), patch.object(analysis, "build_analysis", return_value=packet), \
                 patch.object(service, "answer", return_value=canonical) as models:
                if count == 16:
                    result = rag.answer(self.snapshot, None, SimpleNamespace(**self.question), settings=self.settings)
                    self.assertEqual(len(result["evidence"]), 64)
                    self.assertEqual([row["id"] for row in result["evidence"][:48]],
                                     [row["id"] for row in packet["evidence"]])
                    self.assertEqual([row["id"] for row in result["evidence"][48:]],
                                     ["models." + row["id"] for row in canonical["evidence"]])
                    self.assertEqual(len(self.generate.call_args.args[1]["evidence"]), 64)
                else:
                    with self.assertRaises(ApiError) as caught:
                        rag.answer(self.snapshot, None, SimpleNamespace(**self.question), settings=self.settings)
                    self.assertEqual(caught.exception.code, "ONLINE_INVALID_RESPONSE")
                    self.generate.assert_not_called()
                models.assert_called_once()
                self.assertEqual(models.call_args.args[2].mode, "offline")
                self.assertIsNone(models.call_args.kwargs["settings"])

    def test_invalid_business_publication_is_not_hidden_by_models_or_project_knowledge(self):
        self.plan.return_value = {"kind": "analysis", "topics": ["overview", "models"]}
        provider = self.model_provider()
        for code in ("BATCH_MISMATCH", "DATA_NOT_READY"):
            self.advanced.side_effect = ApiError(503, code, "统计成果校验失败")
            with self.subTest(code=code), self.assertRaises(ApiError) as caught:
                self.ask(provider=provider)
            self.assertEqual(caught.exception.code, code)
        self.generate.assert_not_called()
        provider.registry.assert_not_called()

    def test_cancellation_during_real_analysis_stops_before_generation_and_keeps_deadline(self):
        cancelled = threading.Event()
        deadline = time.monotonic() + 10

        def cancel_after_aggregate(*args, **kwargs):
            cancelled.set()
            return self.advanced_data

        self.advanced.side_effect = cancel_after_aggregate
        with self.assertRaises(ApiError) as caught:
            rag.answer(self.snapshot, None, SimpleNamespace(**self.question), settings=self.settings,
                       deadline=deadline, cancelled=cancelled)
        self.assertEqual(caught.exception.code, "ADVISOR_TIMEOUT")
        self.assertEqual(self.build.call_args.kwargs["deadline"], deadline)
        self.assertIs(self.build.call_args.kwargs["cancelled"], cancelled)
        self.generate.assert_not_called()

    def test_analysis_citations_cannot_be_knowledge_only_and_duplicates_are_normalized(self):
        self.generate.side_effect = lambda settings, payload, **kwargs: {
            "answer": "知识片段不能替代业务观察。", "citations": [payload["knowledge"][0]["id"]]}
        with self.assertRaises(ApiError) as caught:
            self.ask()
        self.assertEqual(caught.exception.code, "ONLINE_INVALID_RESPONSE")
        self.generate.side_effect = lambda settings, payload, **kwargs: {
            "answer": "实际业务证据。", "citations": [payload["evidence"][0]["id"]] * 2}
        result = self.ask()
        self.assertEqual(result["citations"], ["overview.energy"])

    def test_no_aggregate_observations_does_not_generate_a_data_conclusion(self):
        self.overview.return_value = {"metrics": {}}
        result = self.ask()
        self.assertEqual(result["status"], "no_evidence")
        self.assertEqual(result["citations"], [])
        self.generate.assert_not_called()

    def test_invalid_or_missing_citations_fail_instead_of_pretending_to_be_grounded(self):
        self.generate.side_effect = None
        for citations in ([], ["fabricated-id"], ["overview.energy", "fabricated-id"], [42]):
            self.generate.return_value = {"answer": "所谓有据回答", "citations": citations}
            with self.subTest(citations=citations), self.assertRaises(ApiError) as caught:
                self.ask()
            self.assertEqual(caught.exception.code, "ONLINE_INVALID_RESPONSE")

    def test_planner_cannot_select_private_records_or_arbitrary_queries(self):
        for plan in ({"kind": "analysis", "topics": ["anomalies"]}, {"kind": "analysis", "topics": ["SELECT * FROM users"]},
                     {"kind": "analysis", "topics": ["overview"] * 4}, {"kind": "execute", "topics": []}):
            self.plan.return_value = plan
            with self.subTest(plan=plan), self.assertRaises(ApiError) as caught:
                self.ask()
            self.assertEqual(caught.exception.code, "ONLINE_INVALID_RESPONSE")
        self.overview.assert_not_called()

    def test_nonstring_plan_kind_is_a_safe_invalid_response_not_a_type_error(self):
        for kind in ([], {}):
            self.plan.return_value = {"kind": kind, "topics": []}
            with self.subTest(kind=kind), self.assertRaises(ApiError) as caught:
                self.ask()
            self.assertEqual((caught.exception.status, caught.exception.code), (502, "ONLINE_INVALID_RESPONSE"))
        self.overview.assert_not_called()
        self.generate.assert_not_called()

    def test_model_outages_and_deadlines_remain_explicit(self):
        self.plan.side_effect = ApiError(502, "ONLINE_UNAVAILABLE", "在线模型不可用")
        with self.assertRaises(ApiError) as caught:
            self.ask(question="你好")
        self.assertEqual(caught.exception.code, "ONLINE_UNAVAILABLE")
        self.plan.side_effect = None
        for deadline, cancelled in ((time.monotonic() - 1, None), (None, threading.Event())):
            if cancelled:
                cancelled.set()
            with self.assertRaises(ApiError) as caught:
                rag.answer(self.snapshot, None, SimpleNamespace(**self.question), settings=self.settings,
                           deadline=deadline, cancelled=cancelled)
            self.assertEqual(caught.exception.code, "ADVISOR_TIMEOUT")


if __name__ == "__main__":
    unittest.main()
