"""轻量集成检查：生产代理模块转发真实文件服务，不启动桌面或守护。"""
import concurrent.futures
import hashlib
import http.client
import http.server
import pathlib
import secrets
import socket
import struct
import subprocess
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / 'windows/build/proxy-tests'
process = None
httpd = None


def exact(peer, count):
    data = b''
    while len(data) < count:
        chunk = peer.recv(count - len(data))
        if not chunk:
            raise RuntimeError('连接提前关闭')
        data += chunk
    return data


def http_get(path):
    connection = http.client.HTTPConnection('127.0.0.1', 18080, timeout=15)
    try:
        connection.request('GET', 'http://localhost:18081/' + path)
        response = connection.getresponse()
        data = response.read()
        assert response.status == 200, response.status
        return data
    finally:
        connection.close()


def socks(domain=True):
    peer = socket.create_connection(('127.0.0.1', 18080), timeout=15)
    peer.sendall(b'\x05\x01\x00')
    assert exact(peer, 2) == b'\x05\x00'
    address = b'\x03\x09localhost' if domain else b'\x01\x7f\x00\x00\x01'
    peer.sendall(b'\x05\x01\x00' + address + struct.pack('!H', 18081))
    assert exact(peer, 10)[:2] == b'\x05\x00'
    return peer


def finish(peer):
    peer.sendall(b'GET /README.md HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
    peer.shutdown(socket.SHUT_WR)
    response = http.client.HTTPResponse(peer)
    response.begin()
    assert response.status == 200
    assert response.read() == (ROOT / 'README.md').read_bytes()
    peer.close()


try:
    token = secrets.token_hex(32)
    (WORK / 'host.ini').write_text(
        '[connection]\nmode=reverse\ncontroller_host=127.0.0.1\ntoken=' + token +
        '\n[proxy]\ntunnel_port=17403\nconnections=8\n', encoding='ascii')
    (WORK / 'client.ini').write_text('[proxy]\nlocal_port=18080\ntunnel_port=17403\n', encoding='ascii')
    handler = lambda *args, **kwargs: http.server.SimpleHTTPRequestHandler(*args, directory=str(ROOT), **kwargs)
    httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 18081), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    process = subprocess.Popen([str(WORK / 'proxy_smoke.exe')], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert process.stdout.readline().strip() == b'READY'
    time.sleep(1)
    assert http_get('README.md') == (ROOT / 'README.md').read_bytes()
    print('HTTP 和被控端域名解析通过', flush=True)
    for domain in (True, False):
        finish(socks(domain))
    print('SOCKS5 域名、IPv4 和 TCP 半关闭通过', flush=True)
    connection = http.client.HTTPConnection('127.0.0.1', 18080, timeout=15)
    connection.set_tunnel('localhost', 18081)
    connection.request('GET', '/README.md')
    response = connection.getresponse()
    assert response.status == 200 and response.read() == (ROOT / 'README.md').read_bytes()
    connection.close()
    print('HTTP CONNECT 转发通过', flush=True)
    target = 'windows/build/proxy-tests/proxy_smoke.exe'
    expected = hashlib.sha256((ROOT / target).read_bytes()).digest()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda _: hashlib.sha256(http_get(target)).digest(), range(8)))
    assert all(result == expected for result in results)
    print('八次并发真实文件传输与 SHA-256 校验通过', flush=True)
    peer = socket.create_connection(('127.0.0.1', 18080), timeout=15)
    peer.sendall(b'\x05\x01\x00')
    assert exact(peer, 2) == b'\x05\x00'
    peer.sendall(b'\x05\x03\x00\x01\x7f\x00\x00\x01\x00\x35')
    assert exact(peer, 10)[1] == 7
    peer.close()
    print('不支持的 UDP 请求明确拒绝通过', flush=True)
    peer = socket.create_connection(('127.0.0.1', 17403), timeout=8)
    assert exact(peer, 40)[:8] == b'WSPXY1\r\n'
    peer.sendall(bytes(64))
    assert peer.recv(1) == b''
    peer.close()
    print('错误认证拒绝通过', flush=True)
    active = socks()
    started = time.monotonic()
    process.communicate(input=b'\n', timeout=8)
    assert process.returncode == 0
    active.close()
    print(f'存在活动和空闲连接时正常退出通过，耗时 {time.monotonic() - started:.2f} 秒', flush=True)
finally:
    if process and process.poll() is None:
        process.kill()
        process.wait()
    if httpd:
        httpd.shutdown()
