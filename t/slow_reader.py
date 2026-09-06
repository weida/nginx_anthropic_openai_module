#!/usr/bin/env python3
"""Real nginx/TCP pause-resume test, with measured socket pressure.

This verifies kernel queues and producer blocking, not nginx's exact NGX_AGAIN
return values. The separate filter_schedule harness proves those exact states.
"""
import array
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
EVENTS = 256
READ_SIZE = 8192
READ_INTERVAL = 0.004
PAUSE_SECONDS = 0.8
TOTAL_TIMEOUT = 30
SIOCOUTQ = 0x5411  # Linux socket bytes queued for transmission.


def text(i):
    return f'{i:04d}|' + 'abcdefghijklmnop' * 2047 + '🙂漢\n'


def frame(delta=None, finish=None):
    obj = {'choices': [{'delta': delta or {}, 'finish_reason': finish}]}
    return ('data: ' + json.dumps(obj, ensure_ascii=False) + '\n\n').encode()


def nginx_send_queue(server_port, client_port):
    """Read the real accepted nginx TCP connection's outstanding send bytes."""
    for line in Path('/proc/net/tcp').read_text().splitlines()[1:]:
        fields = line.split()
        if (int(fields[1].split(':')[1], 16) == server_port
                and int(fields[2].split(':')[1], 16) == client_port
                and fields[3] == '01'):
            return int(fields[4].split(':')[0], 16)
    return 0


class Decoder:
    def __init__(self):
        self.wire = bytearray()
        self.sse = bytearray()
        self.chunk_size = None
        self.complete = False
        self.events = []
        self.pieces = []
        self.open_blocks = set()
        self.stopped = False
        self.body_bytes = 0

    def feed(self, data):
        assert not self.complete or not data, 'bytes after HTTP completion'
        self.wire.extend(data)
        while True:
            if self.chunk_size is None:
                at = self.wire.find(b'\r\n')
                if at < 0:
                    return
                self.chunk_size = int(self.wire[:at].split(b';')[0], 16)
                del self.wire[:at + 2]
            size = self.chunk_size
            if len(self.wire) < size + 2:
                return
            assert self.wire[size:size + 2] == b'\r\n', 'invalid HTTP chunk ending'
            payload = self.wire[:size]
            del self.wire[:size + 2]
            self.chunk_size = None
            if size == 0:
                self.complete = True
                assert not self.wire and not self.sse, 'incomplete trailing bytes'
                return
            self.body_bytes += len(payload)
            self.sse.extend(payload)
            while True:
                at = self.sse.find(b'\n\n')
                if at < 0:
                    break
                raw = bytes(self.sse[:at])
                del self.sse[:at + 2]
                assert len(raw) + 2 <= 262144, 'event exceeds one output slot'
                event, data = raw.split(b'\ndata: ', 1)
                assert event.startswith(b'event: ')
                obj = json.loads(data)
                kind = obj['type']
                assert event[7:].decode() == kind
                assert not self.stopped, 'event after message_stop'
                if kind == 'content_block_start':
                    assert obj['index'] == 0 and not self.open_blocks
                    assert obj['content_block'] == {'type': 'text', 'text': ''}
                    self.open_blocks.add(obj['index'])
                elif kind == 'content_block_delta':
                    assert obj['index'] in self.open_blocks
                    assert obj['delta']['type'] == 'text_delta'
                    piece = obj['delta']['text']
                    assert len(self.pieces) < EVENTS, 'duplicate extra text delta'
                    assert piece == text(len(self.pieces)), 'missing, reordered or duplicated text'
                    self.pieces.append(piece)
                elif kind == 'content_block_stop':
                    assert obj['index'] in self.open_blocks
                    self.open_blocks.remove(obj['index'])
                elif kind == 'message_delta':
                    assert not self.open_blocks
                    assert obj['delta']['stop_reason'] == 'end_turn'
                elif kind == 'message_stop':
                    assert not self.open_blocks
                    self.stopped = True
                else:
                    assert kind in ('message_start', 'ping'), f'unexpected {kind}'
                self.events.append(kind)


def run():
    frames = [frame({'content': text(i)}) for i in range(EVENTS)]
    frames.append(frame(finish='stop') + b'data: [DONE]\n\n')
    upstream_bytes = sum(map(len, frames))
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(5)
    reserve = socket.socket()
    reserve.bind(('127.0.0.1', 0))
    port = reserve.getsockname()[1]
    reserve.close()
    state = {'socket': None, 'sent': 0, 'finished': False,
             'error': None, 'writes': [], 'sndbuf': 0}

    def upstream():
        try:
            conn, _ = listener.accept()
            state['socket'] = conn
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
            state['sndbuf'] = conn.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
            conn.settimeout(TOTAL_TIMEOUT)
            request = bytearray()
            while b'\r\n\r\n' not in request:
                chunk = conn.recv(8192)
                assert chunk, 'truncated upstream request'
                request.extend(chunk)
            headers, body = request.split(b'\r\n\r\n', 1)
            length = next(int(line.split(b':', 1)[1]) for line in headers.split(b'\r\n')
                          if line.lower().startswith(b'content-length:'))
            while len(body) < length:
                chunk = conn.recv(length - len(body))
                assert chunk, 'truncated upstream request body'
                body.extend(chunk)
            assert json.loads(body)['stream'] is True
            conn.sendall(('HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n'
                          'X-Accel-Buffering: yes\r\nConnection: close\r\n'
                          f'Content-Length: {upstream_bytes}\r\n\r\n').encode())
            for item in frames:
                start = time.monotonic()
                conn.sendall(item)
                end = time.monotonic()
                state['writes'].append((start, end))
                state['sent'] += len(item)
            state['finished'] = True
        except BaseException as error:
            state['error'] = error
        finally:
            if state['socket'] is not None:
                state['socket'].close()

    producer = threading.Thread(target=upstream, daemon=True)
    nginx = None
    client = None
    with tempfile.TemporaryDirectory(prefix='ao-slow-reader-') as temp:
        directory = Path(temp)
        (directory / 'logs').mkdir()
        conf = directory / 'nginx.conf'
        conf.write_text(f'''load_module {ROOT}/build/ngx_http_anthropic_openai_module.so;
daemon off; master_process off; pid {temp}/nginx.pid;
error_log {temp}/error.log info;
events {{ worker_connections 128; }}
http {{ access_log off; client_body_temp_path {temp}/body;
server {{ listen 127.0.0.1:{port} sndbuf=16k; tcp_nodelay on;
location / {{ anthropic_openai on; proxy_buffering on; proxy_buffer_size 4k;
proxy_read_timeout 10s; send_timeout 10s; proxy_next_upstream off;
proxy_pass http://127.0.0.1:{listener.getsockname()[1]}; }} }} }}
''')
        try:
            nginx = subprocess.Popen([os.environ.get('TEST_NGINX_BINARY', '/usr/sbin/nginx'),
                                      '-p', temp + '/', '-c', str(conf)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            startup = time.monotonic()
            while True:
                assert nginx.poll() is None, 'nginx exited during startup'
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    assert time.monotonic() - startup < 4, 'nginx startup timeout'
                    time.sleep(.02)
            producer.start()
            client = socket.socket()
            client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
            rcvbuf = client.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            client.settimeout(2)
            client.connect(('127.0.0.1', port))
            client_port = client.getsockname()[1]
            body = json.dumps({'model': 'slow-reader', 'max_tokens': 200000,
                               'stream': True, 'messages': []}).encode()
            client.sendall(b'POST /v1/messages HTTP/1.1\r\nHost: localhost\r\n'
                           b'Connection: close\r\nContent-Type: application/json\r\n'
                           + f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
            raw = bytearray()
            while b'\r\n\r\n' not in raw:
                chunk = client.recv(READ_SIZE)
                assert chunk, 'EOF before response headers'
                raw.extend(chunk)
            headers, initial = raw.split(b'\r\n\r\n', 1)
            assert headers.startswith(b'HTTP/1.1 200 ')
            assert b'transfer-encoding: chunked' in headers.lower()
            assert b'content-type: text/event-stream' in headers.lower()
            decoder = Decoder()
            decoder.feed(initial)
            while not decoder.pieces:
                chunk = client.recv(READ_SIZE)
                assert chunk, 'EOF before first text event'
                decoder.feed(chunk)

            # Do not call recv at all during the pause. Sample both directions:
            # nginx has bytes outstanding to this client, and the small-buffer
            # upstream sender becomes non-writable with bytes still queued.
            pause_start = time.monotonic()
            samples = []
            while time.monotonic() - pause_start < PAUSE_SECONDS:
                conn = state['socket']
                pending = array.array('i', [0])
                if conn is not None and not state['finished']:
                    fcntl.ioctl(conn, SIOCOUTQ, pending, True)
                    writable = bool(select.select([], [conn], [], 0)[1])
                else:
                    writable = True
                samples.append((time.monotonic(), nginx_send_queue(port, client_port),
                                pending[0], writable, state['sent']))
                time.sleep(.02)
            pause_end = time.monotonic()
            pressured = [s for s in samples if s[1] > 0 and s[2] > 0 and not s[3]]
            assert len(pressured) >= 5, f'no sustained measured TCP pressure: {samples}'
            assert pressured[-1][0] - pressured[0][0] >= .1
            assert not state['finished'], 'entire response fit in transport buffers'
            paused_sent = state['sent']

            # Resume with a delay before every read, for the entire remaining
            # multi-megabyte response (no final fast-drain shortcut).
            read_start = time.monotonic()
            reads = 0
            while True:
                assert time.monotonic() - read_start < TOTAL_TIMEOUT, 'slow-read completion deadline'
                time.sleep(READ_INTERVAL)
                chunk = client.recv(READ_SIZE)
                reads += 1
                if not chunk:
                    break
                decoder.feed(chunk)
            read_seconds = time.monotonic() - read_start
            producer.join(2)
            assert not producer.is_alive(), 'upstream did not finish after client resumed'
            assert state['error'] is None, state['error']
            assert state['finished'] and state['sent'] == upstream_bytes
            assert decoder.complete and decoder.stopped, 'HTTP/SSE did not complete'
            assert not decoder.sse and not decoder.wire
            assert decoder.events == (['message_start', 'ping', 'content_block_start']
                                      + ['content_block_delta'] * EVENTS
                                      + ['content_block_stop', 'message_delta', 'message_stop'])
            actual = ''.join(decoder.pieces).encode()
            expected = ''.join(text(i) for i in range(EVENTS)).encode()
            assert actual == expected
            assert reads >= 500 and read_seconds >= reads * READ_INTERVAL * .95
            blocked_writes = [(a, b) for a, b in state['writes']
                              if b - a >= .1 and a < pause_end and b > pause_start]
            assert blocked_writes, 'upstream sendall never measurably blocked during client pause'
            digest = hashlib.sha256(actual).hexdigest()
            print('ok real nginx slow reader: '
                  f'{EVENTS} ordered deltas, {len(actual)} text bytes, {decoder.body_bytes} SSE bytes; '
                  f'pause={pause_end-pause_start:.3f}s, reads={reads}x<={READ_SIZE} bytes '
                  f'with {READ_INTERVAL:.3f}s/read, drain={read_seconds:.3f}s')
            print('ok measured transport pressure: '
                  f'client SO_RCVBUF={rcvbuf}, nginx listen sndbuf=16384, '
                  f'upstream SO_SNDBUF={state["sndbuf"]}; '
                  f'{len(pressured)} simultaneous nginx-send-queue/upstream-nonwritable samples, '
                  f'peak nginx queued={max(s[1] for s in samples)}, '
                  f'producer sent only {paused_sent}/{upstream_bytes} bytes during pause; '
                  f'overlapping sendall block={max(b-a for a,b in blocked_writes):.3f}s')
            print(f'ok exact reconstruction sha256={digest}; one message_stop; complete HTTP then TCP EOF')
        except BaseException:
            log = directory / 'error.log'
            if log.exists():
                print(log.read_text(), flush=True)
            raise
        finally:
            if client is not None:
                client.close()
            if nginx is not None:
                nginx.terminate()
                try:
                    nginx.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    nginx.kill()
                    nginx.wait(timeout=2)
                if nginx.stderr is not None:
                    nginx.stderr.close()
            listener.close()
            if state['socket'] is not None:
                try:
                    state['socket'].shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            if producer.ident is not None:
                producer.join(2)


if __name__ == '__main__':
    run()
