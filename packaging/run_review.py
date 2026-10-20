#!/usr/bin/env python3
"""Self-contained review launcher. Uses only bundled runtimes and private demo data."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen

PROJECT = Path(__file__).resolve().parents[1]
RESOURCES = PROJECT.parent
CONTENTS = RESOURCES.parent
BIN = CONTENTS / 'MacOS'


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def write_private(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf8')
    path.chmod(0o600)


def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=.2):
            return True
    except OSError:
        return False


def get_json(base, path):
    with urlopen(base + path, timeout=30) as response:
        return json.load(response)


def verify_models(base):
    registry = get_json(base, '/api/v1/intelligence/models')['data']
    families = ('load', 'availability', 'insights', 'arrival')
    if any(registry[name]['status'] != 'READY' for name in families):
        raise RuntimeError('本地模型未全部就绪。')
    pin = {key: registry[key] for key in ('datasetId', 'publishedBatchId')}
    history = get_json(base, '/api/v1/dashboard/overview?' + urlencode(pin))
    get_json(base, '/api/v1/dashboard/advanced?' + urlencode(pin))
    for target in ('load', 'availability'):
        model = next(item for item in registry[target]['models'] if item['horizonHours'] == 1)
        body = {**pin, 'target': target, 'modelId': model['modelId'], 'stationId': 'ST-DL-01',
                'cityId': 'DL', 'referenceTime': registry['defaultReferenceTime'], 'horizonHours': 1}
        request = Request(base + '/api/v1/intelligence/forecast', data=json.dumps(body).encode(),
                          headers={'Content-Type': 'application/json'})
        with urlopen(request, timeout=45) as response:
            result = json.load(response)
        if result['code'] != 'OK' or len(result['data']['points']) != 1:
            raise RuntimeError('本地预测未通过验证。')
    for kind in ('anomalies', 'churn'):
        get_json(base, '/api/v1/intelligence/insights/' + kind + '?' + urlencode({**pin, 'limit': 1}))
    replay = get_json(base, '/api/v1/chargepilot/bootstrap')
    return {'families': {name: registry[name]['status'] for name in families},
            'loadPrediction': True, 'availabilityPrediction': True, 'insights': True,
            'advancedHistory': True, 'replay': replay['meta']['mode'],
            'historicalSource': history['data']['source'], 'externalAiRequests': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true', help='Check packaged services and exit without external AI/map calls')
    parser.add_argument('--data-dir', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--no-client', action='store_true')
    args = parser.parse_args()
    identity = hashlib.sha256(str(CONTENTS).encode()).hexdigest()[:12]
    state = args.data_dir or (Path.home() / 'Library/Application Support/ChargingPlatformReview' / identity)
    state = state.expanduser().resolve()
    state.mkdir(parents=True, exist_ok=True)
    state.chmod(0o700)
    # Prevent simultaneous owners of this demo datadir. Lock is released on crash.
    import fcntl
    lock = (state / 'instance.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('此评审包已在运行，请使用已打开的窗口。', flush=True)
        return 2
    seed = RESOURCES / 'mysql-seed'
    mysql_data = state / 'mysql'
    if not mysql_data.exists():
        print('首次启动：正在创建独立演示数据副本…', flush=True)
        temporary = state / 'mysql-copying'
        if temporary.exists():
            shutil.rmtree(temporary)
        shutil.copytree(seed / 'data', temporary)
        temporary.rename(mysql_data)
    tcp_port, mysql_port = free_port(), free_port()
    while mysql_port == tcp_port:
        mysql_port = free_port()
    mysql_env = json.loads((seed / 'environment.json').read_text(encoding='utf8'))
    for prefix in ('ANALYTICS', 'CHARGEPILOT'):
        mysql_env[prefix + '_MYSQL_HOST'] = '127.0.0.1'
        mysql_env[prefix + '_MYSQL_PORT'] = str(mysql_port)
    mysql_env.update(OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='2', MKL_NUM_THREADS='2')
    environment_file = state / 'service-environment.json'
    write_private(environment_file, mysql_env)
    config_file = state / 'dashboard-service.json'
    write_private(config_file, {
        'pythonExecutable': str(RESOURCES / 'python/bin/python3'), 'mode': 'full',
        'environmentFile': str(environment_file),
        'mysql': {'executable': str(RESOURCES / 'mysql/bin/mysqld'),
                  'baseDir': str(RESOURCES / 'mysql'), 'dataDir': str(mysql_data),
                  'port': mysql_port, 'runtimeDir': str(state / 'mysql-runtime')}})
    env = os.environ.copy()
    # Prevent unrelated developer/runtime configuration from redirecting this bundle.
    for name in list(env):
        if name.startswith(('CHARGING_', 'ANALYTICS_', 'CHARGEPILOT_', 'ML_ADVISOR_', 'TENCENT_MAP_',
                            'PYTHON', 'QT_', 'QML', 'DYLD_', 'AVAILABILITY_MODEL_', 'INSIGHTS_MODEL_')):
            env.pop(name, None)
    env.update(CHARGING_SERVICES_CONFIG=str(PROJECT / 'config/services.local.json'),
               CHARGING_DASHBOARD_CONFIG=str(config_file),
               CHARGING_REVIEW_DATA_DIR=str(state),
               CHARGING_ML_CACHE_DIR=str(state / 'ml-cache'),
               PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', PYTHONUNBUFFERED='1',
               OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='2', MKL_NUM_THREADS='2')
    if args.verify:
        # Exercise both native processes without spending online credentials.
        verification_config = state / 'verification-services.json'
        write_private(verification_config, {'version': 1, 'ai': {'enabled': False}, 'tencentMap': {}})
        env['CHARGING_SERVICES_CONFIG'] = str(verification_config)
    events = queue.Queue()
    children = []
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())

    def read_output(process):
        for line in process.stdout:
            # Qt's own readiness contract is the only child output exposed.
            if line.startswith('CHARGING_DASHBOARD_READY='):
                events.put(line.partition('=')[2].strip())

    result = {'platform': 'macOS 15+ / Apple Silicon', 'status': 'FAILED'}
    api_port = None
    try:
        server = subprocess.Popen([str(BIN / 'charging-server-real'), '--database',
            str(state / 'business.sqlite3'), '--demo-seed', '--address', '127.0.0.1', '--port', str(tcp_port)],
            cwd=PROJECT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding='utf8', errors='replace')
        children.append(server)
        threading.Thread(target=read_output, args=(server,), daemon=True).start()
        print('正在启动运营管理端及本地历史分析服务…', flush=True)
        deadline = time.monotonic() + 120
        url = None
        while time.monotonic() < deadline and not stop.is_set():
            if server.poll() is not None:
                raise RuntimeError('管理端未能启动，请检查系统版本及目录权限。')
            try:
                url = events.get(timeout=.2)
                break
            except queue.Empty:
                continue
        if not url:
            raise RuntimeError('本地服务准备超时，请关闭窗口后重试。')
        parsed = urlsplit(url)
        if parsed.hostname != '127.0.0.1' or parsed.scheme != 'http' or not parsed.port:
            raise RuntimeError('本地服务返回地址异常。')
        api_port = parsed.port
        base = 'http://127.0.0.1:' + str(api_port)
        snapshot = get_json(base, '/api/v1/qt-live/snapshot')
        if snapshot.get('code') != 'OK':
            raise RuntimeError('演示业务数据未就绪。')
        if not args.no_client:
            client = subprocess.Popen([str(BIN / 'charging-client'), '--host', '127.0.0.1',
                                       '--port', str(tcp_port)], cwd=PROJECT, env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            children.append(client)
            time.sleep(1)
            if client.poll() is not None:
                raise RuntimeError('客户端未能启动。')
        print('评审环境已就绪：管理端、客户端、历史分析及四类本地模型可用。', flush=True)
        if args.verify:
            result.update(status='PASS', qtLive=snapshot['data']['summary'],
                          vueServed=urlopen(base + '/', timeout=10).status == 200,
                          clientStarted=not args.no_client, fullServiceReady=True,
                          isolatedBusinessDatabase=True, isolatedMysql=True,
                          externalMapAndAiDisabledDuringVerification=True,
                          modelChecks=verify_models(base))
            print('移动目录启动验证通过；正在清理本次独立服务。', flush=True)
        else:
            while not stop.is_set() and server.poll() is None:
                time.sleep(.2)
            result['status'] = 'PASS' if stop.is_set() or server.returncode == 0 else 'FAILED'
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
        # Parent watcher owns cleanup even when the native process exits abruptly.
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline and (listening(mysql_port) or (api_port and listening(api_port))):
            time.sleep(.2)
        result['ownedServicesStopped'] = not listening(mysql_port) and (not api_port or not listening(api_port))
        if not result['ownedServicesStopped']:
            result['status'] = 'FAILED'
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError):
        # Only these locally constructed errors are user-facing; never print config.
        print('评审包启动失败。请检查系统版本、包是否完整及演示数据目录权限。', file=sys.stderr)
        raise SystemExit(1) from None
