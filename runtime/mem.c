/* Guest address space: identity-mapped region, heap allocator (HeapAlloc & co)
 * and a page-granular VirtualAlloc region. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef __wasm__
#include <sys/mman.h>
#endif

/* ------------------------------------------------------------------ heap
 * Boundary-tag allocator.  Chunk at c:
 *   M32(c)   prev_size (valid when previous chunk is free)
 *   M32(c+4) size | INUSE | PREV_INUSE
 *   user data at c+8; free chunks keep next/prev links at c+8/c+12.  */
#define INUSE 1u
#define PREV_INUSE 2u
#define SZ(c) (M32((c) + 4) & ~7u)
#define NBINS 128
#define MINCHUNK 16u

static uint32_t bins[NBINS];
static uint32_t heap_top, heap_limit;

static int bin_of(uint32_t sz) {
    if (sz < 1024) return sz >> 4;                       /* 0..63, exact */
    int lg = 31 - __builtin_clz(sz);                      /* >= 10 */
    int sub = (sz >> (lg - 3)) & 7;                       /* 8 sub-bins per power of two */
    int b = 64 + (lg - 10) * 8 + sub;
    return b < NBINS ? b : NBINS - 1;
}

static void bin_insert(uint32_t c) {
    int b = bin_of(SZ(c));
    uint32_t h = bins[b];
    M32(c + 8) = h; M32(c + 12) = 0;
    if (h) M32(h + 12) = c;
    bins[b] = c;
}

static void bin_remove(uint32_t c) {
    uint32_t n = M32(c + 8), p = M32(c + 12);
    if (p) M32(p + 8) = n; else bins[bin_of(SZ(c))] = n;
    if (n) M32(n + 12) = p;
}

static void set_free(uint32_t c, uint32_t sz) {
    M32(c + 4) = sz | (M32(c + 4) & PREV_INUSE);
    M32(c + sz) = sz;                                    /* next chunk's prev_size */
    M32(c + sz + 4) &= ~PREV_INUSE;
}

uint32_t heap_alloc(uint32_t size, int zero) {
    if (size > 0x7fffffffu) return 0;
    uint32_t need = (size + 8 + 15) & ~15u;
    if (need < MINCHUNK) need = MINCHUNK;
    for (int b = bin_of(need); b < NBINS; b++) {
        for (uint32_t c = bins[b]; c; c = M32(c + 8)) {
            uint32_t sz = SZ(c);
            if (sz < need) continue;
            bin_remove(c);
            if (sz - need >= 32) {                         /* split */
                uint32_t r = c + need;
                M32(c + 4) = need | (M32(c + 4) & PREV_INUSE) | INUSE;
                M32(r + 4) = (sz - need) | PREV_INUSE;
                set_free(r, sz - need);
                bin_insert(r);
            } else {
                M32(c + 4) |= INUSE;
                M32(c + sz + 4) |= PREV_INUSE;
            }
            if (zero) memset(PTR(c + 8), 0, SZ(c) - 8);
            return c + 8;
        }
        if (b < 64 && bins[b] == 0) continue;
    }
    /* carve from the top; the top "chunk" header lives at heap_top */
    if (heap_top + need + 16 > heap_limit && heap_limit == DLL_BASE) {
        /* switch to the second arena above the DLL; the old top stays as an in-use sentinel */
        M32(heap_top + 4) = INUSE | (M32(heap_top + 4) & PREV_INUSE);
        heap_top = DLL_BASE + 0x00100000u;
        heap_limit = VA_BASE;
        M32(heap_top + 4) = PREV_INUSE;
    }
    if (heap_top + need + 16 > heap_limit) {
        uint64_t freeb = 0; uint32_t nfree = 0;
        for (int b2 = 0; b2 < NBINS; b2++) for (uint32_t c2 = bins[b2]; c2; c2 = M32(c2 + 8)) { freeb += SZ(c2); nfree++; }
        logf_("heap exhausted: request %u bytes; top=%08x, %u free chunks totalling %llu bytes\n", size, heap_top, nfree, (unsigned long long)freeb);
        return 0;
    }
    uint32_t c = heap_top;
    M32(c + 4) = need | INUSE | (M32(c + 4) & PREV_INUSE);
    heap_top += need;
    M32(heap_top + 4) = PREV_INUSE;                       /* sentinel header of top */
    if (zero) memset(PTR(c + 8), 0, need - 8);
    return c + 8;
}

void heap_free(uint32_t p) {
    if (!p) return;
    uint32_t c = p - 8;
    if (!(M32(c + 4) & INUSE)) { logf_("double free of %08x\n", p); return; }
    uint32_t sz = SZ(c);
    M32(c + 4) &= ~INUSE;
    /* merge with next */
    uint32_t n = c + sz;
    if (n == heap_top) {
        /* merge into top: also merge previous free chunk first */
        if (!(M32(c + 4) & PREV_INUSE)) {
            uint32_t ps = M32(c);
            c -= ps; bin_remove(c);
        }
        heap_top = c;
        M32(c + 4) = (M32(c + 4) & PREV_INUSE);
        return;
    }
    if (!(M32(n + 4) & INUSE)) { bin_remove(n); sz += SZ(n); }
    if (!(M32(c + 4) & PREV_INUSE)) {
        uint32_t ps = M32(c);
        c -= ps; bin_remove(c); sz += ps;
    }
    M32(c + 4) = sz | (M32(c + 4) & PREV_INUSE);
    set_free(c, sz);
    bin_insert(c);
}

uint32_t heap_size(uint32_t p) {
    if (!p) return 0;
    return SZ(p - 8) - 8;
}

uint32_t heap_realloc(uint32_t p, uint32_t size, int zero) {
    if (!p) return heap_alloc(size, zero);
    uint32_t old = heap_size(p);
    if (size <= old) return p;
    uint32_t q = heap_alloc(size, 0);
    if (!q) return 0;
    memcpy(PTR(q), PTR(p), old);
    if (zero) memset(PTR(q + old), 0, heap_size(q) - old);
    heap_free(p);
    return q;
}

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = heap_alloc(n, 0);
    memcpy(PTR(p), s, n);
    return p;
}

uint32_t guest_alloc_static(uint32_t size) { return heap_alloc(size, 1); }

/* ------------------------------------------------------------ VirtualAlloc
 * 64 KiB granular reservations tracked in a table.  Memory in the guest region
 * is always backed; "commit" only zeroes on first use. */
#define VA_GRAN 0x10000u
#define MAX_RES 4096
static struct { uint32_t base, size; } res[MAX_RES];
static int nres;
static uint32_t va_top = VA_BASE;

static int res_find(uint32_t a) {
    for (int i = 0; i < nres; i++)
        if (a >= res[i].base && a < res[i].base + res[i].size) return i;
    return -1;
}

uint32_t va_alloc(uint32_t addr, uint32_t size, uint32_t type) {
    const uint32_t MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000;
    if (addr) {
        int i = res_find(addr);
        if (i >= 0) return (type & MEM_COMMIT) ? (addr & ~0xfffu) : res[i].base;
        if (!(type & MEM_RESERVE)) return 0;
    }
    uint32_t sz = (size + VA_GRAN - 1) & ~(VA_GRAN - 1);
    /* first fit among gaps */
    uint32_t a = VA_BASE;
    for (;;) {
        int clash = -1;
        for (int i = 0; i < nres; i++)
            if (a < res[i].base + res[i].size && res[i].base < a + sz) { clash = i; break; }
        if (clash < 0) break;
        a = res[clash].base + res[clash].size;
    }
    if (a + sz > GUEST_END || nres >= MAX_RES) { logf_("VirtualAlloc failed (%u bytes)\n", size); return 0; }
    res[nres].base = a; res[nres].size = sz; nres++;
    memset(PTR(a), 0, sz);
    if (a + sz > va_top) va_top = a + sz;
    return a;
}

int va_free(uint32_t addr, uint32_t size, uint32_t type) {
    const uint32_t MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000;
    int i = res_find(addr);
    if (i < 0) return 0;
    if (type & MEM_RELEASE) {
        res[i] = res[--nres];
        return 1;
    }
    if (type & MEM_DECOMMIT) {
        memset(PTR(addr), 0, size ? size : res[i].size);
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------- init */
void mem_init(void) {
#ifndef __wasm__
    void *p = mmap((void *)(uintptr_t)GUEST_LOW, GUEST_END - GUEST_LOW, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)(uintptr_t)GUEST_LOW) fatal("cannot map guest address space at 0x%x", GUEST_LOW);
#else
    extern unsigned char __global_base;
    if ((uintptr_t)&__global_base < GUEST_END) fatal("runtime data overlaps guest space");
#endif
    heap_top = HEAP_BASE;
    heap_limit = DLL_BASE;   /* recompiled DLLs load at 0x10000000 */
    M32(heap_top + 4) = PREV_INUSE;
}
