/* Process startup, logging and the table of natively provided DLL exports. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

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

/* Runs the game's entry point; never returns (ExitProcess ends the process). */
void game_start(void) {
    mem_init();
    dispatch_init();
    load_image("GameSaga.exe");
    setup_process("GameSaga.exe");
    R.esp -= 4;
    M32(R.esp) = 0xdead0000u;
    dispatch(image_entry);
    LOG("entry point returned eax=%d\n", R.eax);
    plat_exit((int)R.eax);
}
