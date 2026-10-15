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
import argparse, glob, json, os, posixpath, re, shutil, sys, urllib.parse
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import relay

HERE = os.path.dirname(os.path.abspath(__file__))


KNOWN_EXES = ('GameSaga.exe', 'Game8thwonder.exe', 'GameNorthland.exe')


class Handler(SimpleHTTPRequestHandler):
    game_default = overlay_root = None
    game_dirs = {}   # variant -> its own game folder (--version-dir), else game_default
    game = overlay = None   # per request: the variant's game folder and <overlay_root>/<variant>

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
    def select_overlay(self):
        v = (self.query().get('v') or ['gamesaga'])[0].lower()
        if not re.fullmatch(r'[a-z0-9_-]{1,40}', v):
            v = 'gamesaga'
        self.overlay = os.path.join(self.overlay_root, v)
        self.game = self.game_dirs.get(v, self.game_default)

    def versions(self):
        """Builds in web/ (cultures-<variant>.json) and whether their executable is in the game folder."""
        out = []
        for j in sorted(glob.glob(os.path.join(HERE, 'web', 'cultures-*.json'))):
            try:
                info = json.load(open(j))
            except Exception:
                continue
            info['built'] = os.path.exists(j[:-5] + '.wasm')
            d = self.game_dirs.get(info.get('variant'), self.game_default)
            info['present'] = os.path.isdir(d) and info.get('exe', '').lower() in {f.lower() for f in os.listdir(d)}
            out.append(info)
        order = [e.lower()[:-4] for e in KNOWN_EXES]
        out.sort(key=lambda i: (order.index(i['variant']) if i.get('variant') in order else len(order), i.get('variant', '')))
        self.reply(200, json.dumps(out).encode(), 'application/json')

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        self.select_overlay()
        if url.path == '/versions':
            return self.versions()
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
        self.select_overlay()
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
        self.select_overlay()
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


def migrate_userdata(root):
    """Saves used to live directly in userdata/; they belong to the Saga version."""
    old = os.path.join(root, 'Saves')
    if os.path.isdir(old) and not os.path.exists(os.path.join(root, 'gamesaga')):
        os.makedirs(os.path.join(root, 'gamesaga'))
        for name in os.listdir(root):
            if name != 'gamesaga':
                shutil.move(os.path.join(root, name), os.path.join(root, 'gamesaga', name))
        print('moved existing saves to', os.path.join(root, 'gamesaga'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.dirname(HERE), help='Cultures install directory (contains GameSaga.exe or another version)')
    ap.add_argument('--userdata', default=os.path.join(HERE, 'userdata'), help='where saves/settings are written')
    ap.add_argument('--version-dir', action='append', default=[], metavar='VARIANT=DIR',
                    help='game folder for one version, e.g. gamenorthland=/games/Northland (repeatable)')
    ap.add_argument('--port', type=int, default=8000)
    ap.add_argument('--bind', default='127.0.0.1')
    a = ap.parse_args()
    for spec in a.version_dir:
        v, _, d = spec.partition('=')
        if not d or not os.path.isdir(d):
            sys.exit('--version-dir %s: not a directory' % spec)
        Handler.game_dirs[v.lower()] = os.path.abspath(d)
    present = set()
    for d in [a.game] + list(Handler.game_dirs.values()):
        if os.path.isdir(d):
            present |= {f.lower() for f in os.listdir(d)}
    if not any(e.lower() in present for e in KNOWN_EXES):
        sys.exit('no Cultures executable (%s) found in %s (use --game or --version-dir)' % (', '.join(KNOWN_EXES), a.game))
    Handler.game_default = os.path.abspath(a.game)
    Handler.overlay_root = os.path.abspath(a.userdata)
    migrate_userdata(Handler.overlay_root)
    Handler.extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.js': 'text/javascript'}
    print('Cultures web port: http://%s:%d/  (game: %s, saves: %s)' % (a.bind, a.port, Handler.game_default, Handler.overlay_root))
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
