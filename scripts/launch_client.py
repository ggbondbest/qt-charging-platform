#!/usr/bin/env python3
"""Optionally launch the Qt client with map settings and a selected server.

Uses only Python's standard library. Extra client arguments follow `--`.
Closing the client exits this wrapper; Ctrl+C also stops its owned client.
The client also reads the shared map configuration when started directly.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def find_client(build_dir: Path | None = None) -> Path:
    from scripts.launch_demo import executable

    if build_dir is not None:
        return executable(build_dir.expanduser().resolve(), "client")
    for candidate in (ROOT / "build/Qt_6_12_0_for_macOS_Debug", ROOT / "build-delivery"):
        try:
            return executable(candidate, "client")
        except RuntimeError:
            pass
    raise RuntimeError("未找到客户端，请先构建 charging_client，或用 --build-dir 指定构建目录。")


def run_client(arguments: list[str], environment: dict[str, str]) -> int:
    """Wait for our child and clean up only that Popen handle on interruption."""
    process = None
    interrupted_signal = signal.SIGINT
    previous_term = signal.getsignal(signal.SIGTERM)

    def stop_requested(number, _frame):
        nonlocal interrupted_signal
        interrupted_signal = number
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_requested)
    try:
        process = subprocess.Popen(arguments, cwd=ROOT, env=environment)
        code = process.wait()
        return code if code >= 0 else 128 - code
    except KeyboardInterrupt:
        print("客户端已停止。")
        return 128 + interrupted_signal
    finally:
        try:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        finally:
            signal.signal(signal.SIGTERM, previous_term)


def main(argv=None):
    from scripts.launch_demo import configured_client_environment, port_number

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="Qt build directory containing client/")
    parser.add_argument("--services-config", type=Path, help="shared JSON; the client uses only tencentMap")
    parser.add_argument("--host", help="Qt business server address; default follows the client environment")
    parser.add_argument("--port", type=port_number, help="Qt business server port")
    parser.add_argument("client_args", nargs=argparse.REMAINDER, help="original client arguments after --")
    args = parser.parse_args(argv)
    if args.client_args and args.client_args[0] != "--":
        parser.error("其他客户端参数请放在 -- 之后。")
    try:
        environment = configured_client_environment(args.services_config)
        arguments = [str(find_client(args.build_dir))]
        if args.host is not None:
            if not args.host.strip() or "\0" in args.host:
                parser.error("服务器地址不能为空。")
            arguments.extend(["--host", args.host])
        if args.port is not None:
            arguments.extend(["--port", str(args.port)])
        arguments.extend(args.client_args[1:])
        return run_client(arguments, environment)
    except RuntimeError as exc:
        # Configuration errors name invalid fields, never credential values.
        print(f"客户端启动失败：{exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError):
        print("客户端启动失败：请检查构建产物、参数和执行权限。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
