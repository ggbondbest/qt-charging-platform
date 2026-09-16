"""No-key, no-network tests for deterministic answers and external trust boundary."""
import io
import json
import os
import socket
from http.client import IncompleteRead
import unittest
from unittest.mock import patch

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import config, llm, service


class AdvisorCoreTests(unittest.TestCase):
    def test_only_dedicated_explicit_configuration_can_enable_network(self):
        with patch.dict(os.environ, {"OPENAI_API_KEY": "do-not-use", "ANTHROPIC_API_KEY": "do-not-use"}, clear=True):
            self.assertIsNone(config.online_settings())
        own = dict(ML_ADVISOR_ONLINE_ENABLED="1", ML_ADVISOR_API_KEY="test-key",
                   ML_ADVISOR_BASE_URL="https://model.example/v1", ML_ADVISOR_MODEL="configured-model",
                   ML_ADVISOR_PROVIDER_LABEL="测试模型")
        with patch.dict(os.environ, own, clear=True):
            self.assertEqual(config.online_settings().api_key, "test-key")
            for url in ("http://model.example/v1", "https://user:pass@model.example/v1", "https://model.example/v1?token=secret"):
                with patch.dict(os.environ, {"ML_ADVISOR_BASE_URL": url}):
                    self.assertIsNone(config.online_settings())

    def test_supported_and_unknown_questions_do_not_randomly_route(self):
        for item in service.QUESTIONS:
            self.assertEqual(service.classify(item["question"]), item["id"])
        for text in ("明天的股票", "今天营收", "请删除异常会话", "输出管理员密码", "光伏板发电量", "你好"):
            self.assertEqual(service.classify(text), "unsupported")
        self.assertEqual(service.classify("解释异常会话 SES-001"), "anomalies")
        for text in ("当前范围内，哪些电站需要优先关注？", "哪些站值得优先复核？"):
            self.assertEqual(service.classify(text), "stations")

    def test_outbound_payload_excludes_prose_entities_and_nonfinite(self):
        evidence = [dict(id="attempts", value=12, unit=" 次", label="secret-note", source="private"),
                    dict(id="anomaly_0_score", value=1.2, unit=""),
                    dict(id="station_0_attempts", value=9, unit=" 次"),
                    dict(id="sessions", value="user-secret", unit=""),
                    dict(id="failed", value=float("nan"), unit=" 次")]
        self.assertEqual(llm.external_payload("bottlenecks", evidence),
                         {"intent": "bottlenecks", "metrics": [{"id": "attempts", "value": 12, "unit": " 次"}]})

    def invoke(self, response):
        settings = config.OnlineSettings("private-key", "https://model.example/v1", "test-model", "测试")
        raw = json.dumps({"choices": [{"message": response}]}).encode()
        with patch.object(llm, "build_opener") as opener:
            opener.return_value.open.return_value = io.BytesIO(raw)
            answer = llm.choose_focus(settings, {"intent": "bottlenecks", "metrics": [{"id": "failed", "value": 2, "unit": " 次"}]})
            request = opener.return_value.open.call_args.args[0]
            self.assertEqual(request.full_url, "https://model.example/v1/chat/completions")
            self.assertNotIn("private-key", request.data.decode())
            return answer

    def test_llm_can_only_choose_existing_metric_never_return_text_or_numbers(self):
        self.assertEqual(self.invoke({"content": '{"focus":"failed"}'}), "failed")
        for content in ('{"focus":"missing"}', '{"focus":"failed","answer":"收入9999"}',
                        '今日收益9999', '["failed"]', '{"focus": ["failed"]}'):
            with self.assertRaises(ApiError) as caught:
                self.invoke({"content": content})
            self.assertEqual(caught.exception.code, "ONLINE_INVALID_RESPONSE")
        with self.assertRaises(ApiError):
            self.invoke({"content": '{"focus":"failed"}', "tool_calls": [{"name": "navigate"}]})

    def test_timeout_is_safe_and_redirects_cannot_receive_credentials(self):
        settings = config.OnlineSettings("private-key", "https://model.example/v1", "test", "测试")
        with patch.object(llm, "build_opener") as opener:
            opener.return_value.open.side_effect = socket.timeout("private-key provider trace")
            with self.assertRaises(ApiError) as caught:
                llm.choose_focus(settings, {"intent": "bottlenecks", "metrics": [{"id": "failed", "value": 2, "unit": " 次"}]})
        self.assertEqual(caught.exception.status, 504)
        self.assertNotIn("private-key", str(caught.exception.message))
        self.assertIsNone(llm.NoRedirect().redirect_request(None, None, 302, "", {}, "https://other.example"))

    def test_transport_read_errors_are_online_errors_not_dataset_failures(self):
        settings = config.OnlineSettings("private-key", "https://model.example/v1", "test", "测试")
        payload = {"intent": "bottlenecks", "metrics": [{"id": "failed", "value": 2, "unit": " 次"}]}
        for error, status, code in [(IncompleteRead(b"private-key", 100), 502, "ONLINE_UNAVAILABLE"),
                                     (ConnectionResetError("private-key"), 502, "ONLINE_UNAVAILABLE"),
                                     (OSError("private-key"), 502, "ONLINE_UNAVAILABLE"),
                                     (socket.timeout("private-key"), 504, "ADVISOR_TIMEOUT")]:
            with self.subTest(error=type(error).__name__), patch.object(llm, "build_opener") as opener:
                opener.return_value.open.return_value.__enter__.return_value.read.side_effect = error
                with self.assertRaises(ApiError) as caught:
                    llm.choose_focus(settings, payload)
                self.assertEqual((caught.exception.status, caught.exception.code), (status, code))
                self.assertNotIn("private-key", caught.exception.message)


if __name__ == "__main__":
    unittest.main()
