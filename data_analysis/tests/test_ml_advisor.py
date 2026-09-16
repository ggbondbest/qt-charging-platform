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


class AdvisorCitationCorrectionTests(unittest.TestCase):
    """Exercise real generation/transport parsing against only in-memory replies."""

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

    def queue(self, *outputs):
        self.open.reset_mock()
        self.open.side_effect = [self.wire(output) for output in outputs]

    def sent(self, index):
        body = json.loads(self.open.call_args_list[index].args[0].data)
        return body, json.loads(body['messages'][1]['content'])

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

    def test_inline_unknown_or_metadata_mismatch_gets_the_same_static_correction(self):
        cases = (
            {'answer': '来源未知。[unknown.metric]', 'citations': ['overview.energy']},
            {'answer': '来源未知。[unknown.metric]', 'citations': ['unknown.metric']},
            {'answer': '来源不一致。[overview.energy]', 'citations': ['knowledge.utilization']},
            {'answer': '数组缺一项。[overview.energy][knowledge.utilization]', 'citations': ['overview.energy']},
            {'answer': '数组多一项。[overview.energy]', 'citations': ['overview.energy', 'knowledge.utilization']},
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

    def test_valid_duplicate_metadata_and_inline_citations_are_deduplicated_without_retry(self):
        output = {'answer': '先看统计。[overview.energy]再看口径。[knowledge.utilization]再次核对。[overview.energy]',
            'citations': ['knowledge.utilization', 'overview.energy', 'knowledge.utilization', 'overview.energy']}
        self.queue(output)
        result = llm.generate_answer(self.settings, self.payload)
        self.assertEqual(result['answer'], output['answer'])
        self.assertEqual(result['citations'], ['knowledge.utilization', 'overview.energy'])
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

    def test_answer_shape_errors_are_not_retried(self):
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
                with self.assertRaises(ApiError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
                self.open.assert_called_once()

    def test_invalid_json_or_provider_message_shapes_are_not_retried(self):
        cases = (
            io.BytesIO(b'not-json'),
            io.BytesIO(json.dumps({'choices': [{'message': {'content': 'not-json'}}]}).encode()),
            self.wire([]), self.wire(self.valid, finish='length'),
            self.wire(self.valid, message_extra={'tool_calls': [{'name': 'pay'}]}),
            self.wire(self.valid, message_extra={'content': None}),
        )
        for response in cases:
            with self.subTest(response_number=cases.index(response)):
                self.open.reset_mock()
                self.open.side_effect = [response, self.wire(self.valid)]
                with self.assertRaises(ApiError) as caught:
                    llm.generate_answer(self.settings, self.payload)
                self.assertEqual(caught.exception.code, 'ONLINE_INVALID_RESPONSE')
                self.open.assert_called_once()

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
