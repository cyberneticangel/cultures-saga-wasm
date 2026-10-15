"""x86 -> C static recompiler.

Every recovered x86 function becomes one C function `f_XXXXXXXX(void)` operating on
the global CPU state `R` (registers are cached in C locals inside a function and
spilled around calls).  Guest memory is identity mapped: guest address == host
address (see runtime/recomp.h).

Flags are computed lazily: a backward liveness pass decides which flags are needed
and condition codes are emitted directly from the operands of the instruction that
produced them whenever possible (cmp/jcc -> `if (a < b)`).
"""
import os, sys, struct
from capstone import CS_OP_IMM, CS_OP_MEM, CS_OP_REG
from capstone import x86_const as X
from pe import Image
from analyze import Analyzer, COND_JUMPS
import regsum
from regsum import ALL, ESP, IMPORT_USE, IMPORT_DEF, CALLER_SAVED, regs_of

FLAGS = ('CF', 'ZF', 'SF', 'OF', 'PF')
ALLF = frozenset(FLAGS)

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SINT = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
MEMF = {1: 'M8', 2: 'M16', 4: 'M32', 8: 'M64'}

CC_FLAGS = {
    'o': {'OF'}, 'no': {'OF'}, 'b': {'CF'}, 'ae': {'CF'}, 'e': {'ZF'}, 'ne': {'ZF'},
    'be': {'CF', 'ZF'}, 'a': {'CF', 'ZF'}, 's': {'SF'}, 'ns': {'SF'}, 'p': {'PF'}, 'np': {'PF'},
    'l': {'SF', 'OF'}, 'ge': {'SF', 'OF'}, 'le': {'ZF', 'SF', 'OF'}, 'g': {'ZF', 'SF', 'OF'},
}
CC_ALIAS = {'z': 'e', 'nz': 'ne', 'c': 'b', 'nc': 'ae', 'nae': 'b', 'nb': 'ae', 'na': 'be', 'nbe': 'a',
            'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g', 'pe': 'p', 'po': 'np'}

def cc_of(mn, prefix):
    c = mn[len(prefix):]
    return CC_ALIAS.get(c, c)


def flag_effects(ins):
    """(reads, writes) flag sets for liveness."""
    mn = ins.mn
    if mn.startswith('lock '): mn = mn[5:]
    if mn.startswith('rep'):
        base = mn.split()[-1]
        if base.startswith(('cmps', 'scas')): return set(), set(ALLF)
        return set(), set()
    if mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'neg', 'shl', 'sal', 'shr', 'sar',
              'shld', 'shrd', 'imul', 'mul', 'div', 'idiv', 'cmpxchg', 'xadd', 'bsf', 'bsr',
              'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd', 'popfd', 'popf') and not (mn == 'cmpsd' and len(ins.ops) == 3):
        return set(), set(ALLF)
    if mn in ('adc', 'sbb'): return {'CF'}, set(ALLF)
    if mn in ('inc', 'dec'): return set(), {'ZF', 'SF', 'OF', 'PF'}
    if mn in ('rol', 'ror'): return set(), {'CF', 'OF'}
    if mn in ('rcl', 'rcr'): return {'CF'}, {'CF', 'OF'}
    if mn in ('clc', 'stc', 'bt', 'bts', 'btr', 'btc'): return set(), {'CF'}
    if mn == 'cmc': return {'CF'}, {'CF'}
    if mn == 'sahf': return set(), {'CF', 'ZF', 'SF', 'PF'}
    if mn in ('lahf',): return {'CF', 'ZF', 'SF', 'PF'}, set()
    if mn in ('pushfd', 'pushf'): return set(ALLF), set()
    if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip'): return set(), set(ALLF)
    if mn in COND_JUMPS and mn not in ('jecxz', 'loop'):
        if mn == 'loope' or mn == 'loopne': return {'ZF'}, set()
        return set(CC_FLAGS[cc_of(mn, 'j')]), set()
    if mn.startswith('set') and mn not in ('setalc',):
        return set(CC_FLAGS.get(cc_of(mn, 'set'), ())), set()
    if mn.startswith('cmov'):
        return set(CC_FLAGS.get(cc_of(mn, 'cmov'), ())), set()
    if mn.startswith('fcmov'):
        return set(ALLF), set()
    if mn == 'call': return set(), set(ALLF)
    return set(), set()


class Unsupported(Exception):
    pass


class Module:
    def __init__(self, img, an):
        self.img = img; self.an = an
        self.sums = regsum.Summaries(an).run()


class FuncEmitter:
    def __init__(self, tr, mod, fn):
        self.tr = tr; self.an = mod.an; self.img = mod.img; self.mod = mod
        self.fn = fn
        self.out = []
        self.tmp = 0
        self.sym = {}

    # ---------------------------------------------------------------- helpers
    def t(self, expr, ty='uint32_t'):
        self.tmp += 1
        n = 't%d' % self.tmp
        self.out.append('  %s %s = %s;' % (ty, n, expr))
        return n

    def e(self, s):
        self.out.append('  ' + s)

    def reg(self, ins, r):
        return ins.cs.reg_name(r)

    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            if self.reg(ins, m.base) not in REG32: raise Unsupported('addr16')
            parts.append(self.reg(ins, m.base))
        if m.index:
            ix = self.reg(ins, m.index)
            if ix not in REG32: raise Unsupported('addr16')
            parts.append(ix if m.scale == 1 else '%s*%d' % (ix, m.scale))
        disp = m.disp & 0xffffffff
        if disp or not parts:
            parts.append('0x%xu' % disp)
        a = ' + '.join(parts)
        if m.segment == X.X86_REG_FS:
            a = 'FS_BASE + ' + a
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
            raise Unsupported('segment')
        return '(uint32_t)(%s)' % a if len(parts) > 1 or m.segment == X.X86_REG_FS else a

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == CS_OP_REG:
            n = self.reg(ins, op.reg)
            if n in REG32: return n
            if n in R16: return '(%s & 0xffffu)' % R16[n]
            if n in R8L: return '(%s & 0xffu)' % R8L[n]
            if n in R8H: return '((%s >> 8) & 0xffu)' % R8H[n]
            raise Unsupported('reg ' + n)
        if op.type == CS_OP_IMM:
            return '0x%xu' % (op.imm & ((1 << (8 * size)) - 1))
        if op.type == CS_OP_MEM:
            if size == 4: return 'M32(%s)' % self.addr(ins, op)
            return '(uint32_t)%s(%s)' % (MEMF[size], self.addr(ins, op))
        raise Unsupported('op')

    def wr(self, ins, op, val, size=None):
        size = size or op.size
        if op.type == CS_OP_REG:
            n = self.reg(ins, op.reg)
            if n in REG32: self.e('%s = %s;' % (n, val)); return
            if n in R16: r = R16[n]; self.e('%s = (%s & 0xffff0000u) | ((%s) & 0xffffu);' % (r, r, val)); return
            if n in R8L: r = R8L[n]; self.e('%s = (%s & 0xffffff00u) | ((%s) & 0xffu);' % (r, r, val)); return
            if n in R8H: r = R8H[n]; self.e('%s = (%s & 0xffff00ffu) | (((%s) & 0xffu) << 8);' % (r, r, val)); return
            raise Unsupported('wreg ' + n)
        if op.type == CS_OP_MEM:
            self.e('%s(%s) = %s;' % (MEMF[size], self.addr(ins, op), val if size == 4 else '(%s)(%s)' % ({1: 'uint8_t', 2: 'uint16_t'}[size], val)))
            return
        raise Unsupported('wop')

    def memop(self, ins):
        for op in ins.ops:
            if op.type == CS_OP_MEM: return op
        return None

    # ------------------------------------------------------------- flags
    def set_arith(self, kind, a, b, r, n, extra=None):
        """Record symbolic flags after an ALU op.  a,b,r are temp names (zero-extended)."""
        hb = 8 * n - 1
        s = {}
        msk = MASK[n]
        if kind in ('add', 'adc'):
            if kind == 'add':
                s['CF'] = '(%s < %s)' % (r, a)
            else:
                s['CF'] = '((uint32_t)((((uint64_t)%s + %s + %s) >> %d) & 1))' % (a, b, extra, 8 * n)
            s['OF'] = '(((%s ^ %s) & (%s ^ %s)) >> %d & 1)' % (a, r, b, r, hb)
        elif kind in ('sub', 'sbb'):
            if kind == 'sub':
                s['CF'] = '(%s < %s)' % (a, b)
            else:
                s['CF'] = '((uint64_t)%s < (uint64_t)%s + %s)' % (a, b, extra)
            s['OF'] = '(((%s ^ %s) & (%s ^ %s)) >> %d & 1)' % (a, b, a, r, hb)
        elif kind == 'logic':
            s['CF'] = '0'; s['OF'] = '0'
        elif kind == 'neg':
            s['CF'] = '(%s != 0)' % a
            s['OF'] = '(%s == 0x%xu)' % (a, 1 << hb)
        s['ZF'] = '(%s == 0)' % r
        s['SF'] = '(%s >> %d & 1)' % (r, hb)
        s['PF'] = 'PARITY(%s)' % r
        info = (kind, a, b, r, n)
        for k, v in s.items():
            self.sym[k] = (v, info)

    def set_flags(self, d, info=None):
        for k, v in d.items():
            self.sym[k] = (v, info)

    def flag(self, f):
        if f in self.sym: return self.sym[f][0]
        return f

    def cond(self, cc):
        need = CC_FLAGS[cc]
        infos = set(self.sym[f][1] for f in need if f in self.sym)
        if len(infos) == 1 and all(f in self.sym for f in need):
            info = next(iter(infos))
            if info is not None:
                kind, a, b, r, n = info
                st = SINT[n]
                if kind == 'sub':
                    rel = {'b': '%s < %s', 'ae': '%s >= %s', 'e': '%s == %s', 'ne': '%s != %s',
                           'be': '%s <= %s', 'a': '%s > %s'}
                    srel = {'l': '<', 'ge': '>=', 'le': '<=', 'g': '>'}
                    if cc in rel: return '(' + rel[cc] % (a, b) + ')'
                    if cc in srel: return '((%s)%s %s (%s)%s)' % (st, a, srel[cc], st, b)
                if kind == 'logic':
                    m = {'e': '%s == 0', 'ne': '%s != 0', 'b': '0', 'ae': '1', 'be': '%s == 0', 'a': '%s != 0',
                         'l': '(%s)%%s < 0' % st, 'ge': '(%s)%%s >= 0' % st, 'le': '(%s)%%s <= 0' % st,
                         'g': '(%s)%%s > 0' % st, 's': '(%s)%%s < 0' % st, 'ns': '(%s)%%s >= 0' % st}
                    if cc in m: return '(' + (m[cc] % r if '%s' in m[cc] else m[cc]) + ')'
        F = self.flag
        return {
            'o': '%s' % F('OF'), 'no': '!%s' % F('OF'), 'b': '%s' % F('CF'), 'ae': '!%s' % F('CF'),
            'e': '%s' % F('ZF'), 'ne': '!%s' % F('ZF'), 'be': '(%s | %s)' % (F('CF'), F('ZF')),
            'a': '!(%s | %s)' % (F('CF'), F('ZF')), 's': '%s' % F('SF'), 'ns': '!%s' % F('SF'),
            'p': '%s' % F('PF'), 'np': '!%s' % F('PF'),
            'l': '(%s != %s)' % (F('SF'), F('OF')), 'ge': '(%s == %s)' % (F('SF'), F('OF')),
            'le': '(%s | (%s != %s))' % (F('ZF'), F('SF'), F('OF')),
            'g': '(!%s & (%s == %s))' % (F('ZF'), F('SF'), F('OF')),
        }[cc]

    def materialize(self, live):
        for f in FLAGS:
            if f in live and f in self.sym:
                v = self.sym[f][0]
                if v != f:
                    self.e('%s = %s;' % (f, v))

    # ------------------------------------------------------------- calls
    def spill(self, mask):
        return ' '.join('R.%s = %s;' % (r, r) for r in regs_of(mask))

    def reload(self, mask):
        return ' '.join('%s = R.%s;' % (r, r) for r in regs_of(mask))

    @property
    def my_def(self):
        return self.mod.sums.defs[self.fn['entry']] | ESP

    def call_direct(self, target, ret):
        an = self.an
        sm = self.mod.sums
        if target in an.funcs:
            self.e('PUSH(0x%xu); %s f_%08x(); %s' % (ret, self.spill(sm.use[target] | ESP), target, self.reload(sm.defs[target] | ESP)))
        else:
            self.e('PUSH(0x%xu); %s dispatch(0x%xu); %s' % (ret, self.spill(ALL), target, self.reload(CALLER_SAVED)))
        self.sym = {}

    def call_import(self, imp, ret):
        dll, name = imp
        self.tr.used_imports.add(imp)
        self.e('PUSH(0x%xu); %s %s(); %s' % (ret, self.spill(IMPORT_USE), imp_name(dll, name), self.reload(IMPORT_DEF)))
        self.sym = {}

    # ------------------------------------------------------------- main
    def emit(self):
        fn = self.fn
        entry = fn['entry']
        blocks = fn['blocks']
        an = self.an
        # liveness
        succ = {}
        for b, body in blocks.items():
            succ[b] = self.block_succ(b, body)
        live_in = {b: set() for b in blocks}
        uses_defs = {}
        for b, body in blocks.items():
            seq = [flag_effects(an.insns[a]) for a in body]
            uses_defs[b] = seq
        changed = True
        live_out = {b: set() for b in blocks}
        while changed:
            changed = False
            for b in sorted(blocks, reverse=True):
                lo = set()
                for s in succ[b]:
                    if s in live_in: lo |= live_in[s]
                live = set(lo)
                for r, w in reversed(uses_defs[b]):
                    live -= w; live |= r
                if live != live_in[b] or lo != live_out[b]:
                    live_in[b] = live; live_out[b] = lo; changed = True
        self.live_out = live_out
        name = 'f_%08x' % entry
        use = self.mod.sums.use[entry] | ESP
        hdr = ['void %s(void) {' % name,
               '  uint32_t ' + ', '.join(('%s = R.%s' % (r, r)) if use & regsum.BIT[r] else ('%s = 0' % r) for r in regsum.REGS) + ';',
               '  FLAGS();']
        order = sorted(blocks)
        if order[0] != entry:
            hdr.append('  goto L_%x;' % entry)
        if live_in.get(entry):
            pass  # flags live into function: read uninitialised locals (harmless, logged)
        self.out = []
        for i, b in enumerate(order):
            self.sym = {}
            self.out.append('L_%x:;' % b)
            body = blocks[b]
            for j, a in enumerate(body):
                ins = an.insns[a]
                last = (j == len(body) - 1)
                self.out.append('  /* %08x: %s %s */' % (a, ins.mn, ins.op_str))
                try:
                    self.insn(ins, b, last, order[i + 1] if i + 1 < len(order) else None)
                except Unsupported as ex:
                    self.tr.unsupported[str(ex) + ' ' + ins.mn] = self.tr.unsupported.get(str(ex) + ' ' + ins.mn, 0) + 1
                    self.e('UNIMPL(0x%xu);' % a)
            if body:
                ins = an.insns[body[-1]]
                if not self.is_terminator(ins):
                    nxt = body[-1] + ins.size
                    self.materialize(live_out[b])
                    if nxt in blocks:
                        self.e('goto L_%x;' % nxt)
                    else:
                        self.e('UNIMPL(0x%xu);' % nxt)
            else:
                self.e('UNIMPL(0x%xu);' % b)
        self.out.append('}')
        return hdr + self.out

    def is_terminator(self, ins):
        mn = ins.mn
        if mn in ('ret', 'jmp', 'hlt', 'int3', 'ud2', 'retf', 'iretd') or mn in COND_JUMPS: return True
        if mn == 'call':
            op = ins.ops[0]
            if op.type == CS_OP_IMM and (op.imm & 0xffffffff) in self.an.noreturn: return True
            imp = self.an.iat_target(op)
            if imp and imp[1] in ('ExitProcess', 'RaiseException', 'TerminateProcess'): return True
        return False

    def block_succ(self, b, body):
        if not body: return []
        ins = self.an.insns[body[-1]]
        nxt = ins.addr + ins.size
        mn = ins.mn
        if mn in COND_JUMPS: return [nxt, ins.ops[0].imm & 0xffffffff]
        if mn == 'jmp':
            if ins.addr in self.an.jumptables: return list(set(self.an.jumptables[ins.addr]))
            op = ins.ops[0]
            if op.type == CS_OP_IMM:
                t = op.imm & 0xffffffff
                if t in self.fn['blocks']: return [t]
            return []
        if self.is_terminator(ins): return []
        return [nxt]

    def goto(self, t):
        if t in self.fn['blocks']:
            return 'goto L_%x;' % t
        sm = self.mod.sums
        if t in self.an.funcs:
            return '{ %s f_%08x(); return; }' % (self.spill(sm.use[t] | self.my_def | ESP), t)
        return '{ %s dispatch(0x%xu); return; }' % (self.spill(ALL), t)

    def insn(self, ins, blk, last, nextblk):
        mn = ins.mn; ops = ins.ops
        nxt = ins.addr + ins.size
        e = self.e
        if mn.startswith('lock '):
            mn = mn[5:]
        live_after = None
        if last:
            pass
        # ---- control flow
        if mn == 'jmp':
            op = ops[0]
            self.materialize(self.live_out[blk])
            if op.type == CS_OP_IMM:
                e(self.goto(op.imm & 0xffffffff)); return
            if ins.addr in self.an.jumptables:
                e('{ uint32_t tgt = %s;' % self.rd(ins, op))
                e('  switch (tgt) {')
                for t in self.an.jumptables[ins.addr]:
                    if t in self.fn['blocks']:
                        e('    case 0x%xu: goto L_%x;' % (t, t))
                e('  }')
                e('  table_miss(0x%xu, tgt); %s dispatch(tgt); return; }' % (ins.addr, self.spill(ALL)))
                return
            imp = self.an.iat_target(op)
            if imp:
                self.tr.used_imports.add(imp)
                e('%s %s(); return;' % (self.spill(IMPORT_USE | self.my_def), imp_name(*imp))); return
            tgt = self.rd(ins, op)
            e('{ uint32_t tgt = %s; %s dispatch(tgt); return; }' % (tgt, self.spill(ALL)))
            return
        if mn in COND_JUMPS:
            t = ops[0].imm & 0xffffffff
            if mn == 'jecxz': c = '(ecx == 0)'
            elif mn == 'loop': e('ecx--;'); c = '(ecx != 0)'
            elif mn == 'loope': e('ecx--;'); c = '(ecx != 0 && %s)' % self.flag('ZF')
            elif mn == 'loopne': e('ecx--;'); c = '(ecx != 0 && !%s)' % self.flag('ZF')
            else: c = self.cond(cc_of(mn, 'j'))
            self.materialize(self.live_out[blk])
            e('if (%s) %s' % (c, self.goto(t)))
            e(self.goto(nxt))
            return
        if mn == 'call':
            op = ops[0]
            if op.type == CS_OP_IMM:
                self.call_direct(op.imm & 0xffffffff, nxt)
            else:
                imp = self.an.iat_target(op)
                if imp:
                    self.call_import(imp, nxt)
                else:
                    tgt = self.t(self.rd(ins, op))
                    e('PUSH(0x%xu); %s dispatch(%s); %s' % (nxt, self.spill(ALL), tgt, self.reload(CALLER_SAVED)))
                    self.sym = {}
            if self.is_terminator(ins):
                e('UNIMPL(0x%xu); /* noreturn */' % nxt)
            return
        if mn in ('ret', 'retf'):
            n = ops[0].imm if ops else 0
            e('esp += %d; %s return;' % (4 + n, self.spill(self.my_def)))
            return
        if mn in ('int3', 'hlt', 'ud2', 'iretd'):
            e('UNIMPL(0x%xu);' % ins.addr); return
        # ---- data movement
        if mn == 'mov':
            d, s = ops
            if d.type == CS_OP_REG and self.reg(ins, d.reg) in ('cs','ds','es','ss','fs','gs'): return
            if s.type == CS_OP_REG and self.reg(ins, s.reg) in ('cs','ds','es','ss','fs','gs'):
                self.wr(ins, d, '0x%xu' % (0x3b if self.reg(ins, s.reg) == 'fs' else 0x23)); return
            self.wr(ins, d, self.rd(ins, s, d.size)); return
        if mn in ('movzx', 'movsx'):
            d, s = ops
            v = self.rd(ins, s)
            if mn == 'movsx':
                v = '(uint32_t)(%s)(%s)(%s)' % (SINT[d.size], SINT[s.size], v)
            self.wr(ins, d, v); return
        if mn == 'lea':
            d, s = ops
            m = s.mem
            a = self.addr(ins, s).replace('FS_BASE + ', '')
            self.wr(ins, d, a); return
        if mn == 'push':
            op = ops[0]
            if op.type == CS_OP_IMM:
                v = '0x%xu' % (op.imm & 0xffffffff)
                if op.size == 2: e('PUSH16(%s);' % v); return
            elif op.type == CS_OP_REG and self.reg(ins, op.reg) in ('cs','ds','es','ss','fs','gs'):
                v = '0x23u'
            else:
                v = self.rd(ins, op)
                if op.size == 2: e('PUSH16(%s);' % v); return
            if v == 'esp':
                e('{ uint32_t ov = esp; PUSH(ov); }')
            else:
                e('{ uint32_t pv = %s; PUSH(pv); }' % v)
            return
        if mn == 'pop':
            op = ops[0]
            if op.type == CS_OP_REG and self.reg(ins, op.reg) in ('cs','ds','es','ss','fs','gs'):
                e('esp += 4;'); return
            if op.size == 2:
                e('{ uint32_t pv = M16(esp); esp += 2;'); self.wr(ins, op, 'pv'); e('}'); return
            e('{ uint32_t pv = M32(esp); esp += 4;'); self.wr(ins, op, 'pv'); e('}'); return
        if mn == 'xchg':
            a, b = ops
            va = self.t(self.rd(ins, a)); vb = self.t(self.rd(ins, b))
            self.wr(ins, a, vb); self.wr(ins, b, va); return
        if mn == 'leave':
            e('esp = ebp; ebp = M32(esp); esp += 4;'); return
        if mn == 'enter':
            sz = ops[0].imm; lvl = ops[1].imm
            if lvl != 0: raise Unsupported('enter level')
            e('PUSH(ebp); ebp = esp; esp -= %d;' % sz); return
        if mn == 'pushal':
            e('{ uint32_t os = esp; PUSH(eax); PUSH(ecx); PUSH(edx); PUSH(ebx); PUSH(os); PUSH(ebp); PUSH(esi); PUSH(edi); }'); return
        if mn == 'popal':
            e('edi = M32(esp); esi = M32(esp+4); ebp = M32(esp+8); ebx = M32(esp+16); edx = M32(esp+20); ecx = M32(esp+24); eax = M32(esp+28); esp += 32;'); return
        if mn in ('pushfd',):
            F = self.flag
            e('PUSH(R.eflags_hi | (%s) | ((%s) << 2) | ((%s) << 6) | ((%s) << 7) | ((%s) << 11) | (R.df << 10) | 2u);' % (F('CF'), F('PF'), F('ZF'), F('SF'), F('OF')))
            return
        if mn in ('popfd',):
            e('{ uint32_t fl = M32(esp); esp += 4; CF = fl & 1; PF = (fl >> 2) & 1; ZF = (fl >> 6) & 1; SF = (fl >> 7) & 1; OF = (fl >> 11) & 1; R.df = (fl >> 10) & 1; R.eflags_hi = fl & 0x00200000u; }')
            self.sym = {}
            return
        if mn == 'cdq': e('edx = (uint32_t)((int32_t)eax >> 31);'); return
        if mn == 'cwde': e('eax = (uint32_t)(int32_t)(int16_t)eax;'); return
        if mn == 'cbw': e('eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);'); return
        if mn == 'cwd': e('edx = (edx & 0xffff0000u) | (((int16_t)eax < 0) ? 0xffffu : 0u);'); return
        if mn == 'nop' or mn == 'wait' or mn == 'fwait': return
        if mn == 'cld': e('R.df = 0;'); return
        if mn == 'std': e('R.df = 1;'); return
        if mn == 'clc': self.set_flags({'CF': '0'}); return
        if mn == 'stc': self.set_flags({'CF': '1'}); return
        if mn == 'cmc': self.set_flags({'CF': '(!%s)' % self.flag('CF')}); return
        if mn == 'sahf':
            ah = self.t('((eax >> 8) & 0xffu)')
            self.set_flags({'CF': '(%s & 1)' % ah, 'PF': '((%s >> 2) & 1)' % ah, 'ZF': '((%s >> 6) & 1)' % ah, 'SF': '((%s >> 7) & 1)' % ah})
            return
        if mn == 'lahf':
            F = self.flag
            e('eax = (eax & 0xffff00ffu) | ((uint32_t)((%s) | ((%s) << 2) | ((%s) << 6) | ((%s) << 7) | 2u) << 8);' % (F('CF'), F('PF'), F('ZF'), F('SF')))
            return
        if mn == 'xlatb':
            e('eax = (eax & 0xffffff00u) | M8(ebx + (eax & 0xffu));'); return
        if mn == 'cpuid':
            e('SPILL(); hle_cpuid(); RELOAD();'); return
        if mn == 'rdtsc':
            e('SPILL(); hle_rdtsc(); RELOAD();'); return
        if mn.startswith('set'):
            cc = cc_of(mn, 'set')
            self.wr(ins, ops[0], '(uint32_t)(%s)' % self.cond(cc)); return
        if mn.startswith('cmov'):
            cc = cc_of(mn, 'cmov')
            e('if (%s) {' % self.cond(cc)); self.wr(ins, ops[0], self.rd(ins, ops[1])); e('}'); return
        # ---- arithmetic
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test', 'adc', 'sbb'):
            d, s = ops
            n = d.size
            msk = MASK[n]
            if mn == 'xor' and d.type == CS_OP_REG and s.type == CS_OP_REG and d.reg == s.reg:
                self.wr(ins, d, '0u')
                self.set_arith('logic', '0u', '0u', '0u', n)
                return
            if d.type == CS_OP_MEM:
                ad = self.t(self.addr(ins, d))
                rdd = 'M32(%s)' % ad if n == 4 else '(uint32_t)%s(%s)' % (MEMF[n], ad)
                a = self.t(rdd)
            else:
                ad = None
                a = self.t(self.rd(ins, d))
            if s.type == CS_OP_IMM:
                b = '0x%xu' % (s.imm & ((1 << (8 * n)) - 1))
            else:
                b = self.t(self.rd(ins, s))
            extra = None
            if mn in ('add',): r = self.t('(%s + %s)%s' % (a, b, '' if n == 4 else ' & ' + msk))
            elif mn in ('sub', 'cmp'): r = self.t('(%s - %s)%s' % (a, b, '' if n == 4 else ' & ' + msk))
            elif mn in ('and', 'test'): r = self.t('%s & %s' % (a, b))
            elif mn == 'or': r = self.t('%s | %s' % (a, b))
            elif mn == 'xor': r = self.t('%s ^ %s' % (a, b))
            elif mn == 'adc':
                extra = self.t('(uint32_t)(%s)' % self.flag('CF'))
                r = self.t('(%s + %s + %s)%s' % (a, b, extra, '' if n == 4 else ' & ' + msk))
            elif mn == 'sbb':
                extra = self.t('(uint32_t)(%s)' % self.flag('CF'))
                r = self.t('(%s - %s - %s)%s' % (a, b, extra, '' if n == 4 else ' & ' + msk))
            if mn not in ('cmp', 'test'):
                if ad is not None:
                    e('%s(%s) = %s;' % (MEMF[n], ad, r if n == 4 else '(%s)%s' % ({1: 'uint8_t', 2: 'uint16_t'}[n], r)))
                else:
                    self.wr(ins, d, r)
            kind = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'and': 'logic', 'test': 'logic', 'or': 'logic',
                    'xor': 'logic', 'adc': 'adc', 'sbb': 'sbb'}[mn]
            self.set_arith(kind, a, b, r, n, extra)
            return
        if mn in ('inc', 'dec', 'neg', 'not'):
            d = ops[0]; n = d.size; msk = MASK[n]
            if d.type == CS_OP_MEM:
                ad = self.t(self.addr(ins, d))
                a = self.t('M32(%s)' % ad if n == 4 else '(uint32_t)%s(%s)' % (MEMF[n], ad))
            else:
                ad = None; a = self.t(self.rd(ins, d))
            if mn == 'inc': r = self.t('(%s + 1)%s' % (a, '' if n == 4 else ' & ' + msk))
            elif mn == 'dec': r = self.t('(%s - 1)%s' % (a, '' if n == 4 else ' & ' + msk))
            elif mn == 'neg': r = self.t('(0u - %s)%s' % (a, '' if n == 4 else ' & ' + msk))
            else: r = self.t('(~%s)%s' % (a, '' if n == 4 else ' & ' + msk))
            if ad is not None:
                e('%s(%s) = %s;' % (MEMF[n], ad, r if n == 4 else '(%s)%s' % ({1: 'uint8_t', 2: 'uint16_t'}[n], r)))
            else:
                self.wr(ins, d, r)
            hb = 8 * n - 1
            if mn == 'inc':
                self.set_flags({'ZF': '(%s == 0)' % r, 'SF': '(%s >> %d & 1)' % (r, hb), 'PF': 'PARITY(%s)' % r,
                                'OF': '(%s == 0x%xu)' % (r, 1 << hb)})
            elif mn == 'dec':
                self.set_flags({'ZF': '(%s == 0)' % r, 'SF': '(%s >> %d & 1)' % (r, hb), 'PF': 'PARITY(%s)' % r,
                                'OF': '(%s == 0x%xu)' % (r, (1 << hb) - 1)})
            elif mn == 'neg':
                self.set_arith('neg', a, '0u', r, n)
            return
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            d = ops[0]; n = d.size; bits = 8 * n; msk = MASK[n]
            if len(ops) > 1:
                c = ops[1]
                if c.type == CS_OP_IMM:
                    cnt = c.imm & 31
                    if cnt == 0: return
                    cs = '%du' % cnt
                else:
                    cnt = None
                    cs = self.t('(ecx & 31u)')
            else:
                cnt = 1; cs = '1u'
            if d.type == CS_OP_MEM:
                ad = self.t(self.addr(ins, d))
                a = self.t('M32(%s)' % ad if n == 4 else '(uint32_t)%s(%s)' % (MEMF[n], ad))
            else:
                ad = None; a = self.t(self.rd(ins, d))
            hb = bits - 1
            if cnt is None and mn in ('rcl', 'rcr', 'rol', 'ror'):
                pass
            if mn in ('shl', 'sal'):
                r = self.t('(%s << %s)%s' % (a, cs, '' if n == 4 else ' & ' + msk))
                cf = '((%s >> (%d - %s)) & 1)' % (a, bits, cs) if n == 4 or True else ''
                if n == 4: cf = '((%s >> (32 - %s)) & 1)' % (a, cs) if cnt != 1 else '(%s >> 31)' % a
                flags = {'CF': cf, 'ZF': '(%s == 0)' % r, 'SF': '(%s >> %d & 1)' % (r, hb), 'PF': 'PARITY(%s)' % r,
                         'OF': '((%s >> %d & 1) ^ %s)' % (r, hb, cf)}
            elif mn == 'shr':
                r = self.t('%s >> %s' % (a, cs))
                flags = {'CF': '((%s >> (%s - 1)) & 1)' % (a, cs), 'ZF': '(%s == 0)' % r, 'SF': '(%s >> %d & 1)' % (r, hb),
                         'PF': 'PARITY(%s)' % r, 'OF': '(%s >> %d & 1)' % (a, hb)}
            elif mn == 'sar':
                r = self.t('(uint32_t)((int32_t)(%s)%s >> %s)%s' % (SINT[n], a, cs, '' if n == 4 else ' & ' + msk))
                flags = {'CF': '((uint32_t)((int32_t)(%s)%s >> (%s - 1)) & 1)' % (SINT[n], a, cs), 'ZF': '(%s == 0)' % r,
                         'SF': '(%s >> %d & 1)' % (r, hb), 'PF': 'PARITY(%s)' % r, 'OF': '0'}
            elif mn in ('rol', 'ror'):
                cm = self.t('(%s %% %d)' % (cs, bits)) if n != 4 else self.t('(%s & 31)' % cs)
                if mn == 'rol':
                    r = self.t('(%s == 0 ? %s : ((%s << %s) | (%s >> (%d - %s)))%s)' % (cm, a, a, cm, a, bits, cm, '' if n == 4 else ' & ' + msk))
                    flags = {'CF': '(%s & 1)' % r, 'OF': '((%s >> %d & 1) ^ (%s & 1))' % (r, hb, r)}
                else:
                    r = self.t('(%s == 0 ? %s : ((%s >> %s) | (%s << (%d - %s)))%s)' % (cm, a, a, cm, a, bits, cm, '' if n == 4 else ' & ' + msk))
                    flags = {'CF': '(%s >> %d & 1)' % (r, hb), 'OF': '((%s >> %d ^ %s >> %d) & 1)' % (r, hb, r, hb - 1)}
            else:  # rcl / rcr through carry
                cin = self.t('(uint32_t)(%s)' % self.flag('CF'))
                cfo = self.t('0u')
                r = self.t('%s' % a)
                e('for (uint32_t k = %s %% %d; k; k--) {' % (cs, bits + 1))
                if mn == 'rcl':
                    e('  %s = (%s >> %d) & 1; %s = ((%s << 1) | %s)%s; %s = %s;' % (cfo, r, hb, r, r, cin, '' if n == 4 else ' & ' + msk, cin, cfo))
                else:
                    e('  %s = %s & 1; %s = (%s >> 1) | (%s << %d); %s = %s;' % (cfo, r, r, r, cin, hb, cin, cfo))
                e('}')
                flags = {'CF': cin, 'OF': '0'}
            if ad is not None:
                e('%s(%s) = %s;' % (MEMF[n], ad, r if n == 4 else '(%s)%s' % ({1: 'uint8_t', 2: 'uint16_t'}[n], r)))
            else:
                self.wr(ins, d, r)
            self.set_flags(flags)
            return
        if mn in ('shld', 'shrd'):
            d, s, c = ops
            if d.size != 4: raise Unsupported('shld16')
            cs = '%du' % (c.imm & 31) if c.type == CS_OP_IMM else self.t('(ecx & 31u)')
            a = self.t(self.rd(ins, d)); b = self.t(self.rd(ins, s))
            if mn == 'shld':
                r = self.t('(%s == 0) ? %s : ((%s << %s) | (%s >> (32 - %s)))' % (cs, a, a, cs, b, cs))
            else:
                r = self.t('(%s == 0) ? %s : ((%s >> %s) | (%s << (32 - %s)))' % (cs, a, a, cs, b, cs))
            self.wr(ins, d, r)
            self.set_flags({'ZF': '(%s == 0)' % r, 'SF': '(%s >> 31)' % r, 'CF': '0', 'OF': '0', 'PF': 'PARITY(%s)' % r})
            return
        if mn in ('imul', 'mul'):
            if len(ops) == 1:
                s = ops[0]; n = s.size
                b = self.t(self.rd(ins, s))
                if n == 4:
                    if mn == 'imul':
                        p = self.t('(int64_t)(int32_t)eax * (int64_t)(int32_t)%s' % b, 'int64_t')
                        e('eax = (uint32_t)%s; edx = (uint32_t)((uint64_t)%s >> 32);' % (p, p))
                        ov = '(%s != (int64_t)(int32_t)%s)' % (p, p)
                    else:
                        p = self.t('(uint64_t)eax * (uint64_t)%s' % b, 'uint64_t')
                        e('eax = (uint32_t)%s; edx = (uint32_t)(%s >> 32);' % (p, p))
                        ov = '(edx != 0)'
                elif n == 2:
                    if mn == 'imul':
                        p = self.t('(int32_t)(int16_t)eax * (int32_t)(int16_t)%s' % b, 'int32_t')
                        ov = '(%s != (int32_t)(int16_t)%s)' % (p, p)
                    else:
                        p = self.t('(eax & 0xffffu) * %s' % b)
                        ov = '((%s >> 16) != 0)' % p
                    e('eax = (eax & 0xffff0000u) | ((uint32_t)%s & 0xffffu); edx = (edx & 0xffff0000u) | (((uint32_t)%s >> 16) & 0xffffu);' % (p, p))
                else:
                    if mn == 'imul':
                        p = self.t('(int32_t)(int8_t)eax * (int32_t)(int8_t)%s' % b, 'int32_t')
                        ov = '(%s != (int32_t)(int8_t)%s)' % (p, p)
                    else:
                        p = self.t('(eax & 0xffu) * %s' % b)
                        ov = '((%s >> 8) != 0)' % p
                    e('eax = (eax & 0xffff0000u) | ((uint32_t)%s & 0xffffu);' % p)
                self.set_flags({'CF': ov, 'OF': ov, 'ZF': '0', 'SF': '0', 'PF': '0'})
                return
            if len(ops) == 2:
                d, s = ops; a = self.rd(ins, d); b = self.rd(ins, s)
            else:
                d, s, c = ops; a = self.rd(ins, s); b = '0x%xu' % (c.imm & 0xffffffff)
            n = d.size
            if n == 4:
                p = self.t('(int64_t)(int32_t)%s * (int64_t)(int32_t)%s' % (a, b), 'int64_t')
                ov = '(%s != (int64_t)(int32_t)%s)' % (p, p)
            else:
                p = self.t('(int32_t)(int16_t)%s * (int32_t)(int16_t)%s' % (a, b), 'int32_t')
                ov = '(%s != (int32_t)(int16_t)%s)' % (p, p)
            self.wr(ins, d, '(uint32_t)%s' % p)
            self.set_flags({'CF': ov, 'OF': ov, 'ZF': '0', 'SF': '0', 'PF': '0'})
            return
        if mn in ('div', 'idiv'):
            s = ops[0]; n = s.size
            b = self.t(self.rd(ins, s))
            e('if (%s == 0) DIVZERO(0x%xu);' % (b, ins.addr))
            if n == 4:
                if mn == 'div':
                    nm = self.t('((uint64_t)edx << 32) | eax', 'uint64_t')
                    e('eax = (uint32_t)(%s / %s); edx = (uint32_t)(%s %% %s);' % (nm, b, nm, b))
                else:
                    nm = self.t('(int64_t)(((uint64_t)edx << 32) | eax)', 'int64_t')
                    e('eax = (uint32_t)(%s / (int32_t)%s); edx = (uint32_t)(%s %% (int32_t)%s);' % (nm, b, nm, b))
            elif n == 2:
                if mn == 'div':
                    nm = self.t('((edx & 0xffffu) << 16) | (eax & 0xffffu)')
                    q = self.t('%s / %s' % (nm, b)); rm = self.t('%s %% %s' % (nm, b))
                else:
                    nm = self.t('(int32_t)(((edx & 0xffffu) << 16) | (eax & 0xffffu))', 'int32_t')
                    q = self.t('(uint32_t)(%s / (int16_t)%s)' % (nm, b)); rm = self.t('(uint32_t)(%s %% (int16_t)%s)' % (nm, b))
                e('eax = (eax & 0xffff0000u) | (%s & 0xffffu); edx = (edx & 0xffff0000u) | (%s & 0xffffu);' % (q, rm))
            else:
                if mn == 'div':
                    nm = self.t('eax & 0xffffu')
                    q = self.t('%s / %s' % (nm, b)); rm = self.t('%s %% %s' % (nm, b))
                else:
                    nm = self.t('(int32_t)(int16_t)eax', 'int32_t')
                    q = self.t('(uint32_t)(%s / (int8_t)%s)' % (nm, b)); rm = self.t('(uint32_t)(%s %% (int8_t)%s)' % (nm, b))
                e('eax = (eax & 0xffff0000u) | (%s & 0xffu) | ((%s & 0xffu) << 8);' % (q, rm))
            self.set_flags({f: '0' for f in FLAGS})
            return
        if mn in ('bt', 'bts', 'btr', 'btc'):
            d, s = ops
            if d.type == CS_OP_MEM and s.type != CS_OP_IMM:
                # bit string: byte address = ea + (bit >> 3) (signed), bit = bit & 7
                bit = self.t(self.rd(ins, s))
                ad = self.t('(uint32_t)(%s + (uint32_t)((int32_t)%s >> 3))' % (self.addr(ins, d), bit))
                b8 = self.t('(%s & 7u)' % bit)
                v = self.t('(uint32_t)M8(%s)' % ad)
                self.set_flags({'CF': '((%s >> %s) & 1)' % (v, b8)})
                if mn == 'bts': self.e('M8(%s) = (uint8_t)(%s | (1u << %s));' % (ad, v, b8))
                elif mn == 'btr': self.e('M8(%s) = (uint8_t)(%s & ~(1u << %s));' % (ad, v, b8))
                elif mn == 'btc': self.e('M8(%s) = (uint8_t)(%s ^ (1u << %s));' % (ad, v, b8))
                return
            a = self.t(self.rd(ins, d)); b = self.t('(%s & %d)' % (self.rd(ins, s), 8 * d.size - 1))
            self.set_flags({'CF': '((%s >> %s) & 1)' % (a, b)})
            if mn == 'bts': self.wr(ins, d, '%s | (1u << %s)' % (a, b))
            elif mn == 'btr': self.wr(ins, d, '%s & ~(1u << %s)' % (a, b))
            elif mn == 'btc': self.wr(ins, d, '%s ^ (1u << %s)' % (a, b))
            return
        if mn in ('bsf', 'bsr'):
            d, s = ops
            v = self.t(self.rd(ins, s))
            e('if (%s) {' % v)
            self.wr(ins, d, '(uint32_t)__builtin_ctz(%s)' % v if mn == 'bsf' else '(uint32_t)(31 - __builtin_clz(%s))' % v)
            e('}')
            self.set_flags({'ZF': '(%s == 0)' % v})
            return
        if mn == 'bswap':
            d = ops[0]; self.wr(ins, d, '__builtin_bswap32(%s)' % self.rd(ins, d)); return
        if mn == 'xadd':
            d, s = ops
            a = self.t(self.rd(ins, d)); b = self.t(self.rd(ins, s))
            r = self.t('(%s + %s)%s' % (a, b, '' if d.size == 4 else ' & ' + MASK[d.size]))
            self.wr(ins, s, a); self.wr(ins, d, r)
            self.set_arith('add', a, b, r, d.size)
            return
        if mn == 'cmpxchg':
            d, s = ops; n = d.size
            acc = {4: 'eax', 2: '(eax & 0xffffu)', 1: '(eax & 0xffu)'}[n]
            a = self.t(acc); b = self.t(self.rd(ins, d))
            r = self.t('(%s - %s)%s' % (a, b, '' if n == 4 else ' & ' + MASK[n]))
            e('if (%s == %s) {' % (a, b)); self.wr(ins, d, self.rd(ins, s)); e('} else {')
            if n == 4: e('eax = %s;' % b)
            else: self.wr(ins, type('o', (), {'type': CS_OP_REG, 'reg': X.X86_REG_AX if n == 2 else X.X86_REG_AL, 'size': n})(), b)
            e('}')
            self.set_arith('sub', a, b, r, n)
            return
        # ---- string ops
        if self.string_op(ins, mn): return
        # ---- FPU
        if ins.raw and self.fpu(ins): return
        # ---- MMX
        if self.mmx(ins, mn): return
        raise Unsupported('insn')

    def string_op(self, ins, mn):
        parts = mn.split()
        rep = parts[0] if len(parts) == 2 else None
        base = parts[-1]
        sizes = {'b': 1, 'w': 2, 'd': 4}
        if base[:-1] not in ('movs', 'stos', 'lods', 'cmps', 'scas') or base[-1] not in sizes: return False
        if base == 'cmpsd' and len(ins.ops) == 3: return False
        if base == 'movsd' and ins.ops and ins.ops[0].type == CS_OP_REG and 'xmm' in self.reg(ins, ins.ops[0].reg): return False
        op = base[:-1]; n = sizes[base[-1]]
        e = self.e
        step = 'STEP%d' % n
        if op == 'movs':
            if rep: e('ecx = rep_movs(&edi, &esi, ecx, %d);' % n)
            else: e('%s(edi) = %s(esi); esi += %s; edi += %s;' % (MEMF[n], MEMF[n], step, step))
            return True
        if op == 'stos':
            acc = {4: 'eax', 2: '(uint16_t)eax', 1: '(uint8_t)eax'}[n]
            if rep: e('ecx = rep_stos(&edi, %s, ecx, %d);' % (acc, n))
            else: e('%s(edi) = %s; edi += %s;' % (MEMF[n], acc, step))
            return True
        if op == 'lods':
            if rep: raise Unsupported('rep lods')
            v = '%s(esi)' % MEMF[n]
            if n == 4: e('eax = %s;' % v)
            elif n == 2: e('eax = (eax & 0xffff0000u) | %s;' % v)
            else: e('eax = (eax & 0xffffff00u) | %s;' % v)
            e('esi += %s;' % step)
            return True
        msk = '' if n == 4 else ' & ' + MASK[n]
        if op == 'scas':
            acc = {4: 'eax', 2: '(eax & 0xffffu)', 1: '(eax & 0xffu)'}[n]
            a = self.t(acc); b = self.t('0u'); r = self.t('0u')
            if rep:
                cond = '!=' if rep == 'repne' else '=='
                e('while (ecx) { ecx--; %s = %s(edi); edi += %s; %s = (%s - %s)%s; if (%s %s 0) {} else break; }' % (b, MEMF[n], step, r, a, b, msk, r, '!=' if rep == 'repne' else '=='))
            else:
                e('%s = %s(edi); edi += %s; %s = (%s - %s)%s;' % (b, MEMF[n], step, r, a, b, msk))
            self.set_arith('sub', a, b, r, n)
            return True
        if op == 'cmps':
            a = self.t('0u'); b = self.t('0u'); r = self.t('0u')
            if rep:
                e('while (ecx) { ecx--; %s = %s(esi); %s = %s(edi); esi += %s; edi += %s; %s = (%s - %s)%s; if (%s %s 0) {} else break; }' % (a, MEMF[n], b, MEMF[n], step, step, r, a, b, msk, r, '!=' if rep == 'repne' else '=='))
            else:
                e('%s = %s(esi); %s = %s(edi); esi += %s; edi += %s; %s = (%s - %s)%s;' % (a, MEMF[n], b, MEMF[n], step, step, r, a, b, msk))
            self.set_arith('sub', a, b, r, n)
            return True
        return False

    # ------------------------------------------------------------- x87
    def fpu(self, ins):
        raw = ins.raw
        i = 0
        while raw[i] in (0x66, 0xf2, 0xf3, 0x2e, 0x3e, 0x26, 0x36, 0x64, 0x65, 0x9b):
            i += 1
        op = raw[i]
        if not (0xd8 <= op <= 0xdf): return False
        modrm = raw[i + 1]
        mod = modrm >> 6; reg = (modrm >> 3) & 7; rm = modrm & 7
        e = self.e
        mem = self.memop(ins)
        A = self.addr(ins, mem) if mem is not None else None
        ARITH = ['+', '*', None, None, '-', 'r-', '/', 'r/']
        def arith(dst, src, k):
            o = ARITH[k]
            if o[0] == 'r': return '%s = %s %s %s;' % (dst, src, o[1], dst)
            return '%s = %s %s %s;' % (dst, dst, o, src)
        def com(src, pops):
            e('FCOM(ST(0), %s);' % src)
            for _ in range(pops): e('FPOP();')
        if mod != 3:
            if op in (0xd8, 0xdc, 0xda, 0xde):
                src = {0xd8: 'MF32(%s)', 0xdc: 'MF64(%s)', 0xda: '(double)(int32_t)M32(%s)', 0xde: '(double)(int16_t)M16(%s)'}[op] % A
                if reg in (2, 3): com(src, reg - 2); return True
                e(arith('ST(0)', src, reg)); return True
            if op == 0xd9:
                if reg == 0: e('{ double v = MF32(%s); FPUSH(v); }' % A); return True
                if reg == 2: e('MF32(%s) = (float)ST(0);' % A); return True
                if reg == 3: e('MF32(%s) = (float)ST(0); FPOP();' % A); return True
                if reg == 5: e('R.fcw = M16(%s);' % A); return True
                if reg == 7: e('M16(%s) = R.fcw;' % A); return True
                if reg == 4: return True  # fldenv
                if reg == 6: e('fpu_fnstenv(%s);' % A); return True
            if op == 0xdb:
                if reg == 0: e('{ double v = (double)(int32_t)M32(%s); FPUSH(v); }' % A); return True
                if reg == 2: e('M32(%s) = (uint32_t)FIST32(ST(0));' % A); return True
                if reg == 3: e('M32(%s) = (uint32_t)FIST32(ST(0)); FPOP();' % A); return True
                if reg == 1: e('M32(%s) = (uint32_t)(int32_t)ST(0); FPOP();' % A); return True
                if reg == 5: e('{ double v = fld80(%s); FPUSH(v); }' % A); return True
                if reg == 7: e('fst80(%s, ST(0)); FPOP();' % A); return True
            if op == 0xdd:
                if reg == 0: e('{ double v = MF64(%s); FPUSH(v); }' % A); return True
                if reg == 2: e('MF64(%s) = ST(0);' % A); return True
                if reg == 3: e('MF64(%s) = ST(0); FPOP();' % A); return True
                if reg == 7: e('M16(%s) = FSW();' % A); return True
                if reg == 4: e('fpu_frstor(%s);' % A); return True
                if reg == 6: e('fpu_fnsave(%s);' % A); return True
            if op == 0xdf:
                if reg == 0: e('{ double v = (double)(int16_t)M16(%s); FPUSH(v); }' % A); return True
                if reg == 2: e('M16(%s) = (uint16_t)FIST32(ST(0));' % A); return True
                if reg == 3: e('M16(%s) = (uint16_t)FIST32(ST(0)); FPOP();' % A); return True
                if reg == 5: e('{ double v = (double)(int64_t)M64(%s); FPUSH(v); }' % A); return True
                if reg == 7: e('M64(%s) = (uint64_t)FIST64(ST(0)); FPOP();' % A); return True
            raise Unsupported('fpu mem %x/%d' % (op, reg))
        # register forms
        sti = 'ST(%d)' % rm
        if op == 0xd8:
            if reg in (2, 3): com(sti, reg - 2); return True
            e(arith('ST(0)', sti, reg)); return True
        if op in (0xdc, 0xde):
            if op == 0xde and reg == 3 and rm == 1: com('ST(1)', 2); return True
            if reg in (2, 3): com(sti, reg - 2 + (1 if op == 0xde else 0)); return True
            # dst = st(i); note sub/div reversed relative to memory forms
            k = {0: 0, 1: 1, 4: 5, 5: 4, 6: 7, 7: 6}[reg]
            e(arith(sti, 'ST(0)', k))
            if op == 0xde: e('FPOP();')
            return True
        if op == 0xd9:
            if reg == 0: e('{ double v = %s; FPUSH(v); }' % sti); return True
            if reg == 1: e('{ double v = ST(0); ST(0) = %s; %s = v; }' % (sti, sti)); return True
            x = modrm
            m = {0xd0: '', 0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabs(ST(0));',
                 0xe4: 'FCOM(ST(0), 0.0);', 0xe5: 'FXAM();',
                 0xe8: 'FPUSH(1.0);', 0xe9: 'FPUSH(3.321928094887362347870);', 0xea: 'FPUSH(1.442695040888963407360);',
                 0xeb: 'FPUSH(3.141592653589793238463);', 0xec: 'FPUSH(0.301029995663981195214);',
                 0xed: 'FPUSH(0.693147180559945309417);', 0xee: 'FPUSH(0.0);',
                 0xf0: 'ST(0) = exp2(ST(0)) - 1.0;', 0xf1: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                 0xf2: 'ST(0) = tan(ST(0)); FPUSH(1.0);', 0xf3: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                 0xf4: 'FXTRACT();', 0xf5: 'ST(0) = remainder(ST(0), ST(1));',
                 0xf6: 'R.ftop = (R.ftop - 1) & 7;', 0xf7: 'R.ftop = (R.ftop + 1) & 7;',
                 0xf8: 'ST(0) = fmod(ST(0), ST(1));', 0xf9: 'ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();',
                 0xfa: 'ST(0) = sqrt(ST(0));', 0xfb: '{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }',
                 0xfc: 'ST(0) = FRND(ST(0));', 0xfd: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                 0xfe: 'ST(0) = sin(ST(0));', 0xff: 'ST(0) = cos(ST(0));'}
            if x in m:
                e(m[x])
                # our trig/remainder results are always complete: clear C2 ("incomplete/out of range"),
                # otherwise a C2 left over from fxam/fcom makes the CRT's fprem reduction loop spin forever
                if x in (0xf2, 0xf5, 0xf8, 0xfb, 0xfe, 0xff): e('R.fsw &= ~0x0400u;')
                return True
        if op == 0xda and modrm == 0xe9: com('ST(1)', 2); return True
        if op == 0xdb:
            if modrm in (0xe2, 0xe0, 0xe1, 0xe4): e('R.fsw &= 0x7f00u;'); return True
            if modrm == 0xe3: e('fpu_init();'); return True
            if reg in (5, 6):
                e('{ double a = ST(0), b = %s; ZF = (a == b) || isnan(a) || isnan(b); CF = (a < b) || isnan(a) || isnan(b); PF = isnan(a) || isnan(b); OF = 0; SF = 0; }' % sti)
                self.sym = {}; return True
        if op == 0xdd:
            if reg == 0: return True  # ffree
            if reg == 2: e('%s = ST(0);' % sti); return True
            if reg == 3: e('%s = ST(0); FPOP();' % sti); return True
            if reg == 4: com(sti, 0); return True
            if reg == 5: com(sti, 1); return True
        if op == 0xdf:
            if modrm == 0xe0: e('eax = (eax & 0xffff0000u) | FSW();'); return True
            if reg in (5, 6):
                e('{ double a = ST(0), b = %s; ZF = (a == b) || isnan(a) || isnan(b); CF = (a < b) || isnan(a) || isnan(b); PF = isnan(a) || isnan(b); OF = 0; SF = 0; FPOP(); }' % sti)
                self.sym = {}; return True
        raise Unsupported('fpu reg %x %x' % (op, modrm))

    # ------------------------------------------------------------- MMX
    def mmx(self, ins, mn):
        ops = ins.ops
        def mmreg(op):
            if op.type == CS_OP_REG:
                n = self.reg(ins, op.reg)
                if n.startswith('mm'): return 'R.mm[%s]' % n[2:]
            return None
        def src64(op):
            r = mmreg(op)
            if r: return r
            if op.type == CS_OP_MEM: return 'M64(%s)' % self.addr(ins, op)
            if op.type == CS_OP_IMM: return '(uint64_t)%du' % (op.imm & 0xff)
            raise Unsupported('mmx src')
        if mn == 'emms': return True
        if mn == 'movd':
            d, s = ops
            if mmreg(d): self.e('%s = (uint64_t)%s;' % (mmreg(d), self.rd(ins, s, 4)))
            else: self.wr(ins, d, '(uint32_t)%s' % mmreg(s), 4)
            return True
        if mn == 'movq':
            d, s = ops
            if mmreg(d): self.e('%s = %s;' % (mmreg(d), src64(s)))
            elif d.type == CS_OP_MEM: self.e('M64(%s) = %s;' % (self.addr(ins, d), src64(s)))
            else: raise Unsupported('movq')
            return True
        simple = {'pand': '&', 'por': '|', 'pxor': '^'}
        if mn in simple:
            d, s = ops
            self.e('%s %s= %s;' % (mmreg(d), simple[mn], src64(s))); return True
        if mn == 'pandn':
            d, s = ops; self.e('%s = ~%s & %s;' % (mmreg(d), mmreg(d), src64(s))); return True
        helpers = {'punpcklbw', 'punpcklwd', 'punpckldq', 'punpckhbw', 'punpckhwd', 'punpckhdq', 'pmullw', 'pmulhw',
                   'psrlw', 'psrld', 'psrlq', 'psllw', 'pslld', 'psllq', 'psraw', 'psrad', 'paddusw', 'paddusb', 'paddw', 'paddd', 'paddb',
                   'paddsw', 'psubw', 'psubd', 'psubb', 'psubusb', 'psubusw', 'psubsw', 'packuswb', 'packssdw', 'packsswb', 'pmaddwd',
                   'pcmpeqb', 'pcmpeqw', 'pcmpeqd', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd'}
        if mn in helpers:
            d, s = ops
            if not mmreg(d): raise Unsupported('mmx dst')
            self.e('%s = mmx_%s(%s, %s);' % (mmreg(d), mn, mmreg(d), src64(s)))
            self.tr.mmx_used.add(mn)
            return True
        return False


def imp_name(dll, name):
    return 'imp_%s_%s' % (dll.split('.')[0], name)


class Translator:
    def __init__(self, mods):
        self.mods = mods
        self.used_imports = set()
        self.unsupported = {}
        self.mmx_used = set()

    def run(self, outdir, nfiles=64):
        os.makedirs(outdir, exist_ok=True)
        owner = {}
        for m in self.mods:
            for f in m.an.funcs: owner[f] = m
        funcs = sorted(owner)
        chunks = [[] for _ in range(nfiles)]
        sizes = [0] * nfiles
        for f in funcs:
            n = sum(len(b) for b in owner[f].an.funcs[f]['blocks'].values())
            i = sizes.index(min(sizes))
            chunks[i].append(f); sizes[i] += n
        protos = ['void f_%08x(void);' % f for f in funcs]
        for k, ch in enumerate(chunks):
            lines = ['#include "recomp.h"', '#include "funcs.h"', '']
            for f in sorted(ch):
                fe = FuncEmitter(self, owner[f], owner[f].an.funcs[f])
                lines += fe.emit()
                lines.append('')
            write_if_changed(os.path.join(outdir, 'code_%02d.c' % k), '\n'.join(lines) + '\n')
        all_imports = {}
        for m in self.mods: all_imports.update(m.img.imports)
        imps = sorted(set(all_imports.values()) | self.used_imports)
        h = ['#pragma once', '#include <stdint.h>'] + protos + ['void %s(void);' % imp_name(*i) for i in imps]
        write_if_changed(os.path.join(outdir, 'funcs.h'), '\n'.join(h) + '\n')
        # function table + import table
        t = ['#include "recomp.h"', '#include "funcs.h"', '',
             'const uint32_t recomp_func_count = %d;' % len(funcs),
             'const struct func_entry recomp_funcs[] = {']
        t += ['  {0x%08xu, f_%08x},' % (f, f) for f in funcs]
        t += ['};', '',
              'const uint32_t recomp_import_count = %d;' % len(all_imports),
              'const struct import_entry recomp_imports[] = {']
        for slot, (dll, name) in sorted(all_imports.items()):
            t.append('  {0x%08xu, "%s", "%s", %s},' % (slot, dll, name, imp_name(dll, name)))
        t += ['};']
        # weak default stubs for unimplemented imports
        for dll, name in imps:
            t.append('__attribute__((weak)) void %s(void) { hle_missing("%s", "%s"); }' % (imp_name(dll, name), dll, name))
        if self.mmx_used:
            pass
        write_if_changed(os.path.join(outdir, 'table.c'), '\n'.join(t) + '\n')


def write_if_changed(path, data):
    try:
        if open(path).read() == data: return
    except FileNotFoundError:
        pass
    open(path, 'w').write(data)


def load_extra(path):
    s = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].strip()
            if line: s.add(int(line, 16))
    return s


if __name__ == '__main__':
    # usage: translate.py OUTDIR MODULE.exe [MODULE.dll ...]
    outdir, paths = sys.argv[1], sys.argv[2:]
    extra = load_extra(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_entries.txt'))
    mods = []
    for p in paths:
        img = Image(p)
        lo, hi = img.base, img.base + img.size
        an = Analyzer(img, [e for e in extra if lo <= e < hi]).run()
        print('%s: %d functions' % (os.path.basename(p), len(an.funcs)))
        mods.append(Module(img, an))
    tr = Translator(mods)
    tr.run(outdir)
    for k, v in sorted(tr.unsupported.items(), key=lambda kv: -kv[1]):
        print('  unsupported', k, v)
    print('mmx', sorted(tr.mmx_used))
