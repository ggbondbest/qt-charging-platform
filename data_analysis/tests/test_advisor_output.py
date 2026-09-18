"""Bounded model JSON parsing repairs only literal line and tab characters."""
import json
import unittest
from unittest.mock import patch

from data_analysis.ml.advisor import output


class AdvisorOutputTests(unittest.TestCase):
    def assert_rejected(self, content):
        with self.assertRaises(ValueError) as raised:
            output.parse_object(content)
        reason = str(raised.exception)
        self.assertRegex(reason, r"^[a-z][a-z0-9_-]{0,63}$")
        return reason

    def test_constants_are_the_public_resource_limits(self):
        self.assertEqual(output.MAX_CONTENT_BYTES, 131072)
        self.assertEqual(output.MAX_DEPTH, 64)

    def test_nonstring_inputs_are_value_errors_with_safe_codes(self):
        for content in (None, False, 1, {}, [], b'{"answer":"text"}'):
            with self.subTest(content_type=type(content).__name__):
                self.assert_rejected(content)

    def test_complete_objects_preserve_json_values_and_unicode(self):
        expected = {
            "answer": '中文 🚀 {braces} [brackets] "quotes" \\ slash /',
            "nested": [{"value": True}, None, False, 12, -2, 1.25, 1e308],
            "A": 1,
            "a": 2,
            "unicode": "\u2028\u2029\ufeff",
        }
        for content in (json.dumps(expected), json.dumps(expected, ensure_ascii=False)):
            with self.subTest(content=content):
                self.assertEqual(output.parse_object(content), expected)
        self.assertEqual(output.parse_object("{}"), {})

    def test_standard_outer_whitespace_and_leading_bom_are_accepted(self):
        for content in (' \t\r\n{"ok":true}\t \r\n', '\ufeff{"ok":true}',
                        '\ufeff \t\r\n{"ok":true}\r\n'):
            with self.subTest(content=content):
                self.assertEqual(output.parse_object(content), {"ok": True})

    def test_whole_markdown_fence_accepts_json_case_and_line_endings(self):
        for language in ("", "json", "JSON", "JsOn"):
            for newline in ("\n", "\r\n", "\r"):
                with self.subTest(language=language, newline=repr(newline)):
                    content = ("\ufeff \t" + newline + "```" + language + newline
                               + '{"ok":true}' + newline + " \t```\t " + newline)
                    self.assertEqual(output.parse_object(content), {"ok": True})

    def test_bare_line_and_tab_characters_are_preserved_in_string_values(self):
        cases = [
            ('{"answer":"first\nsecond\rthird\tlast"}',
             {"answer": "first\nsecond\rthird\tlast"}),
            ('{"answer":"first\r\nsecond\n\n\tlast"}',
             {"answer": "first\r\nsecond\n\n\tlast"}),
            ('{"line\nkey":[{"answer":"tab\tvalue"}]}',
             {"line\nkey": [{"answer": "tab\tvalue"}]}),
        ]
        for content, expected in cases:
            with self.subTest(content=content):
                self.assertEqual(output.parse_object(content), expected)
                self.assertEqual(output.parse_object("```json\n" + content + "\n```"), expected)

    def test_existing_valid_escapes_are_not_changed(self):
        content = r'{"answer":"line\nreturn\rtab\tquote\"slash\\solidus\/unicode\u4e2d","literal":"\\n\\r\\t\\u0000"}'
        expected = {
            "answer": 'line\nreturn\rtab\tquote"slash\\solidus/unicode中',
            "literal": r"\n\r\t\u0000",
        }
        self.assertEqual(output.parse_object(content), expected)

    def test_backslash_parity_and_escaped_quotes_preserve_string_boundaries(self):
        for count in (2, 4, 6):
            for control in ("\n", "\r", "\t"):
                with self.subTest(count=count, control=repr(control)):
                    content = '{"answer":"left' + "\\" * count + control + 'right"}'
                    self.assertEqual(output.parse_object(content), {
                        "answer": "left" + "\\" * (count // 2) + control + "right",
                    })
        content = r'{"answer":"say \"hello\"' + '\nnext","ok":true}'
        self.assertEqual(output.parse_object(content), {"answer": 'say "hello"\nnext', "ok": True})

    def test_odd_backslash_before_bare_line_or_tab_is_rejected(self):
        for count in (1, 3, 5):
            for control in ("\n", "\r", "\t"):
                with self.subTest(count=count, control=repr(control)):
                    self.assert_rejected('{"answer":"left' + "\\" * count + control + 'right"}')

    def test_line_and_tab_whitespace_outside_strings_remains_valid(self):
        content = '{\r\n\t"items"\t:\n[\r1,\n2\t],\r"ok"\n:\ttrue\n}'
        self.assertEqual(output.parse_object(content), {"items": [1, 2], "ok": True})

    def test_duplicate_keys_are_rejected_at_every_object_level(self):
        cases = (
            '{"answer":1,"answer":2}',
            '{"nested":{"answer":1,"answer":1}}',
            '{"nested":[{"answer":1,"answer":2}]}',
            r'{"a":1,"\u0061":2}',
            '{"line\nkey":1,"line\\nkey":2}',
        )
        for content in cases:
            with self.subTest(content=content):
                self.assert_rejected(content)

    def test_nonfinite_numbers_are_rejected_including_float_overflow(self):
        for literal in ("NaN", "Infinity", "-Infinity", "1e9999", "-1E9999"):
            for content in ('{"value":' + literal + '}', '{"nested":[{"value":' + literal + '}]}'):
                with self.subTest(content=content):
                    self.assert_rejected(content)

    def test_top_level_value_must_be_an_object(self):
        for content in ('[]', '[{}]', 'null', 'true', 'false', '0', '1.25', '"{}"', '"answer"'):
            with self.subTest(content=content):
                self.assert_rejected(content)
                self.assert_rejected("```json\n" + content + "\n```")

    def test_explanations_and_multiple_objects_are_not_sliced_out(self):
        for content in (
            'Here is the JSON: {"answer":"ok"}',
            '{"answer":"ok"} Thanks!',
            'prefix {"answer":"ok"} suffix',
            '{}{}', '{}\n{}', '{"answer":1}, {"answer":2}',
            'before\n```json\n{}\n```', '```json\n{}\n```\nafter',
            '```json\n{}\n{}\n```',
        ):
            with self.subTest(content=content):
                self.assert_rejected(content)

    def test_malformed_json_is_not_repaired(self):
        for content in (
            '', ' \r\n\t ', '{', '{"answer":1', '{"answer":}',
            '{"answer":"unfinished}', '{"answer" "value"}',
            '{"a":1 "b":2}', '{"a":1,}', "{'a':1}",
            '{a:1}', '{"a":01}', '{"a":+1}', '{"a":.5}',
            '{"a":True}', '{"a":undefined}', '{"a":1/* comment */}',
            '{"a":1 // comment\n}', r'{"answer":"invalid\q"}',
            r'{"answer":"invalid\u12xz"}', '{"answer":"unescaped "quote""}',
            '{"answer":"a"}}', '[{"answer":"a"}',
        ):
            with self.subTest(content=content):
                self.assert_rejected(content)

    def test_markdown_requires_exact_whole_fence_and_supported_language(self):
        for content in (
            '```json {}\n```', '```{}\n```',
            '```json\n{}```', '```\n{}```',
            '```python\n{}\n```', '```json5\n{}\n```',
            '```json extra\n{}\n```', '```application/json\n{}\n```',
            '````json\n{}\n````', '~~~json\n{}\n~~~',
            '```json\n{}', '{}\n```', '```json\n\n```',
            '```json\n```json\n{}\n```\n```',
            '```json\n{}\n```\n```json\n{}\n```',
            '```json\n{}\n``` extra', '```json\n{}\ntext ```',
            '```json\nexplanation\n{}\n```',
        ):
            with self.subTest(content=content):
                self.assert_rejected(content)

    def test_forbidden_raw_controls_are_rejected_in_keys_values_and_outer_text(self):
        codepoints = [point for point in range(32) if point not in (9, 10, 13)] + list(range(127, 160))
        for point in codepoints:
            char = chr(point)
            for content in ('{"answer":"left' + char + 'right"}',
                            '{"left' + char + 'right":1}', char + '{}', '{}' + char):
                with self.subTest(codepoint=point, content=repr(content)):
                    self.assert_rejected(content)

    def test_forbidden_escaped_controls_are_also_rejected(self):
        codepoints = [point for point in range(32) if point not in (9, 10, 13)] + list(range(127, 160))
        escapes = ["\\u%04x" % point for point in codepoints] + [r"\b", r"\f"]
        for escape in escapes:
            for content in ('{"answer":"left' + escape + 'right"}',
                            '{"left' + escape + 'right":1}',
                            '{"nested":[{"answer":"' + escape + '"}]}'):
                with self.subTest(escape=escape, content=content):
                    self.assert_rejected(content)

    def test_non_json_unicode_whitespace_is_not_trimmed_or_accepted_as_syntax(self):
        for char in ("\u00a0", "\u1680", "\u2003", "\u2028", "\u2029", "\u3000"):
            for content in (char + '{}', '{}' + char, '{' + char + '"ok":true}'):
                with self.subTest(content=repr(content)):
                    self.assert_rejected(content)

    def test_valid_escaped_surrogate_pairs_are_accepted(self):
        self.assertEqual(output.parse_object(r'{"\ud83d\ude80":"before\ud83d\ude80after"}'),
                         {"🚀": "before🚀after"})

    def test_invalid_surrogates_are_rejected_in_keys_and_values(self):
        sequences = (r"\ud800", r"\udfff", r"\ud800x", r"\udfff\ud800",
                     r"\ud800\ud800", r"\ud800\u0041", "\ud800", "\udfff", "\ud83d\ude80")
        for sequence in sequences:
            for content in ('{"answer":"' + sequence + '"}', '{"' + sequence + '":1}'):
                with self.subTest(content=repr(content)):
                    self.assert_rejected(content)

    def test_exact_raw_byte_limit_is_accepted_and_one_more_byte_is_rejected(self):
        overhead = len('{"answer":""}'.encode("utf-8"))
        answer = "a" * (output.MAX_CONTENT_BYTES - overhead)
        content = '{"answer":"' + answer + '"}'
        self.assertEqual(len(content.encode("utf-8")), output.MAX_CONTENT_BYTES)
        self.assertEqual(output.parse_object(content), {"answer": answer})
        self.assert_rejected(content + " ")
        self.assert_rejected("\ufeff" + content)
        self.assert_rejected("```json\n" + content + "\n```")

    def test_content_limit_counts_utf8_bytes_and_includes_fence_whitespace(self):
        overhead = len('{"answer":""}'.encode("utf-8"))
        budget = output.MAX_CONTENT_BYTES - overhead
        answer = "中" * (budget // 3) + "a" * (budget % 3)
        content = '{"answer":"' + answer + '"}'
        self.assertEqual(len(content.encode("utf-8")), output.MAX_CONTENT_BYTES)
        self.assertLess(len(content), output.MAX_CONTENT_BYTES)
        self.assertEqual(output.parse_object(content), {"answer": answer})
        self.assert_rejected(content[:-2] + '中"}')
        prefix, suffix = "\ufeff \t```JSON\r\n", "\r\n``` \t"
        answer = "a" * (output.MAX_CONTENT_BYTES - overhead - len((prefix + suffix).encode("utf-8")))
        fenced = prefix + '{"answer":"' + answer + '"}' + suffix
        self.assertEqual(len(fenced.encode("utf-8")), output.MAX_CONTENT_BYTES)
        self.assertEqual(output.parse_object(fenced), {"answer": answer})
        self.assert_rejected(fenced + " ")

    def test_object_depth_boundary_counts_root_as_one(self):
        expected = {}
        for _ in range(output.MAX_DEPTH - 1):
            expected = {"child": expected}
        content = json.dumps(expected)
        self.assertEqual(output.parse_object(content), expected)
        self.assert_rejected('{"child":' + content + '}')

    def test_depth_counts_nested_arrays_and_ignores_brackets_inside_strings(self):
        expected = "leaf"
        for _ in range(output.MAX_DEPTH - 1):
            expected = [expected]
        accepted = {"nested": expected}
        self.assertEqual(output.parse_object(json.dumps(accepted)), accepted)
        self.assert_rejected(json.dumps({"nested": [expected]}))
        text = ('{[}]("\\)' * (output.MAX_DEPTH + 1))
        self.assertEqual(output.parse_object(json.dumps({"answer": text})), {"answer": text})

    def test_excessive_depth_is_a_value_error_not_a_recursion_error(self):
        self.assert_rejected('{"nested":' + '[' * 2000 + '0' + ']' * 2000 + '}')

    def test_decoder_recursion_failure_is_sanitized_as_value_error(self):
        with patch.object(output.json, "JSONDecoder", side_effect=RecursionError("private-response-detail")):
            self.assert_rejected('{}')

    def test_failure_reasons_are_fixed_codes_without_model_content(self):
        first = self.assert_rejected('{"private-provider-response-A":}')
        second = self.assert_rejected('{"private-provider-response-B":}')
        self.assertEqual(first, second)
        self.assertNotIn("private", first)
        duplicate_a = self.assert_rejected('{"sensitive-key-A":1,"sensitive-key-A":2}')
        duplicate_b = self.assert_rejected('{"sensitive-key-B":1,"sensitive-key-B":2}')
        self.assertEqual(duplicate_a, duplicate_b)
        self.assertNotIn("sensitive", duplicate_a)


if __name__ == "__main__":
    unittest.main()
