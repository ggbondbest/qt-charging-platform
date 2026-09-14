"""Shared MySQL 8.4 configuration; credentials never belong in URLs or reports."""

from dataclasses import dataclass, field
import os
import re


@dataclass(frozen=True)
class MySQLSettings:
    host: str = "127.0.0.1"
    port: int = 3306
    user: str = ""
    password: str = field(default="", repr=False)
    database: str = ""
    connect_timeout: int = 5
    read_timeout: int = 60
    write_timeout: int = 60
    ssl_ca: str | None = None

    def __post_init__(self):
        if (not isinstance(self.host, str) or not self.host or len(self.host) > 253
                or any(c.isspace() for c in self.host) or any(c in self.host for c in ("/", "\\", "@"))):
            raise ValueError("MySQL host must be a hostname or IP address")
        if type(self.port) is not int or not 1 <= self.port <= 65535:
            raise ValueError("MySQL port must be between 1 and 65535")
        if not isinstance(self.user, str) or not self.user or len(self.user) > 32:
            raise ValueError("MySQL user must be explicitly configured")
        if not isinstance(self.password, str):
            raise ValueError("MySQL password must be a string")
        if (not isinstance(self.database, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", self.database)
                or self.database in {"mysql", "sys", "information_schema", "performance_schema"}):
            raise ValueError("Use a non-system MySQL database name: lowercase letters, digits and underscores")
        for name in ("connect_timeout", "read_timeout", "write_timeout"):
            value = getattr(self, name)
            if type(value) is not int or not 1 <= value <= 300:
                raise ValueError("MySQL timeouts must be between 1 and 300 seconds")

    @classmethod
    def from_env(cls, prefix="ANALYTICS_MYSQL_"):
        try:
            port = int(os.environ.get(prefix + "PORT", "3306"))
        except ValueError:
            raise ValueError("MySQL port must be an integer") from None
        return cls(host=os.environ.get(prefix + "HOST", "127.0.0.1"), port=port,
            user=os.environ.get(prefix + "USER", ""), password=os.environ.get(prefix + "PASSWORD", ""),
            database=os.environ.get(prefix + "DATABASE", ""), ssl_ca=os.environ.get(prefix + "SSL_CA") or None)


def connect(settings, *, database=None, dict_rows=False):
    """Open one bounded connection; empty database means administrative preflight.

    PyMySQL is lazy-imported so generator/Spark-only tasks need no MySQL driver.
    The caller owns transactions and close(). Remote deployments should configure
    SSL_CA and network access controls; the default address is loopback.
    """
    import pymysql

    options = dict(host=settings.host, port=settings.port, user=settings.user, password=settings.password,
        database=(settings.database if database is None else database) or None,
        charset="utf8mb4", collation="utf8mb4_0900_bin", autocommit=False,
        connect_timeout=settings.connect_timeout, read_timeout=settings.read_timeout,
        write_timeout=settings.write_timeout, local_infile=False,
        sql_mode="STRICT_ALL_TABLES,ONLY_FULL_GROUP_BY,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION",
        init_command="SET time_zone = '+00:00'")
    if dict_rows:
        options["cursorclass"] = pymysql.cursors.DictCursor
    if settings.ssl_ca:
        options.update(ssl_ca=settings.ssl_ca, ssl_verify_cert=True, ssl_verify_identity=True)
    connection = pymysql.connect(**options)
    try:
        with connection.cursor() as cursor:
            cursor.execute("SELECT VERSION()")
            row = cursor.fetchone()
            version = next(iter(row.values())) if isinstance(row, dict) else row[0]
        match = re.match(r"(\d+)\.(\d+)\.", version)
        if not match or tuple(map(int, match.groups())) < (8, 4) or "mariadb" in version.lower():
            raise ValueError("This analytics deployment requires MySQL 8.4 or newer; MariaDB is not validated")
        connection.rollback()
        return connection
    except Exception:
        connection.close()
        raise
