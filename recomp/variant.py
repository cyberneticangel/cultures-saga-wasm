#!/usr/bin/env python3
"""Identify Cultures game executables by content, not by file name.

The original releases all call their executable Game.exe, so the file name cannot tell the
versions apart.  Each build ("variant") is named after the executable's CRC-32 instead.

usage:
  variant.py name EXE        print the variant name (gamesaga, game8thwonder, ... or <name>-<crc>)
  variant.py json EXE        print the web/cultures-<variant>.json description
  variant.py scan DIR...     print every Cultures game executable in the folders and their subfolders
  variant.py scan-unique DIR...  the same, but one executable per variant
  variant.py find VERSION DIR...  the one executable to build: VERSION is a short name (saga,
                             8thwonder, northland), a variant or a CRC, or "" if only one version
                             is in the folders; prints the choices and fails otherwise
"""
import json, os, sys, zlib

# CRC-32 of known executables -> (variant, title)
KNOWN = {
    0x02a8132f: ('gamesaga', 'Cultures – Die Saga (2005)'),
    0x3f104927: ('game8thwonder', 'Cultures – 8th Wonder of the World (2003)'),
    0x8d7952ab: ('gamenorthland', 'Cultures – Northland (2003)'),
}
# short names for VERSION=
SHORT = {'gamesaga': 'saga', 'game8thwonder': '8thwonder', 'gamenorthland': 'northland'}
ENGINE_MARK = b'datax\\libs\\data%4.4d.lib'   # every executable of this engine opens its archives this way
GAME_MARK = b'data\\maps\\demo_mainmenu'       # ...but only the games (not Editor.exe) load the menu's demo map


def crc_of(path):
    return zlib.crc32(open(path, 'rb').read()) & 0xffffffff


def is_game_exe(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return False
    return data[:2] == b'MZ' and ENGINE_MARK in data and GAME_MARK in data


def describe(path):
    crc = crc_of(path)
    name = os.path.basename(path)
    variant, title = KNOWN.get(crc, (None, None))
    if not variant:
        stem = ''.join(c for c in name.lower()[:-4] if c.isalnum()) or 'game'
        variant, title = '%s-%08x' % (stem, crc), 'Cultures (%s, %08x)' % (name, crc)
    return {'variant': variant, 'exe': name, 'crc': '%08x' % crc, 'title': title}


def scan_dir(d):
    """Game executables directly in folder d."""
    for f in sorted(os.listdir(d)):
        p = os.path.join(d, f)
        if f.lower().endswith('.exe') and os.path.isfile(p) and is_game_exe(p):
            yield p


def game_folders(dirs):
    """dirs plus their immediate subfolders (e.g. a Saga install with Northland/ and 8th Wonder/ inside)."""
    seen = set()
    for d in dirs:
        if not os.path.isdir(d):
            continue
        subs = sorted(os.path.join(d, s) for s in os.listdir(d) if s != 'port')
        for f in [d] + [s for s in subs if os.path.isdir(s)]:
            f = os.path.abspath(f)
            if f not in seen:
                seen.add(f)
                yield f


def scan(dirs):
    for d in game_folders(dirs):
        yield from scan_dir(d)


def scan_unique(dirs):
    """One executable per variant, from the folder dedicated to it (fewest game executables)."""
    best = {}
    for d in game_folders(dirs):
        exes = list(scan_dir(d))
        for p in exes:
            v = describe(p)['variant']
            if v not in best or len(exes) < best[v][0]:
                best[v] = (len(exes), p)
    return [p for _, p in best.values()]


def find(version, dirs):
    """(exe, None) for the executable VERSION names, or (None, message) explaining the choices"""
    found = [(p, describe(p)) for p in scan_unique(dirs)]
    v = version.strip().lower()
    if v:
        hit = [p for p, d in found if v in (d['variant'], d['crc'], SHORT.get(d['variant']))]
        if hit: return hit[0], None
        head = 'No game version "%s" in %s.' % (version, ', '.join(dirs))
    elif len(found) == 1:
        return found[0][0], None
    elif len([p for p, d in found if d['variant'] in SHORT]) == 1:
        # one known release plus unknown extras (the GOG installs ship a GameMp.exe next to Game.exe)
        return [p for p, d in found if d['variant'] in SHORT][0], None
    elif not found:
        return None, ('No Cultures game executable found in %s or its subfolders.\n'
                      'Point the build at one with EXE=/path/to/Game.exe or GAME_DIR=/path/to/game.' % ', '.join(dirs))
    else:
        head = 'Several game versions found; choose one with VERSION=...:'
    found.sort(key=lambda x: order(x[1]['variant']))
    names = [SHORT.get(d['variant'], d['variant']) for p, d in found]
    w1, w2 = max(map(len, names)), max(len(d['title']) for p, d in found)
    rows = ['  VERSION=%-*s  %-*s  %s' % (w1, n, w2, d['title'], os.path.relpath(p)) for n, (p, d) in zip(names, found)]
    return None, '\n'.join([head] + rows + ['or build them all with: make all-versions'])


def order(variant):
    known = [v for v, _ in KNOWN.values()]
    return (known.index(variant) if variant in known else len(known), variant)


if __name__ == '__main__':
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'name':
        print(describe(args[0])['variant'])
    elif cmd == 'json':
        print(json.dumps(describe(args[0]), ensure_ascii=False))
    elif cmd == 'scan':
        for p in scan(args):
            print(p)
    elif cmd == 'find':
        exe, msg = find(args[0], args[1:])
        if not exe: sys.exit(msg)
        print(exe)
    elif cmd == 'scan-unique':
        for p in scan_unique(args):
            print(p)
    else:
        sys.exit(__doc__)
