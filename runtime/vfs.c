/* Windows path -> host path relative to the game root, case-insensitively. */
#include "runtime.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

/* The guest believes it lives in this directory. */
char exe_dir_win[260] = "C:\\CULTURES";

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* cache of resolved directory listings */
#define DCACHE 64
static struct { char path[260]; char *names; uint8_t *isdir; int n; } dcache[DCACHE];
static int dnext;

void vfs_invalidate(void) {
    for (int i = 0; i < DCACHE; i++) { free(dcache[i].names); free(dcache[i].isdir); dcache[i].names = NULL; dcache[i].isdir = NULL; dcache[i].path[0] = 0; dcache[i].n = 0; }
}

static int dir_lookup(const char *dir, const char *name, char *out, int *is_dir) {
    int slot = -1;
    for (int i = 0; i < DCACHE; i++)
        if (dcache[i].names && strcmp(dcache[i].path, dir) == 0) { slot = i; break; }
    if (slot < 0) {
        slot = dnext++ % DCACHE;
        free(dcache[slot].names); free(dcache[slot].isdir);
        int cap = 1 << 18;
        dcache[slot].names = malloc(cap);
        dcache[slot].isdir = malloc(8192);
        dcache[slot].n = plat_listdir(dir[0] ? dir : ".", dcache[slot].names, cap, dcache[slot].isdir, 8192);
        if (dcache[slot].n < 0) dcache[slot].n = 0;
        snprintf(dcache[slot].path, sizeof dcache[slot].path, "%s", dir);
    }
    char *p = dcache[slot].names;
    for (int i = 0; i < dcache[slot].n; i++) {
        if (ieq(p, name)) { strcpy(out, p); if (is_dir) *is_dir = dcache[slot].isdir[i]; return 1; }
        p += strlen(p) + 1;
    }
    return 0;
}

/* Convert to host-relative path.  Returns 1 if the whole path exists. */
uint32_t vfs_resolve(const char *win, char *out, int cap) {
    char tmp[520];
    snprintf(tmp, sizeof tmp, "%s", win);
    for (char *c = tmp; *c; c++) if (*c == '/') *c = '\\';
    const char *p = tmp;
    size_t el = strlen(exe_dir_win);
    if (strncasecmp(p, exe_dir_win, el) == 0 && (p[el] == '\\' || p[el] == 0)) p += el;
    else if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;   /* other absolute path: map into root */
    while (*p == '\\') p++;
    while (p[0] == '.' && p[1] == '\\') p += 2;
    out[0] = 0;
    int exists = 1;
    while (*p) {
        const char *s = strchr(p, '\\');
        char comp[260];
        size_t n = s ? (size_t)(s - p) : strlen(p);
        if (n >= sizeof comp) n = sizeof comp - 1;
        memcpy(comp, p, n); comp[n] = 0;
        p = s ? s + 1 : p + n;
        while (*p == '\\') p++;
        if (n == 0 || strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            char *sl = strrchr(out, '/');
            if (sl) *sl = 0; else out[0] = 0;
            continue;
        }
        char real[260];
        if (exists && dir_lookup(out, comp, real, NULL)) {
            /* found */
        } else {
            exists = 0;
            strcpy(real, comp);
        }
        size_t ol = strlen(out);
        if (ol + strlen(real) + 2 >= (size_t)cap) break;
        if (ol) out[ol++] = '/';
        strcpy(out + ol, real);
    }
    if (!out[0]) strcpy(out, ".");
    return exists;
}

/* Read a whole game file into a malloc'd buffer (NULL if missing). */
uint8_t *vfs_read_all(const char *winpath, uint32_t *len) {
    char host[520];
    if (!vfs_resolve(winpath, host, sizeof host)) return NULL;
    int fd = plat_open(host, 0, 0, 0);
    if (fd < 0) return NULL;
    int64_t n = plat_size(fd);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    uint32_t got = 0;
    while (buf && got < (uint32_t)n) {
        int k = plat_read(fd, buf + got, (uint32_t)n - got);
        if (k <= 0) break;
        got += (uint32_t)k;
    }
    plat_close(fd);
    if (!buf || got != (uint32_t)n) { free(buf); return NULL; }
    *len = got;
    return buf;
}
