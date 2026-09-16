"""Explicit, dedicated server configuration. Never discover other tools' keys."""
from dataclasses import dataclass
import os
from urllib.parse import urlsplit


@dataclass(frozen=True)
class OnlineSettings:
    api_key: str
    base_url: str
    model: str
    provider: str
    timeout: float = 8.0


def online_settings():
    """HTTPS endpoint; operator opt-in is separate from per-question consent."""
    if os.environ.get("ML_ADVISOR_ONLINE_ENABLED") != "1":
        return None
    values = [os.environ.get("ML_ADVISOR_" + name, "").strip()
              for name in ("API_KEY", "BASE_URL", "MODEL", "PROVIDER_LABEL")]
    key, base, model, provider = values
    try:
        url = urlsplit(base)
        valid = (all(values) and url.scheme == "https" and url.hostname
                 and not url.username and not url.password and not url.query and not url.fragment
                 and len(provider) <= 80 and len(model) <= 120
                 and all("\n" not in value and "\r" not in value for value in values))
        if not valid:
            return None
        return OnlineSettings(key, base.rstrip("/"), model, provider)
    except ValueError:
        return None
