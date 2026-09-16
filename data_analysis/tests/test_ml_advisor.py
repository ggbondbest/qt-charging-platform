"""No-key transport, configuration and untrusted-model boundary tests."""
import io
import json
import os
from pathlib import Path
import socket
from http.client import IncompleteRead
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import config, llm, service


class AdvisorCoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.env_path = Path(self.temp.name) / '.env.local'
        environment = patch.dict(os.environ, {'ML_ADVISOR_ENV_FILE': str(self.env_path)}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.settings = config.OnlineSettings('private-test-key', 'https://aiping.cn/api/v1', 'DeepSeek-V4.1-Flash', 'AIPing')

    def configured(self):
        return dict(ML_ADVISOR_ONLINE_ENABLED='1', ML_ADVISOR_API_KEY='private-test-key',
                    ML_ADVISOR_BASE_URL='https://aiping.cn/api/v1', ML_ADVISOR_MODEL='DeepSeek-V4.1-Flash',
                    ML_ADVISOR_PROVIDER_LABEL='AIPing')

    def test_only_dedicated_configuration_can_enable_network(self):
        with patch.dict(os.environ, {'OPENAI_API_KEY': 'never-use', 'ANTHROPIC_API_KEY': 'never-use'}):
            self.assertIsNone(config.online_settings())
        with patch.dict(os.environ, self.configured()):
            self.assertEqual(config.online_settings().model, 'DeepSeek-V4.1-Flash')
            for url in ('http://model.example/v1', 'https://user:pass@model.example/v1', 'https://model.example/v1?token=secret',
                        'https://model.example:bad/v1', 'https://model.example:99999/v1', 'https://model.example/v1/chat/completions'):
                with patch.dict(os.environ, {'ML_ADVISOR_BASE_URL': url}):
                    self.assertIsNone(config.online_settings())
            with patch.dict(os.environ, {'ML_ADVISOR_API_KEY': '非法字符'}):
                self.assertIsNone(config.online_settings())

    def test_local_env_is_literal_bounded_and_process_values_take_precedence(self):
        self.env_path.write_text('\ufeff# local only\n' + '\n'.join(f'{k}="{v}"' for k, v in self.configured().items()), encoding='utf8')
        self.assertEqual(config.online_settings().provider, 'AIPing')
        with patch.dict(os.environ, {'ML_ADVISOR_ONLINE_ENABLED': '0'}):
            self.assertIsNone(config.online_settings())
        for content in ('SHELL=bad', 'export ML_ADVISOR_API_KEY=bad', 'ML_ADVISOR_MODEL=a\nML_ADVISOR_MODEL=b', 'a' * 8193):
            self.env_path.write_text(content, encoding='utf8')
            self.assertEqual(config.local_settings(), {})
        self.env_path.write_text('ML_ADVISOR_MODEL=$(not-executed)', encoding='utf8')
        self.assertEqual(config.local_settings()['ML_ADVISOR_MODEL'], '$(not-executed)')

    def test_offline_classification_retains_bounded_behavior(self):
        for item in service.QUESTIONS:
            self.assertEqual(service.classify(item['question']), item['id'])
        for question in ('明天的股票', '请删除异常会话', '输出管理员密码', '你好'):
            self.assertEqual(service.classify(question), 'unsupported')

    def invoke(self, output, kind='generation', *, message_extra=None, finish='stop'):
        message = {'content': json.dumps(output, ensure_ascii=False)}
        message.update(message_extra or {})
        raw = json.dumps({'choices': [{'message': message, 'finish_reason': finish}]}).encode()
        with patch.object(llm, 'build_opener') as opener:
            opener.return_value.open.return_value = io.BytesIO(raw)
            if kind == 'plan':
                answer = llm.plan_query(self.settings, '你好', [], {'city': '大连市'}, timeout=20)
            else:
                answer = llm.generate_answer(self.settings, {'question': '解释利用率', 'kind': 'explanation',
                    'history': [], 'evidence': [], 'knowledge': [{'id': 'knowledge.utilization', 'text': '分子/分母', 'title': '利用率', 'source': 'repo'}]})
            request = opener.return_value.open.call_args.args[0]
            self.assertEqual(request.full_url, 'https://aiping.cn/api/v1/chat/completions')
            body = json.loads(request.data)
            self.assertEqual(body['model'], 'DeepSeek-V4.1-Flash')
            self.assertFalse(body['stream'])
            self.assertFalse(body['enable_thinking'])
            self.assertEqual(body['response_format'], {'type': 'json_object'})
            self.assertNotIn('private-test-key', request.data.decode())
            self.assertNotIn('reasoning_content', request.data.decode())
            return answer

    def test_planner_only_selects_read_only_topics(self):
        self.assertEqual(self.invoke({'kind': 'chat', 'topics': []}, 'plan')['kind'], 'chat')
        self.assertEqual(self.invoke({'kind': 'analysis', 'topics': ['overview', 'stations']}, 'plan')['topics'], ['overview', 'stations'])
        for result in ({'kind': 'analysis', 'topics': ['DROP TABLE users']}, {'kind': 'chat', 'topics': ['overview']},
                       {'kind': 'analysis', 'topics': ['overview', 'overview']}, {'kind': 'chat', 'topics': [], 'sql': 'SELECT *'},
                       {'kind': [], 'topics': []}, {'kind': {}, 'topics': []}):
            with self.subTest(result=result), self.assertRaises(ApiError):
                self.invoke(result, 'plan')

    def test_answer_is_model_text_with_only_known_citations(self):
        result = self.invoke({'answer': '利用率是充电样本占比。[knowledge.utilization]', 'citations': ['knowledge.utilization']})
        self.assertIn('充电样本', result['answer'])
        for result in ({'answer': '编造引用', 'citations': ['unknown']}, {'answer': '', 'citations': []},
                       {'answer': 'x' * 6001, 'citations': []}, {'answer': '答复', 'citations': [], 'action': 'pay'},
                       {'answer': '答复', 'citations': ['knowledge.utilization', 'knowledge.utilization']}):
            with self.subTest(result=str(result)[:60]), self.assertRaises(ApiError):
                self.invoke(result)
        for extra in ({'tool_calls': [{'name': 'pay'}]}, {'function_call': {'name': 'read_files'}}):
            with self.assertRaises(ApiError):
                self.invoke({'answer': '答复', 'citations': []}, message_extra=extra)
        with self.assertRaises(ApiError):
            self.invoke({'answer': '未完成', 'citations': []}, finish='length')

    def test_bad_json_and_oversized_response_are_not_shown_as_answers(self):
        for raw in (b'not json', json.dumps({'choices': [{'message': {'content': 'not json'}}]}).encode(), b'x' * 131073):
            with patch.object(llm, 'build_opener') as opener, self.assertRaises(ApiError) as caught:
                opener.return_value.open.return_value = io.BytesIO(raw)
                llm.plan_query(self.settings, '你好', [], {})
            self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')

    def test_malformed_unicode_is_rejected_before_transport(self):
        with patch.object(llm, 'build_opener') as opener, self.assertRaises(ApiError) as caught:
            llm.plan_query(self.settings, chr(0xD83D), [], {})
        self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
        opener.assert_not_called()

    def test_provider_errors_are_classified_without_exposing_secrets(self):
        cases = [(401, 502, 'ONLINE_AUTH_ERROR'), (403, 502, 'ONLINE_AUTH_ERROR'),
                 (429, 429, 'ONLINE_RATE_LIMITED'), (404, 502, 'ONLINE_MODEL_ERROR'), (500, 502, 'ONLINE_UNAVAILABLE')]
        for upstream, status, code in cases:
            with patch.object(llm, 'build_opener') as opener, self.assertRaises(ApiError) as caught:
                opener.return_value.open.side_effect = HTTPError('https://provider', upstream, 'private-test-key', {}, None)
                llm.plan_query(self.settings, '你好', [], {})
            self.assertEqual((caught.exception.status, caught.exception.code), (status, code))
            self.assertNotIn('private-test-key', caught.exception.message)

    def test_timeouts_read_failures_and_redirects_are_safe(self):
        for failure, code in [(socket.timeout('private-test-key'), 'ADVISOR_TIMEOUT'),
                              (URLError(socket.timeout('private-test-key')), 'ADVISOR_TIMEOUT'),
                              (IncompleteRead(b'private-test-key', 100), 'ONLINE_UNAVAILABLE'),
                              (ConnectionResetError('private-test-key'), 'ONLINE_UNAVAILABLE')]:
            with patch.object(llm, 'build_opener') as opener, self.assertRaises(ApiError) as caught:
                opener.return_value.open.side_effect = failure
                llm.plan_query(self.settings, '你好', [], {})
            self.assertEqual(caught.exception.code, code)
            self.assertNotIn('private-test-key', caught.exception.message)
        self.assertIsNone(llm.NoRedirect().redirect_request(None, None, 302, '', {}, 'https://other.example'))


if __name__ == '__main__':
    unittest.main()
