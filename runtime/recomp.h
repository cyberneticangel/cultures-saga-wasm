/* Core definitions shared by the recompiled game code and the runtime.
 *
 * Guest memory is identity mapped: a 32-bit guest address is used directly as a
 * host pointer.  On wasm32 that is natural; natively the guest region is mmap'd
 * at the same low addresses (see mem.c).
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df, eflags_hi;
    double st[8];
    uint32_t ftop;
    uint16_t fsw, fcw;
    uint64_t mm[8];
} CPU;

extern CPU R;

/* ---- guest memory map ---- */
#define GUEST_LOW      0x00010000u
#define STACK_BOTTOM   0x00100000u   /* main thread stack: 0x100000..0x300000 */
#define STACK_TOP      0x00300000u
#define FS_BASE        0x00380000u   /* TEB */
#define PEB_ADDR       0x00381000u
#define IMAGE_BASE     0x00400000u
#define THUNK_BASE     0x00600000u   /* fake addresses for native callables */
#define THUNK_END      0x00610000u
#define HEAP_BASE      0x00700000u
#define GUEST_END      0x38000000u   /* end of guest address space */

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float    __attribute__((aligned(1), may_alias)) f32u;
typedef double   __attribute__((aligned(1), may_alias)) f64u;

#define PTR(a)  ((void *)(uintptr_t)(uint32_t)(a))
#define M8(a)   (*(uint8_t *)(uintptr_t)(uint32_t)(a))
#define M16(a)  (*(u16u *)(uintptr_t)(uint32_t)(a))
#define M32(a)  (*(u32u *)(uintptr_t)(uint32_t)(a))
#define M64(a)  (*(u64u *)(uintptr_t)(uint32_t)(a))
#define MF32(a) (*(f32u *)(uintptr_t)(uint32_t)(a))
#define MF64(a) (*(f64u *)(uintptr_t)(uint32_t)(a))
#define GADDR(p) ((uint32_t)(uintptr_t)(p))

/* ---- register caching in generated functions ---- */
#define LOCALS() \
    uint32_t eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, \
             esp = R.esp, ebp = R.ebp, esi = R.esi, edi = R.edi; \
    uint32_t CF = 0, ZF = 0, SF = 0, OF = 0, PF = 0; \
    (void)CF; (void)ZF; (void)SF; (void)OF; (void)PF
#define FLAGS() \
    uint32_t CF = 0, ZF = 0, SF = 0, OF = 0, PF = 0; \
    (void)CF; (void)ZF; (void)SF; (void)OF; (void)PF
#define SPILL()  (R.eax = eax, R.ecx = ecx, R.edx = edx, R.ebx = ebx, \
                  R.esp = esp, R.ebp = ebp, R.esi = esi, R.edi = edi)
#define RELOAD() (eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, \
                  esp = R.esp, ebp = R.ebp, esi = R.esi, edi = R.edi)
#define PUSH(v)   (esp -= 4, M32(esp) = (uint32_t)(v))
#define PUSH16(v) (esp -= 2, M16(esp) = (uint16_t)(v))

#define PARITY(x) ((uint32_t)!__builtin_parity((uint32_t)(x) & 0xffu))
#define STEP1 (R.df ? 0xffffffffu : 1u)
#define STEP2 (R.df ? 0xfffffffeu : 2u)
#define STEP4 (R.df ? 0xfffffffcu : 4u)

void unimpl(uint32_t addr) __attribute__((noreturn));
void divzero(uint32_t addr) __attribute__((noreturn));
#define UNIMPL(a) unimpl(a)
#define DIVZERO(a) divzero(a)

void dispatch(uint32_t addr);
void table_miss(uint32_t at, uint32_t tgt);
void hle_missing(const char *dll, const char *name) __attribute__((noreturn));
void hle_cpuid(void);
void hle_rdtsc(void);

typedef void (*gfn_t)(void);
struct func_entry { uint32_t addr; gfn_t fn; };
struct import_entry { uint32_t slot; const char *dll, *name; gfn_t fn; };
extern const uint32_t recomp_func_count, recomp_import_count;
/* the files the code was generated from: [0] is the game executable */
extern const uint32_t recomp_module_count;
extern const char *const recomp_module_names[];
extern const uint32_t recomp_module_crcs[];
extern const char recomp_variant[];   /* build name, from the executable's content (recomp/variant.py) */
extern const struct func_entry recomp_funcs[];
extern const struct import_entry recomp_imports[];

/* ---- string instructions ---- */
static inline uint32_t rep_movs(uint32_t *edi, uint32_t *esi, uint32_t n, int sz) {
    if (!n) return 0;
    uint32_t bytes = n * sz;
    if (!R.df) {
        uint32_t d = *edi, s = *esi;
        if (d <= s || d >= s + bytes) memmove(PTR(d), PTR(s), bytes);
        else for (uint32_t i = 0; i < bytes; i++) M8(d + i) = M8(s + i);
        *edi += bytes; *esi += bytes;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            memmove(PTR(*edi), PTR(*esi), sz);
            *edi -= sz; *esi -= sz;
        }
    }
    return 0;
}
static inline uint32_t rep_stos(uint32_t *edi, uint32_t v, uint32_t n, int sz) {
    if (!n) return 0;
    if (!R.df) {
        uint32_t d = *edi;
        if (sz == 1) memset(PTR(d), (int)(v & 0xff), n);
        else if (sz == 2) for (uint32_t i = 0; i < n; i++) M16(d + 2 * i) = (uint16_t)v;
        else if (v == 0) memset(PTR(d), 0, n * 4);
        else for (uint32_t i = 0; i < n; i++) M32(d + 4 * i) = v;
        *edi += n * sz;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (sz == 1) M8(*edi) = (uint8_t)v; else if (sz == 2) M16(*edi) = (uint16_t)v; else M32(*edi) = v;
            *edi -= sz;
        }
    }
    return 0;
}

/* ---- x87 ---- */
#define ST(i) R.st[(R.ftop + (i)) & 7]
#define FPUSH(v) (R.ftop = (R.ftop - 1) & 7, R.st[R.ftop] = (v))
#define FPOP() (R.ftop = (R.ftop + 1) & 7)
#define FSW() ((uint16_t)((R.fsw & ~0x3800u) | ((R.ftop & 7) << 11)))

static inline void FCOM(double a, double b) {
    R.fsw &= ~0x4500u;
    if (isnan(a) || isnan(b)) R.fsw |= 0x4500u;
    else if (a < b) R.fsw |= 0x0100u;
    else if (a == b) R.fsw |= 0x4000u;
}
static inline double FRND(double x) {
    switch ((R.fcw >> 10) & 3) {
    case 0: return nearbyint(x);
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
static inline int32_t FIST32(double x) {
    double r = FRND(x);
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return INT32_MIN;
    return (int32_t)r;
}
static inline int64_t FIST64(double x) {
    double r = FRND(x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}
static inline void FXAM(void) {
    double v = ST(0);
    uint16_t c = 0;
    if (signbit(v)) c |= 0x0200;              /* C1 = sign */
    if (isnan(v)) c |= 0x0100;                /* C0 */
    else if (isinf(v)) c |= 0x0500;           /* C2|C0 */
    else if (v == 0) c |= 0x4000;             /* C3 */
    else c |= 0x0400;                         /* C2 normal */
    R.fsw = (R.fsw & ~0x4700u) | c;
}
static inline void FXTRACT(void) {
    int e; double m = frexp(ST(0), &e);
    ST(0) = (double)(e - 1);
    FPUSH(m * 2.0);
}
double fld80(uint32_t addr);
void fst80(uint32_t addr, double v);
void fpu_init(void);
void fpu_fnstenv(uint32_t addr);
void fpu_fnsave(uint32_t addr);
void fpu_frstor(uint32_t addr);

/* ---- MMX (helpers in cpu.c) ---- */
#define MMXOP(n) uint64_t mmx_##n(uint64_t a, uint64_t b);
MMXOP(punpcklbw) MMXOP(punpcklwd) MMXOP(punpckldq) MMXOP(punpckhbw) MMXOP(punpckhwd) MMXOP(punpckhdq)
MMXOP(pmullw) MMXOP(pmulhw) MMXOP(psrlw) MMXOP(psrld) MMXOP(psrlq) MMXOP(psllw) MMXOP(pslld) MMXOP(psllq)
MMXOP(psraw) MMXOP(psrad) MMXOP(paddusw) MMXOP(paddusb) MMXOP(paddw) MMXOP(paddd) MMXOP(paddb) MMXOP(paddsw)
MMXOP(psubw) MMXOP(psubd) MMXOP(psubb) MMXOP(psubusb) MMXOP(psubusw) MMXOP(psubsw) MMXOP(packuswb)
MMXOP(packssdw) MMXOP(packsswb) MMXOP(pmaddwd) MMXOP(pcmpeqb) MMXOP(pcmpeqw) MMXOP(pcmpeqd)
MMXOP(pcmpgtb) MMXOP(pcmpgtw) MMXOP(pcmpgtd)
#undef MMXOP
