"""No-key, no-network RAG tests: real canonical service + bounded fake rollups."""
import json
from pathlib import Path
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import knowledge, llm, rag, service


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
        self.addCleanup(patch.stopall)

    def ask(self, **changes):
        return service.answer(self.snapshot, None, SimpleNamespace(**{**self.question, **changes}), settings=self.settings)

    def test_natural_question_uses_model_plan_canonical_aggregates_and_generated_answer(self):
        result = self.ask()
        self.assertEqual(result["answer"], "模型生成的有来源回答。")
        self.assertEqual(result["status"], "answered")
        self.assertEqual(result["citations"], ["overview.energy"])
        self.assertEqual(result["evidence"][0]["value"], 13)
        self.assertEqual(result["scope"]["publishedBatchId"], "test-batch-private")
        self.assertIn("publishedBatchId=test-batch-private", result["evidence"][0]["source"]["endpoint"])
        self.overview.assert_called_once()
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

    def test_overview_revenue_users_and_inventory_copy_published_metrics_and_units(self):
        result = self.ask(question="这段筛选范围内收了多少钱，活跃用户和桩有多少？")
        rows = {item["id"]: item for item in result["evidence"]}
        for key, value, unit, field in (
            ("net_paid", 123.45, " 元", "netPaidCents"), ("paid", 133.45, " 元", "paidCents"),
            ("refund", 10, " 元", "refundCents"), ("active_users", 2, " 人", "activeUsers"),
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
        def canonical(snapshot, provider, query, **kwargs):
            return {"status": "answered", "evidence": [dict(id=f"station_{i}_count", label="北京北站次数", value=i,
                unit="次", source={"endpoint": "/api/v1/dashboard/advanced?datasetId=test-dataset-private&cityId=CITY-01&startDate=2025-12-01",
                                   "field": "stations[stationId=ST-BJ-01].attemptCount"}) for i in range(20)],
                "limitations": ["模拟数据"], "suggestions": []}
        with patch.object(service, "answer", side_effect=canonical):
            result = rag.answer(self.snapshot, None, SimpleNamespace(**self.question), settings=self.settings)
        self.assertEqual(len(result["evidence"]), 36)
        self.assertEqual(len({row["id"] for row in result["evidence"]}), 36)
        self.assertEqual({row["id"].split(".")[0] for row in result["evidence"]}, {"overview", "stations", "behavior"})
        payload = json.dumps(self.generate.call_args.args[1], ensure_ascii=False)
        self.assertNotIn("ST-BJ-01", payload)
        self.assertNotIn("CITY-01", payload)
        self.assertNotIn("test-dataset-private", payload)

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
