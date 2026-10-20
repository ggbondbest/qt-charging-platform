"""Literal local service configuration shared by Python and native launchers.

This module only reads the requested JSON file. It never mutates the process
environment, imports an API client, discovers unrelated credentials, or logs
configuration values.
"""
from collections.abc import Mapping
import json
import os
from pathlib import Path


DEFAULT_CONFIG_FILE = Path(__file__).resolve().parents[1] / "config" / "services.local.json"
CONFIG_ENV = "CHARGING_SERVICES_CONFIG"
MAX_CONFIG_BYTES = 32 * 1024
_MISSING = object()
AI_DEFAULTS = {
    "baseUrl": "https://aiping.cn/api/v1",
    "model": "DeepSeek-V4.1-Flash",
    "providerLabel": "AIPing",
}
MAP_FIELDS = {
    "webServiceKey": "TENCENT_MAP_API_KEY",
    "jsKey": "TENCENT_MAP_JS_KEY",
    "secretKey": "TENCENT_MAP_SECRET_KEY",
}


class ConfigError(RuntimeError):
    """An operator-facing failure containing field names, never input values."""


def _error(field, reason):
    # Callers supply only constant schema field names and controlled reasons.
    return ConfigError(f"服务配置 {field} {reason}")


def _unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise _error("JSON", "含重复字段，请保留每个字段的一份定义")
        result[name] = value
    return result


def _invalid_constant(_value):
    raise _error("JSON", "必须使用标准 JSON 数值")


def _object(value, fields, name):
    if not isinstance(value, dict) or set(value) - fields:
        raise _error(name, "须为仅包含规定字段的对象")
    return value


def _string(value, name, limit=512):
    if not isinstance(value, str):
        raise _error(name, "须为字符串")
    value = value.strip()
    try:
        value.encode("utf-8")
    except UnicodeError:
        raise _error(name, "包含无效字符") from None
    if len(value) > limit or any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise _error(name, "须为长度合理的单行文本")
    return value


def _read_config(config_file, environ):
    explicit = config_file is not None or CONFIG_ENV in environ
    raw_path = config_file if config_file is not None else environ.get(CONFIG_ENV, DEFAULT_CONFIG_FILE)
    try:
        if not isinstance(raw_path, (str, os.PathLike)) or not str(raw_path).strip():
            raise _error(CONFIG_ENV, "须指定配置文件路径")
        path = Path(raw_path).expanduser()
        with path.open("rb") as source:
            raw = source.read(MAX_CONFIG_BYTES + 1)
    except FileNotFoundError:
        if not explicit:
            return _MISSING
        raise _error(CONFIG_ENV, "指定的文件不存在，请检查路径") from None
    except (OSError, ValueError, RuntimeError) as exc:
        if isinstance(exc, ConfigError):
            raise
        raise _error(CONFIG_ENV, "文件无法读取，请检查路径和权限") from None
    if len(raw) > MAX_CONFIG_BYTES:
        raise _error("JSON", "文件大小不能超过 32 KiB")
    try:
        return json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_object,
                          parse_constant=_invalid_constant)
    except (UnicodeError, ValueError, RecursionError):
        raise _error("JSON", "格式无效，请检查 UTF-8 编码及 JSON 语法") from None


def _configuration(config_file, environ):
    environ = os.environ if environ is None else environ
    config = _read_config(config_file, environ)
    if config is _MISSING:
        return {}
    _object(config, {"version", "ai", "tencentMap"}, "根节点")
    if type(config.get("version")) is not int or config["version"] != 1:
        raise _error("version", "须为整数 1")
    return config


def _ai_environment(config):
    result = {}
    if "ai" in config:
        ai = _object(config["ai"], {"enabled", "apiKey", *AI_DEFAULTS}, "ai")
        enabled = ai.get("enabled", "auto")
        if isinstance(enabled, str):
            enabled = enabled.strip()
        if type(enabled) is not bool and enabled != "auto":
            raise _error("ai.enabled", "须为 auto、true 或 false")
        values = {name: _string(ai.get(name, ""), "ai." + name, limit) for name, limit in
                  (("apiKey", 512), ("baseUrl", 2048), ("model", 120), ("providerLabel", 80))}
        if enabled is False:
            result["ML_ADVISOR_ONLINE_ENABLED"] = "0"
        elif enabled is True or values["apiKey"]:
            result.update({
                "ML_ADVISOR_ONLINE_ENABLED": "1",
                "ML_ADVISOR_API_KEY": values["apiKey"],
                "ML_ADVISOR_BASE_URL": values["baseUrl"] or AI_DEFAULTS["baseUrl"],
                "ML_ADVISOR_MODEL": values["model"] or AI_DEFAULTS["model"],
                "ML_ADVISOR_PROVIDER_LABEL": values["providerLabel"] or AI_DEFAULTS["providerLabel"],
            })
    return result


def _map_environment(config):
    result = {}
    if "tencentMap" in config:
        maps = _object(config["tencentMap"], set(MAP_FIELDS), "tencentMap")
        for field, name in MAP_FIELDS.items():
            value = _string(maps.get(field, ""), "tencentMap." + field)
            if value:
                result[name] = value
    return result


def load_ai_environment(config_file=None, *, environ=None):
    """Read only server AI settings; malformed client map fields are ignored.

    JSON syntax, duplicate names, the root schema and version remain shared
    requirements. No client map values are validated or returned by this API.
    """
    return _ai_environment(_configuration(config_file, environ))


def load_map_environment(config_file=None, *, environ=None):
    """Read only client map settings, independent of AI field validity."""
    return _map_environment(_configuration(config_file, environ))


def load_service_environment(config_file=None, *, environ=None):
    """Compatibility reader validating both groups and returning their union.

    Runtime consumers should use the AI or map reader instead, so errors in an
    unused section do not prevent their own service from starting. This reader
    retains the existing strict behavior for callers validating the whole file.
    """
    config = _configuration(config_file, environ)
    return {**_ai_environment(config), **_map_environment(config)}


def client_map_environment(config_file=None, *, environ=None, service_environment=None):
    """Return only Tencent values needed by the existing Qt clients.

    Nonempty modern environment names precede their legacy aliases, followed
    by the local file. The legacy API alias retains the WebService key; both
    native map views consume the separate JS key and fall back to the API key.
    A preloaded service mapping lets launchers read and validate JSON once.
    """
    environ = os.environ if environ is None else environ
    services = (load_map_environment(config_file, environ=environ)
                if service_environment is None else service_environment)
    if not isinstance(services, Mapping):
        raise _error("腾讯地图映射", "须为字段映射")

    def value(source, name):
        entry = source.get(name, "")
        return entry.strip() if isinstance(entry, str) else ""

    api_key = (value(environ, "TENCENT_MAP_API_KEY") or value(environ, "CHARGING_TENCENT_MAP_KEY")
               or value(services, "TENCENT_MAP_API_KEY"))
    js_key = value(environ, "TENCENT_MAP_JS_KEY") or value(services, "TENCENT_MAP_JS_KEY") or api_key
    secret = (value(environ, "TENCENT_MAP_SECRET_KEY") or value(environ, "CHARGING_TENCENT_MAP_SECRET")
              or value(services, "TENCENT_MAP_SECRET_KEY"))
    return {
        "TENCENT_MAP_API_KEY": api_key,
        "TENCENT_MAP_JS_KEY": js_key,
        "TENCENT_MAP_SECRET_KEY": secret,
        "CHARGING_TENCENT_MAP_KEY": api_key,
        "CHARGING_TENCENT_MAP_SECRET": secret,
    }
