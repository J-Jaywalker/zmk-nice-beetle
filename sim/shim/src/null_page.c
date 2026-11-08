/*
 * Emulates the nRF52840's readable address 0.
 *
 * On the Cortex-M4 the bottom of the address space is flash (the vector
 * table), so a NULL pointer *read* does not fault there: firmware that reads
 * through NULL just gets a junk value and carries on.  The LVGL 8 versions of
 * the widgets did this during screen init (luna.c / hid_indicators.c / screen.c
 * dereferenced as_zmk_*(NULL); LVGL 8's lv_canvas_transform() read through the
 * NULL "refreshing display" pointer).  With the LVGL 9 port no such read has
 * been seen, but the handler stays as a safety net.  On Windows such a read is an
 * access violation, so this vectored exception handler emulates loads from
 * the first 64 KiB as reading 0 and skips the instruction.  Each faulting
 * site is logged once.  Writes through NULL are not emulated.
 *
 * Only plain x86-32 load forms are decoded (mov / movzx / movsx / cmp with
 * a memory source); anything else falls through to a normal crash.
 */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "sim_internal.h"

static uintptr_t mod_lo, mod_hi;
static PVOID veh_handle;

static DWORD *reg32(CONTEXT *c, int r) {
    switch (r & 7) {
    case 0: return &c->Eax;
    case 1: return &c->Ecx;
    case 2: return &c->Edx;
    case 3: return &c->Ebx;
    case 4: return &c->Esp;
    case 5: return &c->Ebp;
    case 6: return &c->Esi;
    default: return &c->Edi;
    }
}

static int modrm_len(const uint8_t *p) {
    uint8_t m = p[0], mod = m >> 6, rm = m & 7;
    int len = 1;
    if (mod != 3 && rm == 4) {
        uint8_t sib = p[1];
        len++;
        if (mod == 0 && (sib & 7) == 5) {
            len += 4;
        }
    }
    if (mod == 0 && rm == 5) {
        len += 4;
    } else if (mod == 1) {
        len += 1;
    } else if (mod == 2) {
        len += 4;
    }
    return len;
}

static void set_flags_cmp(CONTEXT *c, uint32_t a, uint32_t b, int size) {
    /* flags of (a - b) at operand size */
    uint32_t mask = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
    uint32_t sign = size == 1 ? 0x80 : size == 2 ? 0x8000 : 0x80000000u;
    a &= mask;
    b &= mask;
    uint32_t r = (a - b) & mask;
    DWORD f = c->EFlags & ~(0x8D5u); /* clear CF PF AF ZF SF OF */
    if (a < b) f |= 0x1;
    if (r == 0) f |= 0x40;
    if (r & sign) f |= 0x80;
    if (((a ^ b) & (a ^ r)) & sign) f |= 0x800;
    int ones = __builtin_popcount(r & 0xff);
    if (!(ones & 1)) f |= 0x4;
    if (((a ^ b ^ r) & 0x10)) f |= 0x10;
    c->EFlags = f;
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t eip = c->Eip, addr = er->ExceptionInformation[1];
    if (er->ExceptionInformation[0] != 0 || addr >= 0x10000 || eip < mod_lo || eip >= mod_hi) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint8_t *p = (const uint8_t *)eip;
    int pre = 0, opsize = 4;
    while (p[pre] == 0x66 || p[pre] == 0x2e || p[pre] == 0x3e) {
        if (p[pre] == 0x66) {
            opsize = 2;
        }
        pre++;
    }
    const uint8_t *op = p + pre;
    int len;
    int reg;
    if (op[0] == 0x8b || op[0] == 0x8a) { /* mov r, r/m */
        reg = (op[1] >> 3) & 7;
        len = pre + 1 + modrm_len(op + 1);
        if (op[0] == 0x8a) {
            DWORD *r = reg32(c, reg & 3);
            *r &= (reg & 4) ? ~0xff00u : ~0xffu;
        } else if (opsize == 2) {
            *reg32(c, reg) &= ~0xffffu;
        } else {
            *reg32(c, reg) = 0;
        }
    } else if (op[0] == 0x0f && (op[1] == 0xb6 || op[1] == 0xb7 || op[1] == 0xbe || op[1] == 0xbf)) {
        reg = (op[2] >> 3) & 7;
        len = pre + 2 + modrm_len(op + 2);
        *reg32(c, reg) = 0;
    } else if (op[0] == 0x3b || op[0] == 0x3a) { /* cmp r, r/m : r - 0 */
        reg = (op[1] >> 3) & 7;
        len = pre + 1 + modrm_len(op + 1);
        set_flags_cmp(c, *reg32(c, reg), 0, op[0] == 0x3a ? 1 : opsize);
    } else if (op[0] == 0x39 || op[0] == 0x38) { /* cmp r/m, r : 0 - r */
        reg = (op[1] >> 3) & 7;
        len = pre + 1 + modrm_len(op + 1);
        set_flags_cmp(c, 0, *reg32(c, reg), op[0] == 0x38 ? 1 : opsize);
    } else if ((op[0] == 0x80 || op[0] == 0x83 || op[0] == 0x81) && ((op[1] >> 3) & 7) == 7) {
        int ml = modrm_len(op + 1);
        int32_t imm = op[0] == 0x81 ? *(const int32_t *)(op + 1 + ml) : (int8_t)op[1 + ml];
        len = pre + 1 + ml + (op[0] == 0x81 ? 4 : 1);
        set_flags_cmp(c, 0, (uint32_t)imm, op[0] == 0x80 ? 1 : opsize);
    } else {
        sim_log(SIM_LOG_ERR, "NULL+0x%x read at %p: instruction not emulated, crashing",
                (unsigned)addr, (void *)eip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static uintptr_t seen[32];
    static int nseen;
    bool known = false;
    for (int i = 0; i < nseen; i++) {
        known |= seen[i] == eip;
    }
    if (!known && nseen < 32) {
        seen[nseen++] = eip;
        sim_log(SIM_LOG_WRN,
                "firmware read through a NULL pointer (address 0x%x, code %p); on nRF52 this "
                "reads flash instead of faulting - emulated as 0",
                (unsigned)addr, (void *)(eip - mod_lo));
    }
    c->Eip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void sim_null_page_install(void) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&sim_null_page_install, &m)) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + dos->e_lfanew);
        mod_lo = (uintptr_t)m;
        mod_hi = mod_lo + nt->OptionalHeader.SizeOfImage;
    }
    veh_handle = AddVectoredExceptionHandler(1, handler);
}

/* The host unloads and reloads the DLL when the firmware reboots; the handler
 * lives in this DLL, so it must be unregistered before the code goes away. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && veh_handle) {
        RemoveVectoredExceptionHandler(veh_handle);
        veh_handle = NULL;
    }
    return TRUE;
}
