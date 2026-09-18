"""Public errors never include SQL, local paths or exception internals."""

from dataclasses import dataclass
from typing import Any


@dataclass
class ApiError(Exception):
    status: int
    code: str
    message: str
    data: Any = None
