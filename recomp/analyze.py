"""Control-flow recovery for the x86 game binary.

Discovers functions (recursive descent seeded from the entry point, call targets,
code immediates and data pointers into .text), their basic blocks and switch
jump tables.  The result feeds the C emitter in translate.py.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG
from capstone import x86_const as X
from pe import Image

COND_JUMPS = {'je','jne','jb','jae','jbe','ja','jl','jge','jle','jg','js','jns','jo','jno','jp','jnp','jecxz','loop','loope','loopne'}

class Insn:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'op_str', 'raw', 'prefix', 'cs')
    def __init__(self, ci):
        self.addr = ci.address; self.size = ci.size; self.mn = ci.mnemonic
        self.op_str = ci.op_str; self.raw = bytes(ci.bytes)
        self.ops = list(ci.operands); self.prefix = list(ci.prefix); self.cs = ci

class Analyzer:
    def __init__(self, img, extra_entries=()):
        self.img = img
        self.md = Cs(CS_ARCH_X86, CS_MODE_32); self.md.detail = True
        self.insns = {}          # addr -> Insn
        self.funcs = {}          # entry -> Function dict
        self.jumptables = {}     # jmp insn addr -> [targets]
        self.bad = set()
        self.extra = list(extra_entries)
        self.noreturn = set()

    def decode(self, a):
        i = self.insns.get(a)
        if i is not None: return i
        if not self.img.in_text(a): return None
        try:
            ci = next(self.md.disasm(self.img.bytes(a, 16), a, 1))
        except StopIteration:
            return None
        i = Insn(ci); self.insns[a] = i
        return i

    def iat_target(self, op):
        if op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            return self.img.imports.get(op.mem.disp & 0xffffffff)
        return None

    def jump_table(self, ins):
        """jmp dword ptr [reg*4 + table]: collect plausible targets near the jump.

        MSVC switch tables start at index 0, but the CRT's hand-written memcpy
        indexes some tables from 1 or with negative indices, so scan a window in
        both directions and keep entries that point close to the jump.  The C
        emitter switches on the loaded address, so extra candidates are harmless."""
        op = ins.ops[0]
        if op.type != CS_OP_MEM or op.mem.base != 0 or op.mem.index == 0 or op.mem.scale != 4:
            return None
        tbl = op.mem.disp & 0xffffffff
        if not self.img.valid(tbl): return None
        def plausible(t):
            return self.img.in_text(t) and abs(t - ins.addr) < 0x4000
        targets = []
        # forward (allow one leading garbage slot for 1-based tables)
        miss = 0
        for k in range(0, 1024):
            a = tbl + 4 * k
            if not self.img.valid(a + 3): break
            t = self.img.u32(a)
            if plausible(t): targets.append(t); miss = 0
            else:
                miss += 1
                if k > 0 or miss > 1: break
        # backward (negative indices)
        for k in range(1, 64):
            a = tbl - 4 * k
            t = self.img.u32(a)
            if plausible(t): targets.append(t)
            else: break
        return list(dict.fromkeys(targets)) or None

    def explore(self, entry):
        """Collect the basic blocks of a function; returns dict with blocks and call targets."""
        blocks = {}   # start -> [insn addrs]
        succ = {}
        calls = set()
        work = [entry]; seen = set()
        while work:
            b = work.pop()
            if b in seen: continue
            seen.add(b)
            cur = b; body = []
            while True:
                ins = self.decode(cur)
                if ins is None:
                    self.bad.add(cur); succ[b] = []; break
                body.append(cur)
                mn = ins.mn; nxt = cur + ins.size
                if mn in ('ret', 'retf', 'hlt', 'int3', 'ud2', 'iretd'):
                    succ[b] = []; break
                if mn == 'jmp':
                    op = ins.ops[0]
                    if op.type == CS_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t in self.func_entries and t != entry:
                            succ[b] = []        # tail call
                        else:
                            succ[b] = [t]; work.append(t)
                    else:
                        jt = self.jump_table(ins)
                        if jt:
                            self.jumptables[cur] = jt
                            succ[b] = list(dict.fromkeys(jt)); work.extend(jt)
                        else:
                            succ[b] = []
                    break
                if mn in COND_JUMPS:
                    t = ins.ops[0].imm & 0xffffffff
                    succ[b] = [nxt, t]; work += [nxt, t]; break
                if mn == 'call':
                    op = ins.ops[0]
                    if op.type == CS_OP_IMM:
                        t = op.imm & 0xffffffff
                        calls.add(t)
                        if t in self.noreturn:
                            succ[b] = []; break
                    else:
                        imp = self.iat_target(op)
                        if imp and imp[1] in ('ExitProcess', 'RaiseException', 'TerminateProcess'):
                            succ[b] = []; break
                if nxt in self.block_starts_hint and nxt != b and nxt in seen:
                    succ[b] = [nxt]; break
                cur = nxt
            blocks[b] = body
        # split blocks that overlap (a jump into the middle of an existing block)
        starts = set(blocks)
        for s in list(succ.values()):
            starts.update(s)
        final = {}
        for b, body in blocks.items():
            cur = [] ; cs = b
            for a in body:
                if a in starts and cur:
                    final.setdefault(cs, cur); cur = []; cs = a
                cur.append(a)
            if cur: final.setdefault(cs, cur)
        return {'entry': entry, 'blocks': final, 'calls': calls}

    def seeds(self):
        img = self.img
        s = {img.entry}
        pe = img.pe
        if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                s.add(img.base + e.address)
        if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            # relocations mark every absolute address exactly
            for b in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in b.entries:
                    if e.type != 3: continue
                    a = img.base + e.rva
                    v = img.u32(a)
                    if img.in_text(v) and not img.in_text(a):
                        s.add(v)
            return s
        # data pointers into .text from non-code sections
        for name, a, b in img.sections:
            if name == '.text': continue
            for p in range(a, b - 3, 4):
                v = img.u32(p)
                if img.in_text(v): s.add(v)
        return s

    def run(self):
        img = self.img
        self.block_starts_hint = set()
        self.func_entries = set(self.seeds()) | set(self.extra)
        work = list(self.func_entries)
        done = set()
        while work:
            f = work.pop()
            if f in done: continue
            done.add(f)
            if self.decode(f) is None: continue
            fn = self.explore(f)
            self.funcs[f] = fn
            for c in fn['calls']:
                if img.in_text(c) and c not in self.func_entries:
                    self.func_entries.add(c); work.append(c)
            # immediates referencing code (callbacks, thread procs, wndproc)
            for body in fn['blocks'].values():
                for a in body:
                    ins = self.insns[a]
                    if ins.mn in ('push', 'mov', 'lea'):
                        for op in ins.ops:
                            if op.type == CS_OP_IMM:
                                v = op.imm & 0xffffffff
                                if img.in_text(v) and v not in self.func_entries and v not in self.all_jt_targets():
                                    self.func_entries.add(v); work.append(v)
        self.gap_funcs = self.fill_gaps()
        return self

    def covered_map(self):
        img = self.img
        cov = bytearray(img.text[1] - img.text[0])
        for f in self.funcs.values():
            for body in f['blocks'].values():
                for a in body:
                    i = self.insns[a]
                    o = a - img.text[0]
                    cov[o:o + i.size] = b'\x01' * i.size
        for j, ts in self.jumptables.items():
            pass
        return cov

    def looks_like_code(self, a):
        """Decodes cleanly for a few instructions and is not a table of code pointers."""
        if self.img.in_text(self.img.u32(a)): return False
        cur = a
        for _ in range(4):
            ins = self.decode(cur)
            if ins is None: return False
            if ins.mn in ('aaa', 'aas', 'daa', 'das', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'into', 'bound',
                          'les', 'lds', 'arpl', 'hlt', 'int1', 'sti', 'cli', 'iretd', 'retf', 'salc', '(bad)', 'int'):
                return False
            if ins.mn in ('ret', 'jmp'): return True
            cur += ins.size
        return True

    def fill_gaps(self):
        """Seed functions at the start of uncovered code so indirectly reached code
        (CRT helpers, code after no-return calls, ...) is recompiled too."""
        img = self.img
        added = 0
        for _round in range(20):
            cov = self.covered_map()
            new = []
            jt_bytes = set()
            for j, ts in self.jumptables.items():
                pass
            i, n = 0, len(cov)
            while i < n:
                if cov[i]: i += 1; continue
                j = i
                while j < n and not cov[j]: j += 1
                a = img.text[0] + i
                while a < img.text[0] + j and img.u8(a) in (0xcc, 0x90): a += 1
                # skip leading code-pointer tables
                while a + 4 <= img.text[0] + j and img.in_text(img.u32(a)): a += 4
                if a < img.text[0] + j and self.looks_like_code(a) and a not in self.funcs:
                    new.append(a)
                i = j
            if not new: break
            for a in new:
                self.func_entries.add(a)
                self.funcs[a] = self.explore(a)
                for c in self.funcs[a]['calls']:
                    if img.in_text(c) and c not in self.funcs:
                        self.func_entries.add(c); self.funcs[c] = self.explore(c)
            added += len(new)
        return added

    _jtt = None
    def all_jt_targets(self):
        return set(t for v in self.jumptables.values() for t in v)

if __name__ == '__main__':
    from collections import Counter
    img = Image(sys.argv[1])
    an = Analyzer(img).run()
    covered = set()
    for f in an.funcs.values():
        for body in f['blocks'].values():
            for a in body: covered.add(a)
    nbytes = sum(an.insns[a].size for a in covered)
    print('functions', len(an.funcs), 'insns', len(covered), 'bytes', nbytes, 'of', img.text[1]-img.text[0])
    print('jumptables', len(an.jumptables), 'bad', len(an.bad))
    c = Counter(an.insns[a].mn for a in covered)
    print(len(c), 'mnemonics')
    print(' '.join('%s:%d' % kv for kv in c.most_common()))
