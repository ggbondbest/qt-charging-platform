"""Parse one bounded model JSON object without guessing missing structure.

Only whole-document packaging and literal LF/CR/TAB inside quoted strings are
normalized. Quotes, escapes, keys, commas, numbers and citation text are never
invented or rewritten. Error messages are fixed codes, never model content.
"""

import json
import math
import re


MAX_CONTENT_BYTES = 131_072
MAX_DEPTH = 64
_SPACE = " \t\r\n"
_FENCE = re.compile(
    r"\A```[ \t]*(?:json[ \t]*)?(?:\r\n|\n|\r)"
    r"(?P<body>[\s\S]*?)(?:\r\n|\n|\r)[ \t]*```\Z", re.IGNORECASE,
)
_LITERAL_ESCAPES = {"\n": r"\n", "\r": r"\r", "\t": r"\t"}


class _Rejected(ValueError):
    """An internally selected, safe reason code."""


def _bad_control(char):
    value = ord(char)
    return (value < 32 and char not in _LITERAL_ESCAPES) or 127 <= value <= 159


def _trim_packaging(text):
    text = text.strip(_SPACE)
    if text.startswith("\ufeff"):
        text = text[1:].strip(_SPACE)
    return text


def _normalize_strings(text):
    result = []
    quoted = escaped = False
    depth = 0
    for char in text:
        if quoted:
            if escaped:
                # A backslash followed by a literal control is an invalid
                # escape, not a bare newline that can safely be normalized.
                if char in _LITERAL_ESCAPES:
                    raise _Rejected("invalid_json")
                result.append(char)
                escaped = False
            elif char == "\\":
                result.append(char)
                escaped = True
            elif char == '"':
                result.append(char)
                quoted = False
            else:
                result.append(_LITERAL_ESCAPES.get(char, char))
        else:
            result.append(char)
            if char == '"':
                quoted = True
            elif char in "[{":
                depth += 1
                if depth > MAX_DEPTH:
                    raise _Rejected("too_deep")
            elif char in "]}":
                depth -= 1
                if depth < 0:
                    raise _Rejected("invalid_json")
    if quoted or escaped or depth:
        raise _Rejected("invalid_json")
    return "".join(result)


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise _Rejected("duplicate_key")
        result[key] = value
    return result


def _nonfinite(_value):
    raise _Rejected("invalid_number")


def _finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise _Rejected("invalid_number")
    return result


def _check_strings(value):
    # Iterative traversal also checks escaped controls and surrogate code
    # points after JSON decoding, including keys and nested string values.
    pending = [value]
    while pending:
        current = pending.pop()
        if isinstance(current, str):
            if any(_bad_control(char) for char in current):
                raise _Rejected("invalid_control")
            try:
                current.encode("utf-8")
            except UnicodeError:
                raise _Rejected("invalid_unicode") from None
        elif isinstance(current, dict):
            pending.extend(current.keys())
            pending.extend(current.values())
        elif isinstance(current, list):
            pending.extend(current)


def parse_object(content: str) -> dict:
    """Return exactly one complete object, or raise ValueError with a safe code.

    The byte limit includes fences/BOM/whitespace. Depth counts containers,
    including the root object. LF/CR/TAB retain their decoded string meaning;
    other C0, DEL and C1 controls are rejected whether literal or escaped.
    """
    if not isinstance(content, str):
        raise ValueError("invalid_type")
    if len(content) > MAX_CONTENT_BYTES:
        raise ValueError("too_large")
    try:
        if len(content.encode("utf-8")) > MAX_CONTENT_BYTES:
            raise _Rejected("too_large")
        if any(_bad_control(char) for char in content):
            raise _Rejected("invalid_control")
        text = _trim_packaging(content)
        if text.startswith("```"):
            match = _FENCE.fullmatch(text)
            if match is None:
                raise _Rejected("invalid_json")
            text = _trim_packaging(match.group("body"))
        if not text:
            raise _Rejected("invalid_json")
        text = _normalize_strings(text)
        decoder = json.JSONDecoder(object_pairs_hook=_object,
            parse_constant=_nonfinite, parse_float=_finite_float)
        result, end = decoder.raw_decode(text)
        if text[end:].strip(_SPACE):
            raise _Rejected("ambiguous_json")
        if not isinstance(result, dict):
            raise _Rejected("invalid_json")
        _check_strings(result)
        return result
    except _Rejected:
        raise
    except UnicodeError:
        raise ValueError("invalid_unicode") from None
    except RecursionError:
        raise ValueError("too_deep") from None
    except (ValueError, OverflowError):
        raise ValueError("invalid_json") from None
