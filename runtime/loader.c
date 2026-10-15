/* Maps the original executable's sections into guest memory and wires the IAT
 * to native implementations. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

uint32_t image_entry;
static uint32_t image_size;

static uint8_t *slurp(const char *path, uint32_t *len) {
    int fd = plat_open(path, 0, 0, 0);
    if (fd < 0) fatal("cannot open %s", path);
    int64_t n = plat_size(fd);
    uint8_t *buf = malloc((size_t)n);
    if (plat_read(fd, buf, (uint32_t)n) != n) fatal("short read on %s", path);
    plat_close(fd);
    *len = (uint32_t)n;
    return buf;
}

#define RD16(p) (*(u16u *)(p))
#define RD32(p) (*(u32u *)(p))

/* Map a PE image at its preferred base and wire its IAT.  Returns the base. */
static uint32_t crc32_of(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}

/* The recompiled code only fits the exact file it was generated from. */
static void check_module(const char *path, const uint8_t *f, uint32_t len) {
    const char *base = strrchr(path, '\\'); base = base ? base + 1 : path;
    for (uint32_t i = 0; i < recomp_module_count; i++)
        if (strcasecmp(recomp_module_names[i], base) == 0) {
            uint32_t c = crc32_of(f, len);
            if (c != recomp_module_crcs[i])
                fatal("%s differs from the file this build was made from (CRC %08x, expected %08x). "
                      "Rebuild with: make web EXE=<path to this %s>", base, c, recomp_module_crcs[i], base);
            return;
        }
}

uint32_t map_pe(const char *path, uint32_t *entry_out, uint32_t *size_out) {
    uint32_t len;
    uint8_t *f = slurp(path, &len);
    check_module(path, f, len);
    uint32_t pe = RD32(f + 0x3c);
    if (RD32(f + pe) != 0x4550) fatal("%s: not a PE file", path);
    uint16_t nsec = RD16(f + pe + 6);
    uint16_t optsz = RD16(f + pe + 20);
    uint8_t *opt = f + pe + 24;
    uint32_t base = RD32(opt + 28);
    uint32_t size = RD32(opt + 56);
    uint32_t entry = base + RD32(opt + 16);
    memset(PTR(base), 0, size);
    memcpy(PTR(base), f, RD32(opt + 60));
    uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = RD32(sec + 8), va = RD32(sec + 12), rawsz = RD32(sec + 16), rawptr = RD32(sec + 20);
        uint32_t n = rawsz < vsize || vsize == 0 ? rawsz : vsize;
        memcpy(PTR(base + va), f + rawptr, n);
    }
    free(f);
    for (uint32_t i = 0; i < recomp_import_count; i++) {
        const struct import_entry *e = &recomp_imports[i];
        if (e->slot >= base && e->slot < base + size) M32(e->slot) = thunk_register(e->fn, e->name);
    }
    if (entry_out) *entry_out = entry;
    if (size_out) *size_out = size;
    return base;
}

void load_image(const char *exe_path) {
    uint32_t base = map_pe(exe_path, &image_entry, &image_size);
    if (base != IMAGE_BASE) fatal("unexpected image base %08x", base);
    LOG("image loaded: entry %08x, size %08x\n", image_entry, image_size);
}

/* Look up an export by name in a mapped image. */
uint32_t pe_export(uint32_t base, const char *name) {
    uint32_t pe = M32(base + 0x3c);
    uint32_t dir = M32(base + pe + 24 + 96);
    if (!dir) return 0;
    uint32_t ed = base + dir;
    uint32_t nnames = M32(ed + 24), funcs = base + M32(ed + 28), names = base + M32(ed + 32), ords = base + M32(ed + 36);
    for (uint32_t i = 0; i < nnames; i++)
        if (strcmp((char *)PTR(base + M32(names + 4 * i)), name) == 0)
            return base + M32(funcs + 4 * M16(ords + 2 * i));
    return 0;
}

/* Thread/process environment */
void setup_process(const char *cmdline) {
    (void)cmdline;
    memset(PTR(FS_BASE), 0, 0x1000);
    M32(FS_BASE + 0x00) = 0xffffffffu;      /* SEH chain end */
    M32(FS_BASE + 0x04) = STACK_TOP;
    M32(FS_BASE + 0x08) = STACK_BOTTOM;
    M32(FS_BASE + 0x18) = FS_BASE;
    M32(FS_BASE + 0x20) = 0x1000;           /* process id */
    M32(FS_BASE + 0x24) = 0x1004;           /* thread id */
    M32(FS_BASE + 0x30) = PEB_ADDR;
    memset(PTR(PEB_ADDR), 0, 0x1000);
    M32(PEB_ADDR + 0x08) = IMAGE_BASE;
    R.esp = STACK_TOP - 64;
    R.ebp = 0;
    fpu_init();
}

/* ---- COM helpers ---- */
uint32_t com_make_vtbl(const gfn_t *methods, const char *const *names, int n) {
    uint32_t v = guest_alloc_static(4 * n);
    for (int i = 0; i < n; i++) {
        const char *nm = names ? names[i] : NULL;
        if (!nm) { char *b = malloc(32); snprintf(b, 32, "com%p#%d", (void *)methods, i); nm = b; }
        M32(v + 4 * i) = thunk_register(methods[i], nm);
    }
    return v;
}

uint32_t com_new(uint32_t vtbl, uint32_t extra) {
    uint32_t o = guest_alloc_static(4 + extra);
    M32(o) = vtbl;
    return o;
}
