"""Conservative Spark expressions: normalize known presentation, never infer facts."""

RULE_VERSION = "cleaning-rules-1.0.0"
RULES = [
    {"id": "N001", "action": "TRIM_UNICODE_BOUNDARIES", "scope": "all text fields",
     "description": "Strip boundary whitespace, Unicode separators, BOM and zero-width space; preserve interior prose."},
    {"id": "N002", "action": "REMOVE_TOKEN_ZERO_WIDTH", "scope": "IDs, enums and typed numeric/date/time tokens",
     "description": "Remove U+200B/U+FEFF inside structured tokens only; do not rewrite free-text words."},
    {"id": "N003", "action": "ASCII_NUMERIC_PRESENTATION", "scope": "numeric/date/time fields, NOT IDs or free text",
     "description": "Translate full-width digits and numeric +/−/decimal punctuation; never infer units or remove currency symbols."},
    {"id": "N004", "action": "UPPERCASE_ENUM", "scope": "declared enum columns",
     "description": "Canonical uppercase only; unknown business enum values are not guessed."},
    {"id": "N005", "action": "EMPTY_TO_NULL", "scope": "empty normalized strings",
     "description": "Represent blanks as null; never fill a key, amount, energy or timestamp."},
    {"id": "V001", "action": "QUARANTINE_OR_FAIL", "scope": "explicit CSV types",
     "description": "Reject non-finite numbers, decimal integer amounts, overflow and ambiguous datetime formats; timestamps require an explicit timezone."},
    {"id": "V002", "action": "QUARANTINE_OR_FAIL", "scope": "all text fields",
     "description": "Reject Unicode replacement characters and prohibited controls; cannot reconstruct the original bytes or detect all mojibake."},
    {"id": "V003", "action": "QUARANTINE_SESSION", "scope": "charging sessions",
     "description": "Existing required-field, nonnegative-money/energy, SOC/time, fee identity, status, FK and ownership checks; no outlier clipping."},
    {"id": "D001", "action": "DETERMINISTIC_SESSION_DEDUP", "scope": "valid charging sessions",
     "description": "Validate first; keep canonical smallest normalized JSON then raw JSON for each session ID; retain discarded copies with reason."},
    {"id": "V004", "action": "FAIL_BATCH", "scope": "charger telemetry",
     "description": "Existing energy/power/state/interval, station ownership and charger timestamp uniqueness checks."},
    {"id": "A001", "action": "ASSERT_ROW_CONSERVATION", "scope": "all tables",
     "description": "Input equals retained plus quarantined plus duplicate copies; normalized rows are not an additional deletion category."},
]

BOUNDARY_PATTERN = r"^[\s\p{Z}\u200B\uFEFF]+|[\s\p{Z}\u200B\uFEFF]+$"
BAD_TEXT_PATTERN = r"[\x00-\x08\x0B\x0C\x0E-\x1F\x7F\uFFFD]"


def normalize_value(source, field, kind, is_enum=False):
    from pyspark.sql import functions as F, types as T
    trimmed = F.regexp_replace(source, BOUNDARY_PATTERN, "")
    typed_token = not isinstance(kind, T.StringType)
    token = typed_token or is_enum or field.endswith("_id")
    invisible = F.regexp_replace(trimmed, r"[\u200B\uFEFF]", "") if token else trimmed
    numeric = isinstance(kind, (T.LongType, T.DoubleType))
    ascii_value = (F.translate(invisible, "０１２３４５６７８９＋－．", "0123456789+-.") if numeric else
                   F.translate(invisible, "０１２３４５６７８９", "0123456789") if typed_token else invisible)
    upper = F.upper(ascii_value) if is_enum else ascii_value
    value = F.when(upper == "", F.lit(None).cast("string")).otherwise(upper)
    return value, {
        "N001": ~source.eqNullSafe(trimmed),
        "N002": ~trimmed.eqNullSafe(invisible) if token else F.lit(False),
        "N003": ~invisible.eqNullSafe(ascii_value) if typed_token else F.lit(False),
        "N004": ~ascii_value.eqNullSafe(upper) if is_enum else F.lit(False),
        "N005": ~upper.eqNullSafe(value),
    }


def invalid_type(value, kind):
    """Missingness is measured separately; a successful permissive cast is insufficient."""
    from pyspark.sql import functions as F, types as T
    typed = value.cast(kind)
    bad = typed.isNull()
    if isinstance(kind, T.DoubleType):
        bad = bad | F.isnan(typed) | (F.abs(typed) == F.lit(float("inf")))
    elif isinstance(kind, T.LongType):
        bad = bad | ~value.rlike(r"^[+-]?[0-9]+$")
    elif isinstance(kind, T.TimestampType):
        bad = bad | ~value.rlike(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,9})?(Z|[+-][0-9]{2}:[0-9]{2})$")
    elif isinstance(kind, T.DateType):
        bad = bad | ~value.rlike(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
    return F.coalesce(value.isNotNull() & (value != "") & bad, F.lit(False))


def named_flags(flags):
    """A bounded array of matching field/rule names, empty rather than null."""
    from pyspark.sql import functions as F
    return F.filter(F.array(*[F.when(condition, F.lit(name)) for name, condition in flags.items()]),
                    lambda item: item.isNotNull())
