"""Explicit, dedicated server configuration. Never discover other tools' keys."""
from dataclasses import dataclass
import os
from pathlib import Path
from urllib.parse import urlsplit


@dataclass(frozen=True)
class OnlineSettings:
    api_key: str
    base_url: str
    model: str
    provider: str
    timeout: float = 45.0


ENV_NAMES = {"ML_ADVISOR_" + suffix for suffix in
             ("ONLINE_ENABLED", "API_KEY", "BASE_URL", "MODEL", "PROVIDER_LABEL")}


def local_settings():
    """Read only the advisor's dedicated git-ignored file, with literal values.

    No shell expansion/execution. Explicit process environment takes precedence.
    """
    path = Path(os.getenv("ML_ADVISOR_ENV_FILE", str(Path(__file__).with_name(".env.local"))))
    try:
        if not path.is_file() or path.stat().st_size > 8192:
            return {}
        result = {}
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, separator, value = line.partition("=")
            name, value = name.strip(), value.strip()
            if not separator or name not in ENV_NAMES or name in result:
                return {}
            if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
                value = value[1:-1]
            result[name] = value
        return result
    except (OSError, UnicodeError):
        return {}


def online_settings():
    """HTTPS endpoint; operator opt-in is separate from per-question consent."""
    values_by_name = local_settings()
    values_by_name.update({name: os.environ[name] for name in ENV_NAMES if name in os.environ})
    if values_by_name.get("ML_ADVISOR_ONLINE_ENABLED") != "1":
        return None
    values = [values_by_name.get("ML_ADVISOR_" + name, "").strip()
              for name in ("API_KEY", "BASE_URL", "MODEL", "PROVIDER_LABEL")]
    key, base, model, provider = values
    try:
        url = urlsplit(base)
        valid = (all(values) and url.scheme == "https" and url.hostname
                 and (url.port is None or 1 <= url.port <= 65535)
                 and not url.username and not url.password and not url.query and not url.fragment
                 and len(provider) <= 80 and len(model) <= 120
                 and len(key) <= 512 and all(33 <= ord(char) <= 126 for char in key)
                 and not url.path.rstrip("/").endswith(("/chat/completions", "/responses"))
                 and all("\n" not in value and "\r" not in value for value in values))
        if not valid:
            return None
        return OnlineSettings(key, base.rstrip("/"), model, provider)
    except ValueError:
        return None
