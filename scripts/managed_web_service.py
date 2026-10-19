#!/usr/bin/env python3
"""Qt-owned, loopback-only web service. Never initializes or seeds a database.

Qt reads CHARGING_WEB_READY on stdout and sends CHARGING_WEB_STOP on stdin.
Configuration is local JSON: mode, environmentFile, and optional mysql with
executable/baseDir/dataDir/port/socket/runtimeDir. Relative configuration paths
are relative to that JSON file. pythonExecutable is consumed by the Qt launcher.
"""
from __future__ import annotations

import argparse
import asyncio
from contextlib import contextmanager, redirect_stdout
from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
READY_PREFIX = "CHARGING_WEB_READY="
ERROR_PREFIX = "CHARGING_WEB_ERROR="


class ServiceError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code, self.message = code, message


def config_error():
    return ServiceError("INVALID_CONFIG", "网页服务配置无效，请检查本机 JSON 配置及文件权限")


def read_object(path):
    try:
        if path.stat().st_size > 1024 * 1024:
            raise config_error()
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError):
        raise config_error() from None
    if not isinstance(value, dict):
        raise config_error()
    return value


def config_path(value, base):
    if not isinstance(value, str) or not value.strip() or "\0" in value:
        raise config_error()
    path = Path(value).expanduser()
    return (path if path.is_absolute() else base / path).resolve()


@dataclass(frozen=True)
class MysqlConfig:
    executable: Path
    base_dir: Path
    data_dir: Path
    port: int
    socket_path: Path | None = None
    runtime_dir: Path | None = None


@dataclass(frozen=True)
class ServiceConfig:
    database: Path
    mode: str
    environment: dict[str, str]
    mysql: MysqlConfig | None = None


def load_config(database, config_file=None):
    database = Path(database).expanduser()
    if not database.is_absolute() or not database.is_file():
        raise ServiceError("QT_DATABASE_UNAVAILABLE", "Qt 业务数据库不存在或路径不是绝对路径")
    database = database.resolve()
    base = Path(config_file).expanduser().resolve().parent if config_file else ROOT
    config = read_object(Path(config_file).expanduser()) if config_file else {}
    if set(config) - {"mode", "environmentFile", "mysql", "pythonExecutable"}:
        raise config_error()
    mode = config.get("mode", "qt-live-only")
    if mode not in ("qt-live-only", "full"):
        raise config_error()
    environment = {}
    if "environmentFile" in config:
        environment = read_object(config_path(config["environmentFile"], base))
        if any(not isinstance(key, str) or not key or "=" in key or "\0" in key
               or not isinstance(value, str) or "\0" in value
               for key, value in environment.items()):
            raise config_error()
    mysql = None
    if "mysql" in config:
        entry = config["mysql"]
        if mode != "full" or not isinstance(entry, dict):
            raise config_error()
        if set(entry) - {"executable", "baseDir", "dataDir", "port", "socket", "runtimeDir"}:
            raise config_error()
        if not {"executable", "baseDir", "dataDir", "port"} <= entry.keys():
            raise config_error()
        port = entry["port"]
        if type(port) is not int or not 1 <= port <= 65535:
            raise config_error()
        mysql = MysqlConfig(config_path(entry["executable"], base),
                            config_path(entry["baseDir"], base),
                            config_path(entry["dataDir"], base), port,
                            config_path(entry["socket"], base) if "socket" in entry else None,
                            config_path(entry["runtimeDir"], base) if "runtimeDir" in entry else None)
        # Recognize an existing MySQL datadir; never create one or initialize it.
        if not mysql.executable.is_file() or not mysql.base_dir.is_dir() or not (
            (mysql.data_dir / "auto.cnf").is_file() and
            ((mysql.data_dir / "mysql.ibd").is_file() or (mysql.data_dir / "mysql").is_dir())
        ):
            raise ServiceError("MYSQL_NOT_PREPARED", "MySQL 程序或已有数据目录不可用，请先完成历史数据准备")
    return ServiceConfig(database, mode, environment, mysql)


def tcp_ready(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        return False


class OwnedMysql:
    """Own only the Popen handle we created; never act on a pid-file PID."""
    def __init__(self, config):
        self.config = config
        self.process = None
        self.log = None
        self.socket_path = None

    async def start(self, stop):
        if self.config is None or tcp_ready(self.config.port):
            return
        if stop.is_set():
            return
        cfg = self.config
        try:
            runtime_base = cfg.runtime_dir or cfg.data_dir / ".charging-managed"
            runtime_base.mkdir(parents=True, exist_ok=True)
            runtime = Path(tempfile.mkdtemp(prefix="web-", dir=runtime_base))
            args = [str(cfg.executable), "--no-defaults", f"--basedir={cfg.base_dir}",
                    f"--datadir={cfg.data_dir}", "--bind-address=127.0.0.1", f"--port={cfg.port}",
                    f"--pid-file={runtime / 'mysql.pid'}", f"--log-error={runtime / 'mysql.log'}", "--mysqlx=OFF"]
            if os.name != "nt":
                self.socket_path = cfg.socket_path or Path(tempfile.gettempdir()) / (
                    "charging-web-" + uuid.uuid4().hex[:12] + ".sock")
                # Never replace another instance's Unix socket or lock file.
                if self.socket_path.exists() or Path(str(self.socket_path) + ".lock").exists():
                    raise ServiceError("MYSQL_SOCKET_IN_USE", "MySQL 本机套接字已存在，请检查服务状态或更换配置")
                args.append(f"--socket={self.socket_path}")
            self.log = (runtime / "process.log").open("ab", buffering=0)
            self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=self.log,
                                            stderr=self.log, cwd=cfg.data_dir)
        except ServiceError:
            raise
        except OSError:
            raise ServiceError("MYSQL_START_FAILED", "无法启动配置的 MySQL 实例，请检查目录权限和本机配置") from None
        deadline = time.monotonic() + 45
        while not stop.is_set():
            if self.process.poll() is not None:
                raise ServiceError("MYSQL_START_FAILED", "配置的 MySQL 实例启动失败，请检查本机 MySQL 日志")
            if tcp_ready(cfg.port):
                return
            if time.monotonic() >= deadline:
                raise ServiceError("MYSQL_START_TIMEOUT", "等待 MySQL 就绪超时，请检查本机 MySQL 日志")
            await asyncio.sleep(0.1)

    async def close(self):
        if self.process is not None and self.process.poll() is None:
            # Windows terminate() is abrupt. mysqld handles its shutdown event;
            # mysqladmin is unnecessary and would put credentials in another process.
            if os.name == "nt":
                _signal_windows_mysql(self.process.pid)
            else:
                self.process.terminate()
            deadline = time.monotonic() + 9
            while self.process.poll() is None and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            if self.process.poll() is None:
                self.process.kill()
            await asyncio.to_thread(self.process.wait, 2)
        if self.log is not None:
            self.log.close()


def _signal_windows_mysql(pid):
    """MySQL's documented Windows shutdown event, falling back to owned kill."""
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_bool, ctypes.c_wchar_p]
    kernel.OpenEventW.restype = ctypes.c_void_p
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    # mysql/mysql-server 8.4 sql/mysqld.cc: shutdown_event_name.
    handle = kernel.OpenEventW(0x0002, False, f"MYSQLShutdown{pid}")
    if handle:
        try:
            kernel.SetEvent(handle)
        finally:
            kernel.CloseHandle(handle)


class ParentWatch:
    def __init__(self, pid):
        self.pid = pid
        self.direct_parent = pid == os.getppid()
        self.handle = None
        self.kernel = None
        if os.name == "nt":
            import ctypes
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_bool, ctypes.c_uint32]
            self.kernel.OpenProcess.restype = ctypes.c_void_p
            self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            self.handle = self.kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only

    def alive(self):
        if os.name == "nt":
            return bool(self.handle) and self.kernel.WaitForSingleObject(self.handle, 0) == 0x102
        if self.direct_parent and os.getppid() != self.pid:
            return False
        try:
            os.kill(self.pid, 0)
            return True
        except PermissionError:
            return True
        except ProcessLookupError:
            return False

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def watch_stdin(loop, stop):
    """A raw daemon reader avoids buffered-stdin locks during interpreter exit."""
    def request_stop():
        try:
            loop.call_soon_threadsafe(stop.set)
        except RuntimeError:
            pass  # loop has already completed shutdown

    def read():
        pending = b""
        try:
            descriptor = sys.stdin.fileno()
            while True:
                chunk = os.read(descriptor, 4096)
                if not chunk:
                    request_stop()
                    return
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    if line.strip() == b"CHARGING_WEB_STOP":
                        request_stop()
                        return
                if len(pending) > 4096:
                    pending = b""
        except (OSError, ValueError):
            request_stop()
    threading.Thread(target=read, name="qt-web-control", daemon=True).start()


def create_application(config):
    from data_analysis.delivery.app import create_app, create_qt_live_app
    from data_analysis.delivery.qt_live_adapter import QtLiveAdapter, QtLiveError
    try:
        QtLiveAdapter(config.database).health()
    except QtLiveError as exc:
        raise ServiceError(exc.code, exc.message) from None
    if not (ROOT / "data_analysis/frontend/dist/index.html").is_file():
        raise ServiceError("FRONTEND_NOT_BUILT", "Vue 页面尚未构建，请先在前端目录运行 npm ci 和 npm run build")
    if config.mode == "full":
        from data_analysis.chargepilot.store import DomainError
        try:
            app = create_app(qt_database_path=config.database, initialize_operational_store=False)
        except DomainError as exc:
            raise ServiceError(exc.code, exc.message) from None
        try:
            validate_full_application(app)
        except Exception:
            app.state.advisor_runner.close()
            raise
        return app
    return create_qt_live_app(qt_database_path=config.database)


def validate_full_application(app):
    from data_analysis.backend.database import open_snapshot
    from data_analysis.backend.errors import ApiError
    try:
        with open_snapshot(app.state.database_path, mysql_settings=app.state.mysql_settings) as snapshot:
            registry = app.state.models.registry(snapshot.metadata)
    except ApiError as exc:
        raise ServiceError(exc.code, exc.message) from None
    except Exception:
        raise ServiceError("HISTORICAL_DATA_NOT_READY", "历史统计批次不可用，请先检查已有 MySQL 数据及模型批次") from None
    required = ("load", "availability", "insights", "arrival")
    if any(registry.get(name, {}).get("status") != "READY" for name in required):
        raise ServiceError("MODELS_NOT_READY", "完整模式要求四类模型均已准备完成，请先运行独立训练与校验流程")


async def run_service(config, parent_pid, protocol_stdout):
    import uvicorn

    class ManagedServer(uvicorn.Server):
        @contextmanager
        def capture_signals(self):
            # Our owner must finish MySQL cleanup before a signal exits Python.
            yield

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    original_handlers = {}
    for number in (signal.SIGTERM, signal.SIGINT):
        original_handlers[number] = signal.signal(number, lambda *_: loop.call_soon_threadsafe(stop.set))
    parent = ParentWatch(parent_pid) if parent_pid else None
    mysql = OwnedMysql(config.mysql)
    listener = None
    server = None
    serving = None
    watcher = None
    building = None
    dependency_ready = False

    def stopping():
        if mysql.process is not None and mysql.process.poll() is not None:
            raise ServiceError("MYSQL_EXITED", "托管 MySQL 已退出，网页服务随之停止，请检查本机日志")
        return stop.is_set()

    async def monitor():
        while not stop.is_set():
            if parent and not parent.alive():
                stop.set()
            if dependency_ready and mysql.process is not None and mysql.process.poll() is not None:
                stop.set()
            await asyncio.sleep(0.2)

    if parent:
        watch_stdin(loop, stop)
    watcher = asyncio.create_task(monitor())
    try:
        await mysql.start(stop)
        dependency_ready = True
        if stopping():
            return
        building = asyncio.create_task(asyncio.to_thread(create_application, config))
        while not building.done() and not stop.is_set():
            await asyncio.sleep(0.05)
        if stopping():
            return
        app = await building
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        listener.setblocking(False)
        port = listener.getsockname()[1]
        server = ManagedServer(uvicorn.Config(app, host="127.0.0.1", port=port,
                               access_log=False, log_level="critical", timeout_graceful_shutdown=3))
        serving = asyncio.create_task(server.serve(sockets=[listener]))
        while not server.started and not serving.done() and not stop.is_set():
            await asyncio.sleep(0.02)
        if stopping():
            return
        if not server.started:
            await serving
            raise ServiceError("API_START_FAILED", "网页接口启动失败，请检查历史服务和模型准备状态")
        print(READY_PREFIX + json.dumps({"url": f"http://127.0.0.1:{port}/?embedded=operator",
              "database": str(config.database), "mode": config.mode}, ensure_ascii=False),
              file=protocol_stdout, flush=True)
        while not stop.is_set() and not serving.done():
            await asyncio.sleep(0.1)
        if mysql.process is not None and mysql.process.poll() is not None:
            raise ServiceError("MYSQL_EXITED", "托管 MySQL 已退出，网页服务随之停止，请检查本机日志")
        if serving.done():
            await serving
    finally:
        stop.set()
        if server:
            server.should_exit = True
        if serving:
            try:
                await asyncio.wait_for(asyncio.shield(asyncio.gather(serving, return_exceptions=True)), timeout=5)
            except asyncio.TimeoutError:
                serving.cancel()
                await asyncio.gather(serving, return_exceptions=True)
        if listener:
            listener.close()
        if watcher:
            watcher.cancel()
            await asyncio.gather(watcher, return_exceptions=True)
        if building and not building.done():
            building.cancel()
            await asyncio.gather(building, return_exceptions=True)
        await mysql.close()
        if parent:
            parent.close()
        for number, handler in original_handlers.items():
            signal.signal(number, handler)


def positive_pid(value):
    try:
        number = int(value)
        if not 1 <= number <= 0xFFFFFFFF:
            raise ValueError
        return number
    except ValueError:
        raise argparse.ArgumentTypeError("parent PID must be a positive integer") from None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--parent-pid", type=positive_pid)
    args = parser.parse_args(argv)
    protocol_stdout = sys.stdout
    try:
        config = load_config(args.database, args.config)
        os.environ.update(config.environment)
        os.environ["QT_SQLITE_PATH"] = str(config.database)
        for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
            os.environ.setdefault(name, "2")
        sys.path.insert(0, str(ROOT))
        from data_analysis.runtime_config import ConfigError, load_ai_environment
        try:
            # The manager consumes AI only; client map errors must not prevent
            # its API from starting or inject client credentials into the server.
            load_ai_environment()
        except ConfigError as exc:
            raise ServiceError("INVALID_SERVICES_CONFIG", str(exc)) from None
        # Libraries and application startup must not corrupt Qt's stdout protocol.
        with redirect_stdout(sys.stderr):
            asyncio.run(run_service(config, args.parent_pid, protocol_stdout))
        return 0
    except ServiceError as exc:
        error = {"code": exc.code, "message": exc.message}
    except (ImportError, ModuleNotFoundError):
        error = {"code": "DEPENDENCIES_MISSING", "message": "Python 网页服务依赖不完整，请检查虚拟环境和 requirements 文件"}
    except (Exception, KeyboardInterrupt):
        error = {"code": "WEB_SERVICE_FAILED", "message": "网页服务启动或运行失败，请检查本机配置、历史数据和模型文件"}
    print(ERROR_PREFIX + json.dumps(error, ensure_ascii=False), file=sys.stderr, flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
