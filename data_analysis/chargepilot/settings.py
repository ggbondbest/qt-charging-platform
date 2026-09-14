"""Explicit environment configuration; no credentials or production fallbacks."""
from dataclasses import dataclass, field
from pathlib import Path
import os
import re

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Settings:
    model_dir: Path = ROOT / "outputs" / "chargepilot"
    load_dir: Path = ROOT / "outputs" / "ml_load"
    mysql: dict = field(default_factory=dict)
    admin_token: str = ""
    tencent_key: str = ""
    tencent_secret: str = ""
    allowed_origins: tuple[str, ...] = ("http://localhost:5173", "http://127.0.0.1:5173")

    @classmethod
    def from_env(cls):
        database = os.getenv("CHARGEPILOT_MYSQL_DATABASE", "chargepilot_demo")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", database):
            raise ValueError("Invalid CHARGEPILOT_MYSQL_DATABASE")
        user = os.getenv("CHARGEPILOT_MYSQL_USER", "")
        password = os.getenv("CHARGEPILOT_MYSQL_PASSWORD", "")
        if not user or not password:
            raise ValueError("Set CHARGEPILOT_MYSQL_USER and CHARGEPILOT_MYSQL_PASSWORD; no default credentials")
        token = os.getenv("CHARGEPILOT_ADMIN_TOKEN", "")
        if token and len(token) < 16:
            raise ValueError("CHARGEPILOT_ADMIN_TOKEN must contain at least 16 characters")
        return cls(
            model_dir=Path(os.getenv("CHARGEPILOT_MODEL_DIR", str(ROOT / "outputs" / "chargepilot"))),
            load_dir=Path(os.getenv("CHARGEPILOT_LOAD_DIR", str(ROOT / "outputs" / "ml_load"))),
            mysql={"host": os.getenv("CHARGEPILOT_MYSQL_HOST", "127.0.0.1"),
                   "port": int(os.getenv("CHARGEPILOT_MYSQL_PORT", "3306")),
                   "user": user, "password": password, "database": database},
            admin_token=token,
            tencent_key=os.getenv("TENCENT_MAP_API_KEY", ""),
            tencent_secret=os.getenv("TENCENT_MAP_SECRET_KEY", ""),
            allowed_origins=tuple(filter(None, os.getenv("CHARGEPILOT_CORS_ORIGINS",
                "http://localhost:5173,http://127.0.0.1:5173").split(","))),
        )
