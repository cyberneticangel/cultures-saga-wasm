/* CPU state, indirect-call dispatch, native thunks and FPU/MMX helpers. */
#include "recomp.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

CPU R;

/* Direct-mapped dispatch tables, one per module code range. */
#define MAX_RANGES 4
static struct { uint32_t lo, hi; gfn_t *table; } ranges[MAX_RANGES];
static int nranges;

/* Native callables (import thunks, COM methods, ...) live at fake guest
 * addresses THUNK_BASE + 4*i. */
#define MAX_THUNKS ((THUNK_END - THUNK_BASE) / 4)
static gfn_t thunks[MAX_THUNKS];
static const char *thunk_names[MAX_THUNKS];
static uint32_t nthunks;

uint32_t thunk_register(gfn_t fn, const char *name) {
    for (uint32_t i = 0; i < nthunks; i++)
        if (thunks[i] == fn) return THUNK_BASE + 4 * i;
    if (nthunks >= MAX_THUNKS) fatal("out of thunks");
    thunks[nthunks] = fn;
    thunk_names[nthunks] = name;
    /* make the thunk address hold a recognisable instruction (ret) */
    return THUNK_BASE + 4 * nthunks++;
}

const char *thunk_name(uint32_t addr) {
    if (addr >= THUNK_BASE && addr < THUNK_BASE + 4 * nthunks) return thunk_names[(addr - THUNK_BASE) / 4];
    return NULL;
}

void dispatch_init(void) {
    uint32_t i = 0;
    while (i < recomp_func_count && nranges < MAX_RANGES) {
        uint32_t j = i;
        while (j + 1 < recomp_func_count && recomp_funcs[j + 1].addr - recomp_funcs[j].addr < 0x100000u) j++;
        uint32_t lo = recomp_funcs[i].addr, hi = recomp_funcs[j].addr + 1;
        ranges[nranges].lo = lo; ranges[nranges].hi = hi;
        ranges[nranges].table = calloc(hi - lo, sizeof(gfn_t));
        for (uint32_t k = i; k <= j; k++) ranges[nranges].table[recomp_funcs[k].addr - lo] = recomp_funcs[k].fn;
        nranges++;
        i = j + 1;
    }
}

uint32_t trace_calls;

void dispatch(uint32_t addr) {
    for (int r = 0; r < nranges; r++)
        if (addr >= ranges[r].lo && addr < ranges[r].hi) {
            gfn_t f = ranges[r].table[addr - ranges[r].lo];
            if (f) { f(); return; }
            goto fail;
        }
    if (addr >= THUNK_BASE && addr < THUNK_BASE + 4 * nthunks) {
        if (trace_calls) {
            uint32_t i = (addr - THUNK_BASE) / 4;
            logf_("-> %s(%x, %x, %x, %x) from %08x\n", thunk_names[i], M32(R.esp + 4), M32(R.esp + 8), M32(R.esp + 12), M32(R.esp + 16), M32(R.esp));
        }
        thunks[(addr - THUNK_BASE) / 4]();
        return;
    }
fail:
    fatal("dispatch: no function at 0x%08x (ret addr 0x%08x)", addr, M32(R.esp));
}

void table_miss(uint32_t at, uint32_t tgt) {
    logf_("warning: jump table at %08x went to unexpected target %08x\n", at, tgt);
}

/* Call a guest function with stdcall/cdecl args.  Returns eax.  Caller-cleanup
 * functions leave their args on the stack; we restore esp either way. */
uint32_t call_guest(uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t saved = R.esp;
    for (int i = nargs - 1; i >= 0; i--) { R.esp -= 4; M32(R.esp) = args[i]; }
    R.esp -= 4; M32(R.esp) = 0xdead0000u;   /* fake return address */
    dispatch(addr);
    R.esp = saved;
    return R.eax;
}

void unimpl(uint32_t addr) {
    fatal("unimplemented/invalid instruction at 0x%08x", addr);
}
void divzero(uint32_t addr) {
    fatal("integer division by zero at 0x%08x", addr);
}
void hle_missing(const char *dll, const char *name) {
    fatal("unimplemented import %s!%s (called from 0x%08x)", dll, name, M32(R.esp));
}

void hle_cpuid(void) {
    switch (R.eax) {
    case 0:
        R.eax = 1;
        R.ebx = 0x756e6547; R.edx = 0x49656e69; R.ecx = 0x6c65746e;   /* GenuineIntel */
        break;
    case 1:
        R.eax = 0x00000652;          /* Pentium II */
        R.ebx = 0; R.ecx = 0;
        R.edx = 0x00008011;          /* FPU, TSC, CMOV - no MMX/SSE */
        break;
    default:
        R.eax = R.ebx = R.ecx = R.edx = 0;
    }
}
void hle_rdtsc(void) {
    uint64_t t = (uint64_t)plat_ticks_us() * 400;   /* pretend 400 MHz */
    R.eax = (uint32_t)t; R.edx = (uint32_t)(t >> 32);
}

/* ---- x87 extended precision ---- */
double fld80(uint32_t a) {
    uint64_t mant = M64(a);
    uint16_t se = M16(a + 8);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
void fst80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    v = fabs(v);
    uint64_t mant = 0; int exp = 0;
    if (isnan(v)) { exp = 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { exp = 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double m = frexp(v, &e);            /* v = m * 2^e, m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        exp = e - 1 + 16383;
    }
    M64(a) = mant;
    M16(a + 8) = se | (uint16_t)exp;
}
void fpu_init(void) {
    R.fcw = 0x027f;   /* MSVC default: 53-bit precision, round to nearest, all masked */
    R.fsw = 0; R.ftop = 0;
}
void fpu_fnstenv(uint32_t a) {
    memset(PTR(a), 0, 28);
    M16(a) = R.fcw; M16(a + 4) = FSW(); M16(a + 8) = 0xffff;
}
void fpu_fnsave(uint32_t a) {
    fpu_fnstenv(a);
    for (int i = 0; i < 8; i++) fst80(a + 28 + 10 * i, ST(i));
    fpu_init();
}
void fpu_frstor(uint32_t a) {
    R.fcw = M16(a); R.fsw = M16(a + 4); R.ftop = (R.fsw >> 11) & 7;
    for (int i = 0; i < 8; i++) ST(i) = fld80(a + 28 + 10 * i);
}

/* ---- MMX ---- */
typedef union { uint64_t q; uint32_t d[2]; uint16_t w[4]; int16_t sw[4]; int32_t sd[2]; uint8_t b[8]; int8_t sb[8]; } mmx_t;
#define M(x) mmx_t x##_ = {.q = x}
#define LANES(n, expr) for (int i = 0; i < n; i++) { expr; } return r.q

static int32_t sat16s(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static int32_t sat8s(int32_t v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.b[2*i] = a_.b[i]; r.b[2*i+1] = b_.b[i]); }
uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.b[2*i] = a_.b[4+i]; r.b[2*i+1] = b_.b[4+i]); }
uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.w[2*i] = a_.w[i]; r.w[2*i+1] = b_.w[i]); }
uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.w[2*i] = a_.w[2+i]; r.w[2*i+1] = b_.w[2+i]); }
uint64_t mmx_punpckldq(uint64_t a, uint64_t b) { return (a & 0xffffffffull) | (b << 32); }
uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) { return (a >> 32) | (b & 0xffffffff00000000ull); }
uint64_t mmx_pmullw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = (uint16_t)(a_.sw[i] * b_.sw[i])); }
uint64_t mmx_pmulhw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = (uint16_t)((a_.sw[i] * b_.sw[i]) >> 16)); }
uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.sd[i] = a_.sw[2*i] * b_.sw[2*i] + a_.sw[2*i+1] * b_.sw[2*i+1]); }
uint64_t mmx_psrlw(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 15) return 0; LANES(4, r.w[i] = a_.w[i] >> b); }
uint64_t mmx_psrld(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 31) return 0; LANES(2, r.d[i] = a_.d[i] >> b); }
uint64_t mmx_psrlq(uint64_t a, uint64_t b) { return b > 63 ? 0 : a >> b; }
uint64_t mmx_psllw(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 15) return 0; LANES(4, r.w[i] = (uint16_t)(a_.w[i] << b)); }
uint64_t mmx_pslld(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 31) return 0; LANES(2, r.d[i] = a_.d[i] << b); }
uint64_t mmx_psllq(uint64_t a, uint64_t b) { return b > 63 ? 0 : a << b; }
uint64_t mmx_psraw(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 15) b = 15; LANES(4, r.sw[i] = a_.sw[i] >> b); }
uint64_t mmx_psrad(uint64_t a, uint64_t b) { M(a); mmx_t r; if (b > 31) b = 31; LANES(2, r.sd[i] = a_.sd[i] >> b); }
uint64_t mmx_paddb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, r.b[i] = a_.b[i] + b_.b[i]); }
uint64_t mmx_paddw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = a_.w[i] + b_.w[i]); }
uint64_t mmx_paddd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.d[i] = a_.d[i] + b_.d[i]); }
uint64_t mmx_paddusb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, int v = a_.b[i] + b_.b[i]; r.b[i] = v > 255 ? 255 : v); }
uint64_t mmx_paddusw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, int v = a_.w[i] + b_.w[i]; r.w[i] = v > 65535 ? 65535 : v); }
uint64_t mmx_paddsw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.sw[i] = sat16s(a_.sw[i] + b_.sw[i])); }
uint64_t mmx_psubb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, r.b[i] = a_.b[i] - b_.b[i]); }
uint64_t mmx_psubw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = a_.w[i] - b_.w[i]); }
uint64_t mmx_psubd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.d[i] = a_.d[i] - b_.d[i]); }
uint64_t mmx_psubusb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, int v = a_.b[i] - b_.b[i]; r.b[i] = v < 0 ? 0 : v); }
uint64_t mmx_psubusw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, int v = a_.w[i] - b_.w[i]; r.w[i] = v < 0 ? 0 : v); }
uint64_t mmx_psubsw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.sw[i] = sat16s(a_.sw[i] - b_.sw[i])); }
uint64_t mmx_packuswb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r;
    for (int i = 0; i < 4; i++) { int v = a_.sw[i]; r.b[i] = v < 0 ? 0 : v > 255 ? 255 : v; v = b_.sw[i]; r.b[4+i] = v < 0 ? 0 : v > 255 ? 255 : v; }
    return r.q; }
uint64_t mmx_packsswb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r;
    for (int i = 0; i < 4; i++) { r.sb[i] = sat8s(a_.sw[i]); r.sb[4+i] = sat8s(b_.sw[i]); }
    return r.q; }
uint64_t mmx_packssdw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r;
    for (int i = 0; i < 2; i++) { r.sw[i] = sat16s(a_.sd[i] < -32768 ? -32768 : a_.sd[i] > 32767 ? 32767 : a_.sd[i]);
                                  r.sw[2+i] = b_.sd[i] < -32768 ? -32768 : b_.sd[i] > 32767 ? 32767 : b_.sd[i]; }
    return r.q; }
uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, r.b[i] = a_.b[i] == b_.b[i] ? 0xff : 0); }
uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = a_.w[i] == b_.w[i] ? 0xffff : 0); }
uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.d[i] = a_.d[i] == b_.d[i] ? 0xffffffffu : 0); }
uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(8, r.b[i] = a_.sb[i] > b_.sb[i] ? 0xff : 0); }
uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(4, r.w[i] = a_.sw[i] > b_.sw[i] ? 0xffff : 0); }
uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) { M(a); M(b); mmx_t r; LANES(2, r.d[i] = a_.sd[i] > b_.sd[i] ? 0xffffffffu : 0); }
