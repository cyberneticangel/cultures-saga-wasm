/* Process startup, logging and the table of natively provided DLL exports. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>

int log_level = 1;

void logf_(const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log(buf);
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n] = '\n'; buf[n + 1] = 0;
    plat_log("FATAL: ");
    plat_log(buf);
    char regs[256];
    snprintf(regs, sizeof regs, "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
             R.eax, R.ecx, R.edx, R.ebx, R.esp, R.ebp, R.esi, R.edi);
    plat_log(regs);
    plat_exit(3);
}

void ddraw_DirectDrawCreateEx(void);
void ddraw_DirectDrawCreate(void);
void imp_DINPUT_DirectInputCreateA(void);
void imp_DSOUND_ord11(void);

struct native_export { const char *dll, *name; gfn_t fn; };
const struct native_export native_exports[] = {
    {"ddraw.dll", "DirectDrawCreateEx", ddraw_DirectDrawCreateEx},
    {"ddraw.dll", "DirectDrawCreate", ddraw_DirectDrawCreate},
    {"dinput.dll", "DirectInputCreateA", imp_DINPUT_DirectInputCreateA},
    {"dinput.dll", "DirectInputCreate", imp_DINPUT_DirectInputCreateA},
    {"dsound.dll", "DirectSoundCreate8", imp_DSOUND_ord11},
    {NULL, NULL, NULL}};

/* The executable this build was made from, found by content: the original releases are all
 * called Game.exe, so the file in the game folder may have another name than at build time. */
const char *game_exe_name;
uint32_t crc32_of(const uint8_t *p, uint32_t n);
uint8_t *vfs_read_all(const char *winpath, uint32_t *len);
static int exe_matches(const char *name) {
    uint32_t len;
    uint8_t *d = vfs_read_all(name, &len);
    int ok = d && crc32_of(d, len) == recomp_module_crcs[0];
    free(d);
    return ok;
}
static const char *find_game_exe(void) {
    if (exe_matches(recomp_module_names[0])) return recomp_module_names[0];
    static char names[1 << 16]; static uint8_t isdir[2048];
    int n = plat_listdir(".", names, sizeof names, isdir, 2048);
    const char *p = names;
    for (int i = 0; i < n; i++, p += strlen(p) + 1) {
        size_t l = strlen(p);
        if (isdir[i] || l < 5 || strcasecmp(p + l - 4, ".exe") != 0) continue;
        if (exe_matches(p)) { LOG("game executable: %s (built from %s)\n", p, recomp_module_names[0]); return p; }
    }
    fatal("this build is for %s (CRC-32 %08x), but no executable with that content is in the game folder. "
          "Build for your executable with: make gen EXE=<path> && make web EXE=<path>",
          recomp_module_names[0], recomp_module_crcs[0]);
}

/* Runs the game's entry point; never returns (ExitProcess ends the process). */
void game_start(void) {
    mem_init();
    dispatch_init();
    game_exe_name = find_game_exe();
    load_image(game_exe_name);
    setup_process(game_exe_name);
    R.esp -= 4;
    M32(R.esp) = 0xdead0000u;
    dispatch(image_entry);
    LOG("entry point returned eax=%d\n", R.eax);
    plat_exit((int)R.eax);
}
