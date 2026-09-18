"""No-key transport, configuration and untrusted-model boundary tests."""
import io
import json
import os
from copy import deepcopy
from pathlib import Path
import socket
from http.client import IncompleteRead
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import config, llm, rag, service


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
            opener.return_value.open.side_effect = lambda *args, **kwargs: io.BytesIO(raw)
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
                       {'answer': 'x' * 6001, 'citations': []}, {'answer': '答复', 'citations': [], 'action': 'pay'}):
            with self.subTest(result=str(result)[:60]), self.assertRaises(ApiError):
                self.invoke(result)
        for extra in ({'tool_calls': [{'name': 'pay'}]}, {'function_call': {'name': 'read_files'}}):
            with self.assertRaises(ApiError):
                self.invoke({'answer': '答复', 'citations': []}, message_extra=extra)
        with self.assertRaises(ApiError):
            self.invoke({'answer': '未完成', 'citations': []}, finish='length')

    def test_bad_json_and_oversized_response_are_not_shown_as_answers(self):
        for raw in (b'not json', json.dumps({'choices': [{'message': {'content': 'not json'}, 'finish_reason': 'stop'}]}).encode(), b'x' * 131073):
            with patch.object(llm, 'build_opener') as opener, self.assertRaises(ApiError) as caught:
                opener.return_value.open.side_effect = lambda *args, **kwargs: io.BytesIO(raw)
                llm.plan_query(self.settings, '你好', [], {})
            self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
            self.assertEqual(opener.return_value.open.call_count, 2 if b'choices' in raw else 1)

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


class AdvisorCitationCorrectionTests(unittest.TestCase):
    """Real wire parsing and one shared correction budget; never real network."""

    def setUp(self):
        self.settings = config.OnlineSettings('fake-citation-test-key', 'https://aiping.cn/api/v1',
            'DeepSeek-V4.1-Flash', 'AIPing', timeout=45.0)
        self.payload = {
            'question': '解释当前范围的电量和利用率', 'kind': 'explanation',
            'history': [{'role': 'user', 'content': '利用率如何理解？'}],
            'scope': {'startDate': '2026-01-01', 'endDate': '2026-01-08'},
            'evidence': [
                {'id': 'overview.energy', 'label': '观测电量', 'value': 12.5, 'unit': 'kWh'},
                {'id': 'overview.utilization', 'label': '完整小时利用率', 'value': 25, 'unit': '%'},
            ],
            'knowledge': [{'id': 'knowledge.utilization', 'title': '利用率口径',
                'text': '先合计完整小时的分子与分母，缺测不填零。', 'source': 'repo/metrics.md'}],
        }
        self.valid = {'answer': '观测电量为12.5 kWh。[overview.energy]', 'citations': ['overview.energy']}
        network = patch.object(llm, 'build_opener')
        self.opener = network.start()
        self.addCleanup(network.stop)
        self.open = self.opener.return_value.open

    @staticmethod
    def wire(output, *, finish='stop', message_extra=None):
        message = {'content': json.dumps(output, ensure_ascii=False)}
        message.update(message_extra or {})
        return io.BytesIO(json.dumps({'choices': [{'message': message, 'finish_reason': finish}]}).encode())

    @staticmethod
    def wire_content(content, *, finish='stop'):
        return io.BytesIO(json.dumps({'choices': [{'message': {'content': content}, 'finish_reason': finish}]}).encode())

    def queue(self, *outputs):
        self.open.reset_mock()
        self.open.side_effect = [self.wire(output) for output in outputs]

    def sent(self, index):
        body = json.loads(self.open.call_args_list[index].args[0].data)
        return body, json.loads(body['messages'][1]['content'])

    def plan(self, **kwargs):
        return llm.plan_query(self.settings, self.payload['question'], self.payload['history'],
                              self.payload['scope'], **kwargs)

    def test_output_errors_have_safe_reasons_and_explicit_retry_policy(self):
        for reason, retryable in (('envelope', False), ('format', True), ('shape', True), ('plan', True),
                                  ('citations', True), ('truncated', True), ('blocked', False)):
            with self.subTest(reason=reason):
                error = llm.ModelOutputError(reason, retryable=retryable)
                self.assertIsInstance(error, ApiError)
                self.assertEqual((error.status, error.code, error.reason, error.retryable),
                                 (502, 'ONLINE_INVALID_RESPONSE', reason, retryable))
                self.assertTrue(error.message)
                self.assertIsNone(error.data)
                self.assertNotIn(self.settings.api_key, str(error))

    def test_invalid_local_payloads_fail_safely_without_transport_or_retry(self):
        for value in (float('nan'), float('inf'), {'not-json-serializable'}, object()):
            with self.subTest(value_type=type(value).__name__):
                self.payload['scope'] = {'invalid': value}
                with self.assertLogs(llm.logger.name, level='WARNING'), self.assertRaises(llm.ModelOutputError) as caught:
                    self.plan()
                self.assertEqual((caught.exception.status, caught.exception.code), (502, 'ONLINE_INVALID_RESPONSE'))
                self.assertEqual(caught.exception.reason, 'envelope')
                self.assertFalse(caught.exception.retryable)
                self.opener.assert_not_called()

    def test_complete_fences_bom_and_bare_string_whitespace_parse_without_a_retry(self):
        expected = {'answer': '第一行\n第二行\r第三行\t核对。[overview.energy]', 'citations': ['overview.energy']}
        canonical = json.dumps(expected, ensure_ascii=False)
        bare = canonical.replace(r'\n', '\n').replace(r'\r', '\r').replace(r'\t', '\t')
        contents = [canonical, '\ufeff \r\n' + canonical, bare,
                    '```\n' + canonical + '\n```', '```JSON\r\n' + bare + '\r\n```',
                    ' \ufeff\n```json\r' + canonical + '\r```\t ']
        for content in contents:
            with self.subTest(packaging=content[:20]):
                self.open.reset_mock()
                self.open.side_effect = [self.wire_content(content), self.wire(self.valid)]
                self.assertEqual(llm.generate_answer(self.settings, self.payload), expected)
                self.open.assert_called_once()
                self.assertNotIn('citationCorrection', self.sent(0)[1])

    def test_unsafe_inner_json_is_regenerated_from_unchanged_original_evidence(self):
        contents = ('BAD_DRAFT_ONLY {"answer":"fake-citation-test-key', '[]',
                    '{"answer":"one","answer":"two","citations":[]}',
                    '{"answer":"unescaped "quotation"","citations":[]}',
                    '{"answer":"trailing comma","citations":[],}',
                    '{"answer":NaN,"citations":[]}', '{} {}',
                    '{"answer":"bad\u0000control","citations":[]}',
                    '{"answer":"\\ud83d","citations":[]}',
                    '{"extra":' + '[' * 70 + '0' + ']' * 70 + '}')
        original = deepcopy(self.payload)
        corrections = set()
        for content in contents:
            with self.subTest(inner=content[:50]):
                self.open.reset_mock()
                self.open.side_effect = [self.wire_content(content), self.wire(self.valid)]
                with self.assertLogs(llm.logger.name, level='WARNING') as logs:
                    self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
                self.assertEqual(self.open.call_count, 2)
                self.assertIn('stage=answer reason=format attempt=1 retryable=True', logs.output[0])
                _, first = self.sent(0)
                body, second = self.sent(1)
                corrections.add(second['citationCorrection'])
                self.assertEqual({key: value for key, value in second.items() if key != 'citationCorrection'}, first)
                self.assertNotIn('BAD_DRAFT_ONLY', json.dumps(body))
                self.assertNotIn(self.settings.api_key, json.dumps(body))
                self.assertEqual(self.payload, original)
        self.assertEqual(len(corrections), 1)

    def test_length_finish_regenerates_once_even_if_the_first_content_looks_complete(self):
        payload = dict(self.payload, kind='analysis')
        self.open.side_effect = [self.wire(self.valid, finish='length'), self.wire(self.valid)]
        with self.assertLogs(llm.logger.name, level='WARNING') as logs:
            self.assertEqual(llm.generate_answer(self.settings, payload), self.valid)
        self.assertEqual(self.open.call_count, 2)
        self.assertIn('reason=truncated', logs.output[0])
        self.assertEqual([self.sent(index)[0]['max_tokens'] for index in (0, 1)], [3000, 3000])
        self.assertIn('citationCorrection', self.sent(1)[1])

    def test_format_shape_truncation_and_citations_share_exactly_two_total_attempts(self):
        bad_citation = {'answer': '未经支持。[invented.metric]', 'citations': ['invented.metric']}
        cases = (
            ('json-then-citations', lambda: self.wire_content('{"answer":'), lambda: self.wire(bad_citation), 'citations'),
            ('length-then-shape', lambda: self.wire(self.valid, finish='length'), lambda: self.wire({'answer': ''}), 'shape'),
            ('shape-then-json', lambda: self.wire({'answer': 3, 'citations': []}), lambda: self.wire_content('not-json'), 'format'),
            ('citations-then-length', lambda: self.wire(bad_citation), lambda: self.wire(self.valid, finish='length'), 'truncated'),
            ('json-then-json', lambda: self.wire_content('{'), lambda: self.wire_content('{'), 'format'),
        )
        for label, first, second, reason in cases:
            with self.subTest(case=label):
                self.open.reset_mock()
                self.open.side_effect = [first(), second(), self.wire(self.valid)]
                with self.assertLogs(llm.logger.name, level='WARNING') as logs, self.assertRaises(llm.ModelOutputError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual((caught.exception.status, caught.exception.code, caught.exception.reason),
                                 (502, 'ONLINE_INVALID_RESPONSE', reason))
                self.assertEqual(self.open.call_count, 2)
                self.assertEqual(len(logs.output), 2)
                self.assertIn('attempt=2', logs.output[-1])
                self.assertNotIn('invented.metric', json.dumps(self.sent(1)))

    def test_nonretryable_correction_failure_keeps_its_classification_without_a_third_request(self):
        cases = ((lambda: io.BytesIO(b'NEVER_LOG_PROVIDER_BODY'), 'ONLINE_INVALID_RESPONSE'),
                 (lambda: self.wire_content(None), 'ONLINE_INVALID_RESPONSE'),
                 (lambda: self.wire(self.valid, finish='content_filter'), 'ONLINE_INVALID_RESPONSE'),
                 (lambda: HTTPError('https://provider.invalid', 401, self.settings.api_key, {}, None), 'ONLINE_AUTH_ERROR'),
                 (lambda: HTTPError('https://provider.invalid', 429, self.settings.api_key, {}, None), 'ONLINE_RATE_LIMITED'),
                 (lambda: socket.timeout(self.settings.api_key), 'ADVISOR_TIMEOUT'),
                 (lambda: URLError(self.settings.api_key), 'ONLINE_UNAVAILABLE'))
        for make_failure, code in cases:
            with self.subTest(code=code):
                self.open.reset_mock()
                self.open.side_effect = [self.wire_content('{'), make_failure(), self.wire(self.valid)]
                with self.assertLogs(llm.logger.name, level='WARNING') as logs, self.assertRaises(ApiError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.code, code)
                self.assertEqual(self.open.call_count, 2)
                if isinstance(caught.exception, llm.ModelOutputError):
                    self.assertFalse(caught.exception.retryable)
                self.assertNotIn('NEVER_LOG_PROVIDER_BODY', '\n'.join(logs.output))
                self.assertNotIn(self.settings.api_key, '\n'.join(logs.output))
                self.assertNotIn(self.settings.api_key, caught.exception.message)

    def test_planner_corrects_invalid_topics_and_shapes_without_echoing_the_bad_plan(self):
        valid = {'kind': 'analysis', 'topics': ['overview', 'bottlenecks']}
        invalid = ({'kind': 'analysis', 'topics': []}, {'kind': 'analysis', 'topics': ['SELECT * FROM users']},
                   {'kind': 'analysis', 'topics': ['overview', 'overview']},
                   {'kind': 'analysis', 'topics': ['overview', 'stations', 'behavior', 'models']},
                   {'kind': 'chat', 'topics': ['overview']}, {'kind': [], 'topics': []},
                   {'kind': 'analysis', 'topics': [4]}, {'kind': 'analysis'},
                   {'kind': 'analysis', 'topics': ['overview'], 'draft': 'BAD_PLAN_ONLY'})
        corrections = set()
        for bad in invalid:
            with self.subTest(plan=bad):
                self.queue(bad, valid)
                with self.assertLogs(llm.logger.name, level='WARNING') as logs:
                    self.assertEqual(self.plan(), valid)
                self.assertEqual(self.open.call_count, 2)
                self.assertIn('stage=plan reason=plan attempt=1 retryable=True', logs.output[0])
                first_body, first = self.sent(0)
                second_body, second = self.sent(1)
                self.assertEqual(set(first), {'question', 'history', 'scope'})
                self.assertEqual({key: value for key, value in second.items() if key != 'formatCorrection'}, first)
                self.assertEqual(first_body['messages'][0], second_body['messages'][0])
                self.assertEqual(first_body['max_tokens'], 600)
                self.assertEqual(second_body['max_tokens'], 600)
                corrections.add(second['formatCorrection'])
                self.assertNotIn('BAD_PLAN_ONLY', json.dumps(second_body))
                self.assertNotIn('SELECT * FROM users', json.dumps(second_body))
        self.assertEqual(len(corrections), 1)

    def test_planner_json_and_truncation_corrections_use_the_same_two_attempt_budget(self):
        valid = {'kind': 'analysis', 'topics': ['overview']}
        for label, first in (('format', lambda: self.wire_content('{"kind":')),
                             ('truncated', lambda: self.wire(valid, finish='length'))):
            with self.subTest(reason=label):
                self.open.reset_mock()
                self.open.side_effect = [first(), self.wire(valid)]
                with self.assertLogs(llm.logger.name, level='WARNING') as logs:
                    self.assertEqual(self.plan(), valid)
                self.assertEqual(self.open.call_count, 2)
                self.assertIn('stage=plan reason=' + label, logs.output[0])
                self.open.reset_mock()
                self.open.side_effect = [first(), self.wire({'kind': 'analysis', 'topics': []}), self.wire(valid)]
                with self.assertLogs(llm.logger.name, level='WARNING'), self.assertRaises(llm.ModelOutputError) as caught:
                    self.plan()
                self.assertEqual(caught.exception.reason, 'plan')
                self.assertEqual(caught.exception.status, 502)
                self.assertEqual(self.open.call_count, 2)

    def test_planner_accepts_complete_packaging_and_does_not_retry_outer_or_transport_errors(self):
        valid = {'kind': 'chat', 'topics': []}
        self.open.side_effect = [self.wire_content('\ufeff```JSON\r\n' + json.dumps(valid) + '\r\n```')]
        self.assertEqual(self.plan(), valid)
        self.open.assert_called_once()
        for first in (lambda: io.BytesIO(b'not-json'), lambda: self.wire_content(None),
                      lambda: self.wire(valid, message_extra={'tool_calls': [{'name': 'secret-action'}]}),
                      lambda: self.wire(valid, finish='content_filter'),
                      lambda: HTTPError('https://provider.invalid', 401, self.settings.api_key, {}, None),
                      lambda: URLError(self.settings.api_key)):
            self.open.reset_mock()
            self.open.side_effect = [first(), self.wire(valid)]
            with self.assertRaises(ApiError):
                self.plan()
            self.open.assert_called_once()

    def test_planner_correction_reuses_its_budget_and_respects_expiry_and_cancellation(self):
        bad, valid = {'kind': 'analysis', 'topics': []}, {'kind': 'analysis', 'topics': ['overview']}
        self.queue(bad, valid)
        with patch.object(llm.time, 'monotonic', side_effect=[100, 102, 111.5]):
            self.assertEqual(self.plan(timeout=30), valid)
        self.assertEqual([call.kwargs['timeout'] for call in self.open.call_args_list], [28, 18.5])
        self.queue(bad, valid)
        with patch.object(llm.time, 'monotonic', side_effect=[100, 100, 131]), self.assertRaises(ApiError) as caught:
            self.plan(timeout=30)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.open.assert_called_once()
        cancelled = threading.Event()
        cancelled.set()
        self.queue(valid)
        with self.assertRaises(ApiError) as caught:
            self.plan(cancelled=cancelled)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.open.assert_not_called()
        cancelled.clear()

        def cancel_after_first(*args, **kwargs):
            cancelled.set()
            return self.wire(bad)

        self.open.side_effect = cancel_after_first
        with self.assertRaises(ApiError) as caught:
            self.plan(cancelled=cancelled)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.open.assert_called_once()

    def test_validation_logs_contain_only_safe_stage_reason_and_attempt_metadata(self):
        markers = ['BAD_DRAFT_ONLY', 'NEVER_LOG_QUESTION', 'NEVER_LOG_HISTORY', 'invented.secret_reference',
                   self.settings.api_key, '/Users/private/source.json', 'NEVER_LOG_REASONING']
        payload = deepcopy(self.payload)
        payload.update(question=markers[1], history=[{'role': 'user', 'content': markers[2]}])
        bad = {'answer': '；'.join(markers) + '[invented.secret_reference]', 'citations': ['invented.secret_reference']}
        self.open.side_effect = [self.wire_content('；'.join(markers)),
                                self.wire(bad, message_extra={'reasoning_content': markers[-1]})]
        with self.assertLogs(llm.logger.name, level='WARNING') as logs, self.assertRaises(llm.ModelOutputError) as caught:
            llm.generate_answer(self.settings, payload)
        self.assertEqual(caught.exception.reason, 'citations')
        self.assertEqual(len(logs.records), 2)
        for record in logs.records:
            self.assertRegex(record.getMessage(), r'^advisor_model_output stage=answer reason=(format|citations) attempt=[12] retryable=True$')
            self.assertIsNone(record.exc_info)
        for marker in markers:
            self.assertNotIn(marker, '\n'.join(logs.output))
            self.assertNotIn(marker, caught.exception.message)
        second = self.sent(1)[1]
        self.assertEqual(second['question'], markers[1])
        self.assertEqual(second['history'], payload['history'])
        for marker in (markers[0], markers[3], *markers[4:]):
            self.assertNotIn(marker, json.dumps(second))

    def rag_context(self):
        metadata = dict(datasetId='fixture-dataset', publishedBatchId='fixture-batch', source='SIMULATED',
                        startDate='2026-01-01', endDate='2026-02-01')

        def dimensions(sql, parameters=()):
            if sql == 'SELECT city_id, city_name FROM cities':
                return [{'city_id': 'C1', 'city_name': '测试城市'}]
            if sql == 'SELECT station_id, station_name, city_id FROM station_snapshot':
                return [{'station_id': 'ST-1', 'station_name': '测试电站', 'city_id': 'C1'}]
            raise AssertionError('Unexpected access outside published dimensions')

        snapshot = SimpleNamespace(metadata=metadata, rows=dimensions)
        query = SimpleNamespace(question='当前筛选的电量情况？', history=[], mode='online',
            datasetId=metadata['datasetId'], publishedBatchId=metadata['publishedBatchId'],
            startDate='2026-01-08', endDate='2026-01-15', cityId=None, stationId=None)
        evidence = dict(self.payload['evidence'][0], source={'endpoint': '/api/v1/dashboard/overview', 'field': 'metrics.energyWh / 1000'})
        packet = dict(evidence=[evidence], observed=True, limitations=[], suggestions=[], observedTopics=['overview'])
        settings = config.OnlineSettings(self.settings.api_key, self.settings.base_url, self.settings.model,
                                         self.settings.provider, timeout=60)
        return snapshot, query, packet, settings

    def test_plan_analysis_and_generation_retries_share_the_original_sixty_second_deadline(self):
        snapshot, query, packet, settings = self.rag_context()
        clock = [100.0]
        replies = [self.wire_content('{"kind":'), self.wire({'kind': 'analysis', 'topics': ['overview']}),
                   self.wire_content('{"answer":'), self.wire(self.valid)]
        elapsed = [11, 7, 9, 8]

        def receive(*args, **kwargs):
            clock[0] += elapsed.pop(0)
            return replies.pop(0)

        def aggregate(*args, **kwargs):
            clock[0] += 13
            return packet

        self.open.side_effect = receive
        cancelled = threading.Event()
        with patch.object(llm.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(rag.analysis, 'build_analysis', side_effect=aggregate) as build, \
             patch.object(rag.knowledge, 'retrieve', return_value=[]), \
             self.assertLogs(llm.logger.name, level='WARNING') as logs:
            result = rag.answer(snapshot, None, query, settings=settings, deadline=160, cancelled=cancelled)
        self.assertEqual(result['answer'], self.valid['answer'])
        self.assertEqual(result['citations'], self.valid['citations'])
        self.assertEqual(self.open.call_count, 4)
        self.assertEqual([call.kwargs['timeout'] for call in self.open.call_args_list], [60, 49, 29, 20])
        self.assertEqual([self.sent(index)[0]['max_tokens'] for index in range(4)], [600, 600, 3000, 3000])
        self.assertIn('formatCorrection', self.sent(1)[1])
        self.assertIn('citationCorrection', self.sent(3)[1])
        self.assertEqual(build.call_args.kwargs, {'deadline': 160, 'cancelled': cancelled})
        self.assertIn('stage=plan reason=format', logs.output[0])
        self.assertIn('stage=answer reason=format', logs.output[1])

    def test_rag_rejects_late_or_cancelled_valid_results_at_each_stage_boundary(self):
        for stage in ('plan', 'answer'):
            for interruption in ('expired', 'cancelled'):
                with self.subTest(stage=stage, interruption=interruption):
                    snapshot, query, packet, settings = self.rag_context()
                    clock = [100.0]
                    cancelled = threading.Event()
                    self.open.reset_mock()
                    replies = [self.wire({'kind': 'analysis', 'topics': ['overview']}), self.wire(self.valid)]

                    def receive(*args, **kwargs):
                        current_stage = 'plan' if len(replies) == 2 else 'answer'
                        clock[0] += 5
                        if current_stage == stage:
                            if interruption == 'cancelled':
                                cancelled.set()
                            else:
                                clock[0] = 161
                        return replies.pop(0)

                    self.open.side_effect = receive
                    with patch.object(llm.time, 'monotonic', side_effect=lambda: clock[0]), \
                         patch.object(rag.analysis, 'build_analysis', return_value=packet) as build, \
                         patch.object(rag.knowledge, 'retrieve', return_value=[]), self.assertRaises(ApiError) as caught:
                        rag.answer(snapshot, None, query, settings=settings, deadline=160, cancelled=cancelled)
                    self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
                    self.assertEqual(self.open.call_count, 1 if stage == 'plan' else 2)
                    self.assertEqual(build.call_count, 0 if stage == 'plan' else 1)

    def test_allowed_ids_are_explicit_and_success_does_not_mutate_original_context(self):
        original = deepcopy(self.payload)
        self.queue(self.valid)
        self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
        self.open.assert_called_once()
        body, context = self.sent(0)
        self.assertEqual(context, dict(original, allowedCitationIds=[
            'overview.energy', 'overview.utilization', 'knowledge.utilization']))
        self.assertEqual([message['role'] for message in body['messages']], ['system', 'user'])
        self.assertNotIn('fake-citation-test-key', json.dumps(body))
        self.assertEqual(self.payload, original)

    def test_one_correction_regenerates_from_original_context_without_echoing_bad_draft(self):
        original = deepcopy(self.payload)
        bad = {'answer': 'BAD_DRAFT_ONLY：电量999999。[invented.metric]', 'citations': ['invented.metric']}
        self.queue(bad, self.valid)
        self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
        self.assertEqual(self.open.call_count, 2)
        first_body, first = self.sent(0)
        second_body, second = self.sent(1)
        self.assertNotIn('citationCorrection', first)
        self.assertIsInstance(second['citationCorrection'], str)
        self.assertTrue(second['citationCorrection'])
        self.assertEqual({key: value for key, value in second.items() if key != 'citationCorrection'}, first)
        self.assertEqual(second_body['messages'][0], first_body['messages'][0])
        self.assertEqual([message['role'] for message in second_body['messages']], ['system', 'user'])
        for defective_text in ('BAD_DRAFT_ONLY', '999999', 'invented.metric'):
            self.assertNotIn(defective_text, json.dumps(second_body))
        self.assertEqual(self.payload, original)

    def test_second_invalid_citation_reply_fails_without_a_third_attempt(self):
        bad = {'answer': '无效来源。[missing.metric]', 'citations': ['missing.metric']}
        self.queue(bad, bad, self.valid)
        with self.assertRaises(ApiError) as caught:
            llm.generate_answer(self.settings, self.payload)
        self.assertEqual((caught.exception.status, caught.exception.code), (502, 'ONLINE_INVALID_RESPONSE'))
        self.assertEqual(self.open.call_count, 2)

    def test_unknown_ids_on_either_side_and_missing_body_references_require_correction(self):
        cases = (
            {'answer': '来源未知。[unknown.metric]', 'citations': ['overview.energy']},
            {'answer': '来源未知。[unknown.metric]', 'citations': ['unknown.metric']},
            {'answer': '正文已知但列表未知。[overview.energy]', 'citations': ['overview.energy', 'unknown.metric']},
            {'answer': '正文只用简称。[energy]', 'citations': ['overview.energy']},
            {'answer': '列表只用简称。[overview.energy]', 'citations': ['energy']},
            {'answer': '正文缺少引用。', 'citations': ['overview.energy']},
            {'answer': '正文和数组都没有引用。', 'citations': []},
        )
        corrections = set()
        for bad in cases:
            with self.subTest(answer=bad['answer']):
                self.queue(bad, self.valid)
                self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
                self.assertEqual(self.open.call_count, 2)
                corrections.add(self.sent(1)[1]['citationCorrection'])
        self.assertEqual(len(corrections), 1)

    def test_known_metadata_missing_extra_and_reordered_ids_follow_body_without_regeneration(self):
        cases = (
            ({'answer': '只引用电量。[overview.energy]', 'citations': ['knowledge.utilization']}, ['overview.energy']),
            ({'answer': '数组少一项。[overview.energy][knowledge.utilization]', 'citations': ['overview.energy']},
             ['overview.energy', 'knowledge.utilization']),
            ({'answer': '数组多一项。[overview.energy]', 'citations': ['overview.energy', 'knowledge.utilization']}, ['overview.energy']),
            ({'answer': '数组为空。[overview.energy]', 'citations': []}, ['overview.energy']),
            ({'answer': '正文先给口径。[knowledge.utilization]再给统计。[overview.energy]',
              'citations': ['overview.energy', 'knowledge.utilization']}, ['knowledge.utilization', 'overview.energy']),
        )
        for output, expected in cases:
            with self.subTest(citations=output['citations']):
                self.queue(output, self.valid)
                result = llm.generate_answer(self.settings, dict(self.payload, kind='analysis'))
                self.assertEqual(result, {'answer': output['answer'], 'citations': expected})
                self.open.assert_called_once()
                self.assertNotIn('citationCorrection', self.sent(0)[1])

    def test_exact_chinese_and_group_citations_normalize_presentation_without_changing_numbers(self):
        prefix = '保留 [1]、[12.5]、【2】和日期 [2026-01-01]。电量12.5 kWh，利用率25%。'
        for opening, closing in (('[', ']'), ('【', '】')):
            for separator in (',', '，', '、', ';', '；'):
                with self.subTest(wrapper=opening, separator=separator):
                    text = prefix + opening + ' knowledge.utilization ' + separator + ' overview.energy ' + closing
                    output = {'answer': text, 'citations': ['overview.energy', 'overview.utilization', 'knowledge.utilization']}
                    self.queue(output, self.valid)
                    result = llm.generate_answer(self.settings, dict(self.payload, kind='analysis'))
                    self.assertEqual(result, {'answer': prefix + '[knowledge.utilization][overview.energy]',
                                             'citations': ['knowledge.utilization', 'overview.energy']})
                    self.open.assert_called_once()
        self.queue({'answer': prefix + '【overview.energy】', 'citations': []})
        self.assertEqual(llm.generate_answer(self.settings, self.payload),
                         {'answer': prefix + '[overview.energy]', 'citations': ['overview.energy']})
        self.open.assert_called_once()

    def test_unknown_group_members_or_metadata_never_get_guessed_or_silently_dropped(self):
        for reference, metadata in (
            ('[overview.energy, unknown.metric]', ['overview.energy']),
            ('【overview.energy；unknown.metric】', []),
            ('[overview.energy]', ['overview.energy', 'unknown.metric']),
            ('[energy]', ['overview.energy']), ('[overview.energy]', ['energy']),
        ):
            with self.subTest(reference=reference, metadata=metadata):
                bad = {'answer': '12.5 kWh。' + reference, 'citations': metadata}
                self.queue(bad, bad, self.valid)
                with self.assertLogs(llm.logger.name, level='WARNING'), self.assertRaises(llm.ModelOutputError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.reason, 'citations')
                self.assertEqual(self.open.call_count, 2)
                self.assertNotIn('unknown.metric', json.dumps(self.sent(1)[1]))

    def test_malformed_mixed_nested_or_half_citations_cannot_hide_beside_a_valid_reference(self):
        malformed = ('[overview.energy,]', '[overview.energy, unknown]', '[overview.energy / knowledge.utilization]',
                     '[overview.energy', 'overview.energy]', '【overview.energy]', '[overview.energy】',
                     '[[overview.energy]]', '【[overview.energy]】', '[overview.energy [knowledge.utilization]]',
                     '[unknown.metric】', '【unknown.metric]', '[unknown.metric [overview.energy]]', '[unknown.metric',
                     '[overview.energy] [unknown.metric')
        for reference in malformed:
            with self.subTest(reference=reference):
                bad = {'answer': '不改数字12.5。' + reference + '。另一个合法引用[overview.energy]', 'citations': ['overview.energy']}
                self.queue(bad, bad, self.valid)
                with self.assertLogs(llm.logger.name, level='WARNING'), self.assertRaises(llm.ModelOutputError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.reason, 'citations')
                self.assertEqual(self.open.call_count, 2)

    def test_history_reference_removal_preserves_all_nonreference_text_and_numeric_brackets(self):
        original = '第[1]项：电量12.5 kWh[overview.energy]。\n占比25%【overview.utilization】；'
        original += '对比[overview.energy，knowledge.utilization]和【unknown.metric; knowledge.utilization】。保留[12.5]【2】[备注]。'
        expected = '第[1]项：电量12.5 kWh。\n占比25%；对比和。保留[12.5]【2】[备注]。'
        self.assertEqual(llm.without_history_citations(original), expected)
        self.assertIn('[overview.energy]', original)

    def test_valid_duplicate_metadata_and_inline_citations_are_deduplicated_without_retry(self):
        output = {'answer': '先看统计。[overview.energy]再看口径。[knowledge.utilization]再次核对。[overview.energy]',
            'citations': ['knowledge.utilization', 'overview.energy', 'knowledge.utilization', 'overview.energy']}
        self.queue(output)
        result = llm.generate_answer(self.settings, self.payload)
        self.assertEqual(result['answer'], output['answer'])
        self.assertEqual(result['citations'], ['overview.energy', 'knowledge.utilization'])
        self.open.assert_called_once()

    def test_analysis_with_only_knowledge_citations_requires_an_evidence_correction(self):
        payload = dict(self.payload, kind='analysis')
        knowledge_only = {'answer': '利用率先合计分子与分母。[knowledge.utilization]',
            'citations': ['knowledge.utilization']}
        self.queue(knowledge_only, self.valid)
        self.assertEqual(llm.generate_answer(self.settings, payload), self.valid)
        self.assertEqual(self.open.call_count, 2)
        self.queue(knowledge_only, knowledge_only)
        with self.assertRaises(ApiError) as caught:
            llm.generate_answer(self.settings, payload)
        self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
        self.assertEqual(self.open.call_count, 2)
        self.queue(knowledge_only)
        self.assertEqual(llm.generate_answer(self.settings, self.payload), knowledge_only)
        self.open.assert_called_once()

    def test_chat_allows_no_citations_and_corrects_an_accidental_reference_once(self):
        payload = dict(self.payload, kind='chat')
        chat = {'answer': '你好，可以继续提问。', 'citations': []}
        self.queue(chat)
        self.assertEqual(llm.generate_answer(self.settings, payload), chat)
        self.open.assert_called_once()
        self.queue(self.valid, chat)
        self.assertEqual(llm.generate_answer(self.settings, payload), chat)
        self.assertEqual(self.open.call_count, 2)

    def test_answer_shape_errors_share_one_fresh_generation_retry(self):
        cases = (
            {'answer': '', 'citations': []}, {'answer': '   ', 'citations': []},
            {'answer': 123, 'citations': []}, {'answer': 'x' * 6001, 'citations': []},
            {'answer': chr(0xD83D), 'citations': []}, {'answer': '缺字段'},
            {**self.valid, 'action': 'pay'}, {**self.valid, 'citations': None},
            {**self.valid, 'citations': 'overview.energy'}, {**self.valid, 'citations': [17]},
            {**self.valid, 'citations': ['x' * 129]}, {**self.valid, 'citations': ['overview.energy'] * 41},
        )
        for output in cases:
            with self.subTest(shape=str(output)[:70]):
                self.queue(output, self.valid)
                self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
                self.assertEqual(self.open.call_count, 2)
                _, first = self.sent(0)
                _, second = self.sent(1)
                self.assertEqual({key: value for key, value in second.items() if key != 'citationCorrection'}, first)
                self.queue(output, output, self.valid)
                with self.assertRaises(llm.ModelOutputError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual((caught.exception.status, caught.exception.code), (502, 'ONLINE_INVALID_RESPONSE'))
                self.assertIn(caught.exception.reason, ('shape', 'format'))
                self.assertTrue(caught.exception.retryable)
                self.assertEqual(self.open.call_count, 2)

    def test_outer_envelope_tools_filter_and_nonstring_content_are_not_retried(self):
        cases = (
            io.BytesIO(b'not-json'),
            io.BytesIO(b'{}'), io.BytesIO(b'{"choices":[]}'), io.BytesIO(b'{"choices":[{}]}'),
            io.BytesIO(b'x' * 131073),
            io.BytesIO(b'[' * 1200 + b'0' + b']' * 1200),
            self.wire(self.valid, message_extra={'tool_calls': [{'name': 'pay'}]}),
            self.wire(self.valid, message_extra={'function_call': {'name': 'read_files'}}),
            self.wire(self.valid, finish='tool_calls'), self.wire(self.valid, finish='content_filter'),
            self.wire(self.valid, message_extra={'content': None}),
            self.wire(self.valid, message_extra={'content': [{'text': 'not compatible content'}]}),
        )
        for response in cases:
            with self.subTest(response_number=cases.index(response)):
                self.open.reset_mock()
                self.open.side_effect = [response, self.wire(self.valid)]
                with self.assertRaises(llm.ModelOutputError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
                self.assertFalse(caught.exception.retryable)
                self.assertIn(caught.exception.reason, ('envelope', 'blocked'))
                self.open.assert_called_once()

    def assert_outer_rejected_without_retry(self, raw, reason='envelope'):
        for stage in ('plan', 'answer'):
            with self.subTest(stage=stage):
                self.open.reset_mock()
                valid = {'kind': 'chat', 'topics': []} if stage == 'plan' else self.valid
                self.open.side_effect = [io.BytesIO(raw), self.wire(valid)]
                with self.assertLogs(llm.logger.name, level='WARNING') as logs, self.assertRaises(llm.ModelOutputError) as caught:
                    if stage == 'plan':
                        self.plan()
                    else:
                        llm.generate_answer(self.settings, self.payload)
                self.assertEqual((caught.exception.status, caught.exception.code, caught.exception.reason),
                                 (502, 'ONLINE_INVALID_RESPONSE', reason))
                self.assertFalse(caught.exception.retryable)
                self.open.assert_called_once()
                self.assertEqual(len(logs.records), 1)
                self.assertEqual(logs.records[0].getMessage(),
                    f'advisor_model_output stage={stage} reason={reason} attempt=1 retryable=False')
                self.assertIsNone(logs.records[0].exc_info)
                self.assertNotIn('NEVER_LOG_ENVELOPE', '\n'.join(logs.output))

    def test_duplicate_outer_keys_are_rejected_at_every_envelope_level(self):
        content = json.dumps(json.dumps(self.valid, ensure_ascii=False))
        message = '{"content":' + content + '}'
        choice = '{"message":' + message + ',"finish_reason":"stop"}'
        cases = (
            '{"choices":[],"choices":[' + choice + ']}',
            '{"choices":[' + choice + '],"usage":{"total_tokens":1,"total_tokens":2}}',
            '{"choices":[{"message":' + message + ',"finish_reason":"length","finish_reason":"stop"}]}',
            '{"choices":[{"message":{"content":"NEVER_LOG_ENVELOPE","content":' + content + '},"finish_reason":"stop"}]}',
            '{"choices":[{"message":{"content":"NEVER_LOG_ENVELOPE"},"message":' + message + ',"finish_reason":"stop"}]}',
        )
        for index, raw in enumerate(cases):
            with self.subTest(envelope_level=index):
                self.assert_outer_rejected_without_retry(raw.encode())

    def test_nonfinite_outer_values_are_rejected_even_in_unused_usage_fields(self):
        choice = json.dumps({'message': {'content': json.dumps(self.valid)}, 'finish_reason': 'stop'})
        for number in ('NaN', 'Infinity', '-Infinity', '1e999', '-1e999'):
            with self.subTest(number=number):
                raw = '{"choices":[' + choice + '],"usage":{"untrusted_score":' + number + '}}'
                self.assert_outer_rejected_without_retry(raw.encode())
        # An ordinary finite metadata value is not a reason to reject content.
        self.open.reset_mock()
        self.open.side_effect = [io.BytesIO(('{"choices":[' + choice + '],"usage":{"score":1e308}}').encode())]
        self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
        self.open.assert_called_once()

    def test_outer_choices_must_contain_exactly_one_message(self):
        choice = {'message': {'content': json.dumps(self.valid)}, 'finish_reason': 'stop'}
        for choices in (None, {}, [], [choice, choice], 'one', 1, [None], [[]]):
            with self.subTest(choices_type=type(choices).__name__, size=len(choices) if isinstance(choices, list) else None):
                self.assert_outer_rejected_without_retry(json.dumps({'choices': choices}).encode())

    def test_only_stop_finishes_normally_and_blocked_or_unknown_reasons_never_retry(self):
        for finish, reason in ((None, 'envelope'), ('', 'envelope'), ('unknown', 'envelope'),
                              ('error', 'envelope'), ('STOP', 'envelope'), (False, 'envelope'),
                              ([], 'envelope'), ({}, 'envelope'), ('content_filter', 'blocked'),
                              ('tool_calls', 'blocked'), ('function_call', 'blocked')):
            with self.subTest(finish=finish):
                envelope = {'choices': [{'message': {'content': json.dumps(self.valid)}, 'finish_reason': finish}]}
                self.assert_outer_rejected_without_retry(json.dumps(envelope).encode(), reason)
        self.assert_outer_rejected_without_retry(json.dumps({'choices': [{'message': {'content': json.dumps(self.valid)}}]}).encode())

    def test_length_cannot_hide_tools_refusal_or_nonstring_content(self):
        for extra, reason in (({'content': None}, 'envelope'), ({'content': {}}, 'envelope'),
                              ({'content': [{'text': 'NEVER_LOG_ENVELOPE'}]}, 'envelope'),
                              ({'tool_calls': [{'name': 'NEVER_LOG_ENVELOPE'}]}, 'envelope'),
                              ({'function_call': {'name': 'NEVER_LOG_ENVELOPE'}}, 'envelope'),
                              ({'refusal': 'NEVER_LOG_ENVELOPE'}, 'blocked')):
            for finish in ('length', 'stop'):
                with self.subTest(extra_field=next(iter(extra)), finish=finish):
                    message = {'content': json.dumps(self.valid), **extra}
                    raw = json.dumps({'choices': [{'message': message, 'finish_reason': finish}]}).encode()
                    self.assert_outer_rejected_without_retry(raw, reason)
        self.open.reset_mock()
        self.open.side_effect = [self.wire(self.valid, finish='length', message_extra={'refusal': None, 'tool_calls': [], 'function_call': None}),
                                 self.wire(self.valid)]
        with self.assertLogs(llm.logger.name, level='WARNING') as logs:
            self.assertEqual(llm.generate_answer(self.settings, self.payload), self.valid)
        self.assertEqual(self.open.call_count, 2)
        self.assertIn('reason=truncated attempt=1 retryable=True', logs.output[0])

    def test_authentication_and_other_transport_failures_are_not_retried(self):
        cases = [(HTTPError('https://provider', status, 'fake-citation-test-key', {}, None), code)
            for status, code in ((401, 'ONLINE_AUTH_ERROR'), (403, 'ONLINE_AUTH_ERROR'),
                (400, 'ONLINE_MODEL_ERROR'), (404, 'ONLINE_MODEL_ERROR'),
                (429, 'ONLINE_RATE_LIMITED'), (500, 'ONLINE_UNAVAILABLE'))]
        cases.extend([(socket.timeout('fake-citation-test-key'), 'ADVISOR_TIMEOUT'),
            (URLError('fake-citation-test-key'), 'ONLINE_UNAVAILABLE'),
            (IncompleteRead(b'fake-citation-test-key', 10), 'ONLINE_UNAVAILABLE')])
        for failure, code in cases:
            with self.subTest(failure=type(failure).__name__, code=code):
                self.open.reset_mock()
                self.open.side_effect = [failure, self.wire(self.valid)]
                with self.assertRaises(ApiError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.code, code)
                self.assertNotIn('fake-citation-test-key', caught.exception.message)
                self.open.assert_called_once()

    def test_correction_uses_only_the_original_remaining_time_budget(self):
        bad = {'answer': '无引用正文。', 'citations': []}
        for timeout, expected in ((30, [28, 18.5]), (None, [43, 33.5])):
            with self.subTest(timeout=timeout):
                self.queue(bad, self.valid)
                with patch.object(llm.time, 'monotonic', side_effect=[100, 102, 111.5]):
                    self.assertEqual(llm.generate_answer(self.settings, self.payload, timeout=timeout), self.valid)
                self.assertEqual([call.kwargs['timeout'] for call in self.open.call_args_list], expected)

    def test_expired_original_budget_prevents_the_correction_request(self):
        self.queue({'answer': '需要修复引用。', 'citations': []}, self.valid)
        with patch.object(llm.time, 'monotonic', side_effect=[100, 100, 131]), self.assertRaises(ApiError) as caught:
            llm.generate_answer(self.settings, self.payload, timeout=30)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.open.assert_called_once()

    def test_preexisting_cancellation_prevents_any_request(self):
        cancelled = threading.Event()
        cancelled.set()
        self.queue(self.valid)
        with self.assertRaises(ApiError) as caught:
            llm.generate_answer(self.settings, self.payload, cancelled=cancelled)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.opener.assert_not_called()

    def test_cancellation_after_first_attempt_prevents_the_correction_request(self):
        cancelled = threading.Event()

        def first_reply(*args, **kwargs):
            cancelled.set()
            return self.wire({'answer': '需要修复引用。', 'citations': []})

        self.open.side_effect = first_reply
        with self.assertRaises(ApiError) as caught:
            llm.generate_answer(self.settings, self.payload, cancelled=cancelled)
        self.assertEqual(caught.exception.code, 'ADVISOR_TIMEOUT')
        self.open.assert_called_once()


if __name__ == '__main__':
    unittest.main()
