#!/usr/bin/env python3
"""WebSocket relay for Cultures multiplayer.

Every game (browser tab or native build) opens one WebSocket to /net. The relay
gives each connection a client id and forwards binary messages between clients;
the DirectPlay emulation in runtime/dplay.c builds sessions on top of it.

Wire format (binary frames, little-endian):
  client -> relay   u32 dst  + payload     dst 0 = control message for the relay
  relay  -> client  u32 src  + payload     src 0 = control message from the relay

Control messages (first payload byte):
  1 WELCOME   relay->client  u32 your_id
  2 HOST_ON   client->relay  16-byte application GUID   (list me as a session host)
  3 HOST_OFF  client->relay
  4 LIST      client->relay  16-byte application GUID
              relay->client  u32 n + n * u32 host ids
  5 GONE      relay->client  u32 id   (a client disconnected)

serve.py serves the relay at /net. To let players on other machines join, run a
relay they can reach, e.g.  python3 relay.py --bind 0.0.0.0 --port 8001
and enter ws://<this machine>:8001/ as the multiplayer server in the game's
settings. The relay never touches any files.
"""
import argparse, base64, hashlib, socket, struct, threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

GUID_WS = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
MAX_MSG = 1 << 20


def unmask(data, mask):
    n = len(data)
    key = int.from_bytes((mask * (n // 4 + 1))[:n], 'little')
    return (int.from_bytes(data, 'little') ^ key).to_bytes(n, 'little')


class Client:
    def __init__(self, cid, wfile):
        self.cid, self.wfile, self.lock = cid, wfile, threading.Lock()

    def send(self, data):
        n = len(data)
        if n < 126:
            hdr = struct.pack('!BB', 0x82, n)
        elif n < 65536:
            hdr = struct.pack('!BBH', 0x82, 126, n)
        else:
            hdr = struct.pack('!BBQ', 0x82, 127, n)
        try:
            with self.lock:
                self.wfile.write(hdr + data)
                self.wfile.flush()
        except OSError:
            pass


class Relay:
    def __init__(self):
        self.lock = threading.Lock()
        self.clients = {}       # id -> Client
        self.hosts = {}         # id -> application guid
        self.next_id = 1

    def handle(self, h):
        """Serve one WebSocket connection on an http.server request handler (blocking)."""
        key = h.headers.get('Sec-WebSocket-Key')
        if not key or 'websocket' not in h.headers.get('Upgrade', '').lower():
            h.send_error(400, 'WebSocket expected')
            return
        h.protocol_version = 'HTTP/1.1'
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID_WS).digest()).decode()
        h.send_response(101, 'Switching Protocols')
        h.send_header('Upgrade', 'websocket')
        h.send_header('Connection', 'Upgrade')
        h.send_header('Sec-WebSocket-Accept', accept)
        h.end_headers()
        h.wfile.flush()
        h.close_connection = True
        try:
            h.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        with self.lock:
            cid = self.next_id
            self.next_id += 1
            me = self.clients[cid] = Client(cid, h.wfile)
        me.send(struct.pack('<IBI', 0, 1, cid))
        try:
            for msg in self.frames(h.rfile, me):
                if len(msg) < 4:
                    continue
                dst = struct.unpack_from('<I', msg)[0]
                if dst == 0:
                    self.control(me, msg[4:])
                else:
                    with self.lock:
                        peer = self.clients.get(dst)
                    if peer:
                        peer.send(struct.pack('<I', cid) + msg[4:])
        finally:
            with self.lock:
                self.clients.pop(cid, None)
                self.hosts.pop(cid, None)
                others = list(self.clients.values())
            for c in others:
                c.send(struct.pack('<IBI', 0, 5, cid))

    def control(self, me, p):
        if not p:
            return
        op = p[0]
        with self.lock:
            if op == 2:
                self.hosts[me.cid] = bytes(p[1:17])
            elif op == 3:
                self.hosts.pop(me.cid, None)
            elif op == 4:
                ids = [c for c, g in self.hosts.items() if g == bytes(p[1:17]) and c != me.cid]
                reply = struct.pack('<IBI', 0, 4, len(ids)) + b''.join(struct.pack('<I', c) for c in ids)
            else:
                return
        if op == 4:
            me.send(reply)

    @staticmethod
    def frames(rfile, me):
        """Yield complete binary messages; answers pings, stops on close or EOF."""
        buf = b''
        while True:
            hdr = rfile.read(2)
            if len(hdr) < 2:
                return
            b0, b1 = hdr
            op, n = b0 & 15, b1 & 127
            if n == 126:
                n = struct.unpack('!H', rfile.read(2))[0]
            elif n == 127:
                n = struct.unpack('!Q', rfile.read(8))[0]
            if n > MAX_MSG:
                return
            mask = rfile.read(4) if b1 & 128 else None
            data = rfile.read(n)
            if len(data) < n:
                return
            if mask:
                data = unmask(data, mask)
            if op == 8:
                return
            if op == 9:
                with me.lock:
                    me.wfile.write(bytes([0x8a, len(data)]) + data); me.wfile.flush()
                continue
            if op in (0, 1, 2):
                buf += data
                if b0 & 128:
                    yield buf
                    buf = b''


RELAY = Relay()


class RelayOnly(BaseHTTPRequestHandler):
    def do_GET(self):
        RELAY.handle(self)

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description='Cultures multiplayer relay (WebSocket)')
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=8001)
    a = ap.parse_args()
    print('Cultures multiplayer relay on ws://%s:%d/' % (a.bind, a.port))
    ThreadingHTTPServer((a.bind, a.port), RelayOnly).serve_forever()


if __name__ == '__main__':
    main()
