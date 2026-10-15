"""Interprocedural register usage summaries.

For every recovered function F we compute
  DEF(F): registers F (or anything it calls) may write
  USE(F): registers whose value on entry may be read, including registers in
          DEF(F) that are not written on every path to the exit (they are
          spilled back unchanged).
Generated code then loads only USE(F) on entry, spills only DEF(F) on return,
and around a call to G spills USE(G) / reloads DEF(G) instead of all eight
registers.
"""
from capstone import CS_OP_IMM, CS_OP_MEM, CS_OP_REG
from analyze import COND_JUMPS

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
BIT = {r: 1 << i for i, r in enumerate(REGS)}
ALL = 0xff
ESP = BIT['esp']
PARENT = {}
for r in REGS:
    PARENT[r] = r
for a, b in (('ax', 'eax'), ('cx', 'ecx'), ('dx', 'edx'), ('bx', 'ebx'), ('sp', 'esp'), ('bp', 'ebp'), ('si', 'esi'),
             ('di', 'edi'), ('al', 'eax'), ('ah', 'eax'), ('cl', 'ecx'), ('ch', 'ecx'), ('dl', 'edx'), ('dh', 'edx'),
             ('bl', 'ebx'), ('bh', 'ebx')):
    PARENT[a] = b
# runtime-implemented imports read the stack and return in eax (ecx/edx are caller-saved)
IMPORT_USE = BIT['eax'] | BIT['ecx'] | BIT['edx'] | ESP
IMPORT_DEF = BIT['eax'] | BIT['ecx'] | BIT['edx'] | ESP
# indirect calls/jumps target compiler-generated code following the standard
# conventions (ebx/esi/edi/ebp preserved); they may read anything
CALLER_SAVED = BIT['eax'] | BIT['ecx'] | BIT['edx'] | ESP
NORETURN_IMPORTS = ('ExitProcess', 'RaiseException', 'TerminateProcess')


def mask_of(names):
    m = 0
    for n in names:
        p = PARENT.get(n)
        if p: m |= BIT[p]
    return m


class Summaries:
    def __init__(self, an):
        self.an = an
        self.use = {}
        self.defs = {}
        self._rw = {}

    # ---------------------------------------------------------------- per instruction
    def rw(self, ins):
        """(read, write) masks for a non-call, non-ret instruction as translated to C."""
        k = ins.addr
        if k in self._rw: return self._rw[k]
        mn = ins.mn
        if mn in ('cpuid', 'rdtsc'):
            r = (ALL, ALL)
        elif mn in ('xor', 'sub') and len(ins.ops) == 2 and ins.ops[0].type == CS_OP_REG and ins.ops[1].type == CS_OP_REG \
                and ins.ops[0].reg == ins.ops[1].reg and ins.ops[0].size == 4:
            r = (0, mask_of([ins.cs.reg_name(ins.ops[0].reg)]))
        else:
            try:
                rd, wr = ins.cs.regs_access()
            except Exception:
                rd, wr = [], []
            rn = [ins.cs.reg_name(x) for x in rd]
            wn = [ins.cs.reg_name(x) for x in wr]
            read = mask_of(rn)
            write = mask_of(wn)
            # partial-register writes are read-modify-write in the generated C
            for n in wn:
                if n in PARENT and n != PARENT[n]: read |= BIT[PARENT[n]]
            # explicit operands (regs_access can miss some)
            for op in ins.ops:
                if op.type == CS_OP_MEM:
                    if op.mem.base: read |= mask_of([ins.cs.reg_name(op.mem.base)])
                    if op.mem.index: read |= mask_of([ins.cs.reg_name(op.mem.index)])
            if mn.startswith('rep') or mn.split()[-1][:4] in ('movs', 'stos', 'lods', 'cmps', 'scas'):
                read |= BIT['ecx'] | BIT['esi'] | BIT['edi'] | BIT['eax']
                write |= BIT['ecx'] | BIT['esi'] | BIT['edi'] | BIT['eax']
            if mn in ('push', 'pop', 'pushal', 'popal', 'pushfd', 'popfd', 'leave', 'enter'):
                read |= ESP; write |= ESP
            r = (read, write)
        self._rw[k] = r
        return r

    # ---------------------------------------------------------------- terminators / calls
    def call_effect(self, ins):
        """For call instructions: (use, def, returns)."""
        an = self.an
        op = ins.ops[0]
        if op.type == CS_OP_IMM:
            t = op.imm & 0xffffffff
            ret = t not in an.noreturn
            if t in an.funcs:
                return self.use.get(t, 0) | ESP, self.defs.get(t, 0) | ESP, ret
            return ALL, CALLER_SAVED, ret
        imp = an.iat_target(op)
        if imp:
            return IMPORT_USE, IMPORT_DEF, imp[1] not in NORETURN_IMPORTS
        return ALL, CALLER_SAVED, True

    def jump_use(self, fn, t):
        """Register use of leaving the function through `goto t` that is not a local label."""
        if t in fn['blocks']: return None
        if t in self.an.funcs:
            return self.use.get(t, 0) | self.defs[fn['entry']] | ESP
        return ALL

    # ---------------------------------------------------------------- per function
    def local_defs(self, fn):
        d = ESP
        for body in fn['blocks'].values():
            for a in body:
                ins = self.an.insns[a]
                mn = ins.mn
                if mn == 'call':
                    d |= self.call_effect(ins)[1]
                elif mn == 'jmp':
                    op = ins.ops[0]
                    if op.type == CS_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t not in fn['blocks']:
                            d |= self.defs.get(t, 0) if t in self.an.funcs else CALLER_SAVED
                    else:
                        imp = self.an.iat_target(op)
                        d |= IMPORT_DEF if imp else CALLER_SAVED
                elif mn in COND_JUMPS:
                    t = ins.ops[0].imm & 0xffffffff
                    if t not in fn['blocks']:
                        d |= self.defs.get(t, 0) if t in self.an.funcs else CALLER_SAVED
                    if mn in ('loop', 'loope', 'loopne'): d |= BIT['ecx']
                elif mn in ('ret', 'retf'):
                    pass
                else:
                    d |= self.rw(ins)[1]
        return d

    def block_live_in(self, fn, b, live_out_of):
        """Backward transfer over one block given a function returning live-out of successors."""
        an = self.an
        body = fn['blocks'][b]
        if not body: return 0
        F = fn['entry']
        last = an.insns[body[-1]]
        nxt = last.addr + last.size
        mn = last.mn
        # live at the end of the block (after the terminator decision)
        live = 0
        term = True
        if mn in ('ret', 'retf'):
            live = self.defs[F] | ESP
        elif mn == 'jmp':
            op = last.ops[0]
            if op.type == CS_OP_IMM:
                t = op.imm & 0xffffffff
                u = self.jump_use(fn, t)
                live = live_out_of(t) if u is None else u
            elif last.addr in an.jumptables:
                live = ESP   # (the never-expected dispatch fallback is not modelled)
                for t in an.jumptables[last.addr]:
                    if t in fn['blocks']: live |= live_out_of(t)
                live |= self.rw(last)[0]
            else:
                imp = an.iat_target(op)
                if imp: live = IMPORT_USE | self.defs[F]
                else: live = ALL | self.rw(last)[0]
        elif mn in COND_JUMPS:
            t = last.ops[0].imm & 0xffffffff
            u = self.jump_use(fn, t)
            live = live_out_of(t) if u is None else u
            u2 = self.jump_use(fn, nxt)
            live |= live_out_of(nxt) if u2 is None else u2
            if mn in ('jecxz', 'loop', 'loope', 'loopne'): live |= BIT['ecx']
        elif mn in ('hlt', 'int3', 'ud2', 'iretd'):
            live = 0
        else:
            term = False
            # falls through (or noreturn call)
            if mn == 'call' and not self.call_effect(last)[2]:
                live = 0
            elif nxt in fn['blocks']:
                live = live_out_of(nxt)
            else:
                live = 0   # UNIMPL
        # walk the body backwards; the terminator (if any) is already accounted for
        insns = body[:-1] if term else body
        for a in reversed(insns):
            ins = an.insns[a]
            if ins.mn == 'call':
                u, d, r = self.call_effect(ins)
                if not r: live = 0
                live = (live & ~d) | u | ESP
            else:
                rd, wr = self.rw(ins)
                live = (live & ~wr) | rd
        return live

    def func_use(self, fn):
        blocks = fn['blocks']
        live_in = {b: 0 for b in blocks}
        get = lambda t: live_in.get(t, 0)
        changed = True
        while changed:
            changed = False
            for b in sorted(blocks, reverse=True):
                v = self.block_live_in(fn, b, get)
                if v != live_in[b]:
                    live_in[b] = v; changed = True
        return live_in.get(fn['entry'], 0) | ESP

    def run(self):
        an = self.an
        funcs = an.funcs
        for f in funcs:
            self.defs[f] = ESP; self.use[f] = ESP
        changed = True
        while changed:
            changed = False
            for f, fn in funcs.items():
                d = self.local_defs(fn)
                if d | self.defs[f] != self.defs[f]:
                    self.defs[f] |= d; changed = True
        changed = True
        it = 0
        while changed:
            changed = False; it += 1
            for f, fn in funcs.items():
                u = self.func_use(fn)
                if u | self.use[f] != self.use[f]:
                    self.use[f] |= u; changed = True
        return self


def regs_of(mask):
    return [r for r in REGS if mask & BIT[r]]
