#!/usr/bin/env python3
"""Local server for the Cultures WebAssembly port.

Serves the web front-end with the cross-origin isolation headers needed for
SharedArrayBuffer, and exposes the game directory to the browser:

  GET  /fs/index              JSON index of all game files (+ user overlay)
  GET  /fs/file?path=...      file contents (supports Range)
  PUT  /fs/file?path=...      replace file contents in the user overlay
  POST /fs/op                 {"op": "unlink"|"mkdir"|"rename", "path":..., "to":...}
  GET  /net                   WebSocket multiplayer relay (see relay.py)

The original game files are never modified: everything the game writes goes to
the overlay directory (default: ./userdata).

usage: python3 serve.py [--game DIR] [--userdata DIR] [--port 8000]
"""
import argparse, json, os, posixpath, shutil, sys, urllib.parse
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import relay

HERE = os.path.dirname(os.path.abspath(__file__))


class Handler(SimpleHTTPRequestHandler):
    game = overlay = None

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=os.path.join(HERE, 'web'), **kw)

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def log_message(self, fmt, *args):
        if '/fs/' not in self.path:
            super().log_message(fmt, *args)

    # ------------------------------------------------------------ helpers
    def safe(self, rel):
        rel = posixpath.normpath('/' + rel.replace('\\', '/')).lstrip('/')
        if rel.startswith('..'):
            raise ValueError('bad path')
        return rel

    def whiteouts(self):
        p = os.path.join(self.overlay, '.deleted.json')
        try:
            return set(json.load(open(p)))
        except Exception:
            return set()

    def save_whiteouts(self, s):
        os.makedirs(self.overlay, exist_ok=True)
        json.dump(sorted(s), open(os.path.join(self.overlay, '.deleted.json'), 'w'))

    def locate(self, rel):
        o = os.path.join(self.overlay, rel)
        if os.path.exists(o):
            return o
        if rel in self.whiteouts():
            return None
        g = os.path.join(self.game, rel)
        return g if os.path.exists(g) else None

    def reply(self, code, body=b'', ctype='application/octet-stream', extra=()):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def query(self):
        return urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)

    # ------------------------------------------------------------ verbs
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == '/fs/index':
            return self.fs_index()
        if url.path == '/fs/file':
            return self.fs_read()
        if url.path == '/net':
            return relay.RELAY.handle(self)
        return super().do_GET()

    def fs_index(self):
        idx = {}
        wo = self.whiteouts()
        for root, tag in ((self.game, 'g'), (self.overlay, 'o')):
            if not os.path.isdir(root):
                continue
            for dp, dns, fns in os.walk(root):
                dns[:] = [d for d in dns if os.path.realpath(os.path.join(dp, d)) != HERE]
                rd = os.path.relpath(dp, root)
                rd = '' if rd == '.' else rd.replace(os.sep, '/')
                for d in dns:
                    p = posixpath.join(rd, d)
                    if tag == 'g' and p in wo:
                        continue
                    idx[p] = [1, 0]
                for f in fns:
                    p = posixpath.join(rd, f)
                    if f == '.deleted.json' or (tag == 'g' and p in wo):
                        continue
                    idx[p] = [0, os.path.getsize(os.path.join(dp, f))]
        self.reply(200, json.dumps(idx).encode(), 'application/json')

    def fs_read(self):
        try:
            rel = self.safe(self.query()['path'][0])
        except Exception:
            return self.reply(400)
        p = self.locate(rel)
        if not p or os.path.isdir(p):
            return self.reply(404)
        size = os.path.getsize(p)
        rng = self.headers.get('Range')
        with open(p, 'rb') as f:
            if rng and rng.startswith('bytes='):
                a, b = rng[6:].split('-')
                a = int(a); b = min(int(b) if b else size - 1, size - 1)
                if a >= size:
                    return self.reply(416, extra=[('Content-Range', 'bytes */%d' % size)])
                f.seek(a)
                data = f.read(b - a + 1)
                return self.reply(206, data, extra=[('Content-Range', 'bytes %d-%d/%d' % (a, b, size))])
            return self.reply(200, f.read())

    def do_PUT(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != '/fs/file':
            return self.reply(404)
        try:
            rel = self.safe(self.query()['path'][0])
        except Exception:
            return self.reply(400)
        n = int(self.headers.get('Content-Length', 0))
        data = self.rfile.read(n)
        dst = os.path.join(self.overlay, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
        wo = self.whiteouts()
        if rel in wo:
            wo.discard(rel); self.save_whiteouts(wo)
        self.reply(200, b'ok', 'text/plain')

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != '/fs/op':
            return self.reply(404)
        req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        try:
            rel = self.safe(req['path'])
            op = req['op']
            if op == 'mkdir':
                os.makedirs(os.path.join(self.overlay, rel), exist_ok=True)
            elif op == 'unlink':
                o = os.path.join(self.overlay, rel)
                if os.path.isfile(o):
                    os.remove(o)
                if os.path.exists(os.path.join(self.game, rel)):
                    wo = self.whiteouts(); wo.add(rel); self.save_whiteouts(wo)
            elif op == 'rename':
                to = self.safe(req['to'])
                src = self.locate(rel)
                if not src:
                    return self.reply(404)
                dst = os.path.join(self.overlay, to)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                o = os.path.join(self.overlay, rel)
                if os.path.isfile(o):
                    os.remove(o)
                if os.path.exists(os.path.join(self.game, rel)):
                    wo = self.whiteouts(); wo.add(rel); self.save_whiteouts(wo)
            else:
                return self.reply(400)
        except Exception as e:
            return self.reply(500, str(e).encode(), 'text/plain')
        self.reply(200, b'ok', 'text/plain')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.dirname(HERE), help='Cultures install directory (contains GameSaga.exe)')
    ap.add_argument('--userdata', default=os.path.join(HERE, 'userdata'), help='where saves/settings are written')
    ap.add_argument('--port', type=int, default=8000)
    ap.add_argument('--bind', default='127.0.0.1')
    a = ap.parse_args()
    if not os.path.exists(os.path.join(a.game, 'GameSaga.exe')):
        sys.exit('GameSaga.exe not found in %s (use --game)' % a.game)
    Handler.game = os.path.abspath(a.game)
    Handler.overlay = os.path.abspath(a.userdata)
    Handler.extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.js': 'text/javascript'}
    print('Cultures web port: http://%s:%d/  (game: %s, saves: %s)' % (a.bind, a.port, Handler.game, Handler.overlay))
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
