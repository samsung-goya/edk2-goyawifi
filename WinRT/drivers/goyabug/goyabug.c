/* goyabug - writes the stop code, its parameters and the driver they point into on the stop screen.

   The Windows 8 build 8520 stop screen shows only the name of the stop (no parameters) and WinPE keeps
   no crash dump, so an intermittent KMODE_EXCEPTION_NOT_HANDLED cannot be traced. This boot-start driver
   uses only documented interfaces: KeRegisterBugCheckCallback (the routine runs while the stop screen
   is up) and the exported KiBugCheckData (the stop code and its four parameters). Nothing in the kernel
   is patched or hooked.

   At a stop it draws a black box at the bottom of the screen:
     STOP <code>  P1 .. P4                      the parameters
     P<n> <driver>+<offset>                     for each parameter that is an address inside a driver
     PC / LR <driver>+<offset>                  for stops that pass a CONTEXT (0x7E, 0x1000007E)
     stack: <driver>+<offset> ...               Thumb return addresses found on the current stack
                                                (heuristic call chain, newest first)
   Drivers are looked up in a copy of the loaded-module list that a thread refreshes every second (the
   list itself cannot be walked safely at stop time), in a list of drivers that have unloaded since
   (shown with a leading '~'), and, for the exception address, by walking back to the image's MZ header
   and reading its PDB name (finds images that are not in the list).

   Non-PnP kernel service, Start 0 (boot), no INF: make-goya-sd.cmd / install-winrt-sd.cmd copy
   goyabug.sys to System32\drivers and create the service key. ErrorControl 0: if it cannot load,
   Windows boots without it.

   Persistent log (common\goyalog.h): goyabug also writes a text log to the firmware-reserved RAM at
   0x08100000 - boot number, every driver and program image loaded, DbgPrint output when the kernel
   passes it on, a heartbeat and the shutdown - and at a stop the same text it draws, then resets the
   tablet with the watchdog after about 10 s. The firmware saves the log to the card on the next start.
*/
#define GOYA_TRACE_ID 7
#include <ntddk.h>
#include "..\common\goyatrace.h"
#include "..\common\gtfont.h"

#define SystemModuleInformation 11
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG Class, PVOID Buffer, ULONG Length, PULONG ReturnLength);
NTKERNELAPI PUCHAR PsGetProcessImageFileName(PEPROCESS Process);

typedef struct _MODINFO {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex, InitOrderIndex, LoadCount, OffsetToFileName;
    UCHAR  FullPathName[256];
} MODINFO;

typedef struct _MODLIST {
    ULONG   Count;
    MODINFO M[1];
} MODLIST;

#define LIST_BYTES (128 * 1024)

static PUCHAR gList[2];
static volatile LONG gActive = -1;           /* index of the valid snapshot, -1 = none */
static PULONG_PTR gBugData;                  /* KiBugCheckData[5] */
static KBUGCHECK_CALLBACK_RECORD gRecord;

/* ---- persistent log ---------------------------------------------------------------------------- */

static volatile UCHAR *gLog;                 /* whole GOYALOG area, uncached */
static GOYALOG_HDR *gHdr;
static volatile LONG gLogLock;               /* lock word in normal (cached) memory */
static BOOLEAN gCrashLog;                    /* PutCh copies the stop text into the crash area */

typedef struct _LINE { char b[240]; ULONG n; } LINE;

static VOID LS(LINE *l, const char *s) { while (*s && l->n < sizeof(l->b) - 2) l->b[l->n++] = *s++; }
static VOID LC(LINE *l, char c) { if (l->n < sizeof(l->b) - 2) l->b[l->n++] = c; }
static VOID LH(LINE *l, ULONG v, ULONG d) { while (d--) LC(l, "0123456789ABCDEF"[(v >> (d * 4)) & 15]); }
static VOID LD(LINE *l, ULONG v)
{
    char t[11]; ULONG n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) LC(l, t[--n]);
}
static VOID LU(LINE *l, PCUNICODE_STRING u)  /* UNICODE_STRING as ASCII ('?' outside ASCII) */
{
    ULONG i;
    if (u == NULL || u->Buffer == NULL) { LS(l, "(null)"); return; }
    for (i = 0; i < u->Length / sizeof(WCHAR); i++) LC(l, (u->Buffer[i] >= 32 && u->Buffer[i] < 127) ? (char)u->Buffer[i] : '?');
}

/* "[  sss.mmm] " */
static VOID LBegin(LINE *l)
{
    ULONG ms = (ULONG)(KeQueryInterruptTime() / 10000ULL), s = ms / 1000, w = 1, t;
    l->n = 0;
    LC(l, '[');
    for (t = s; t >= 10; t /= 10) w++;
    while (w++ < 5) LC(l, ' ');
    LD(l, s); LC(l, '.');
    LC(l, (char)('0' + ms / 100 % 10)); LC(l, (char)('0' + ms / 10 % 10)); LC(l, (char)('0' + ms % 10));
    LS(l, "] ");
}

/* Append the line to the boot log. Any IRQL (also from the DbgPrint callback): runs at HIGH_LEVEL
   with a spin lock on a normal-memory word, so it cannot be re-entered on this CPU. */
static VOID LEnd(LINE *l)
{
    KIRQL old;
    ULONG i, used;
    if (gHdr == NULL) return;
    l->b[l->n++] = '\r'; l->b[l->n++] = '\n';
    KeRaiseIrql(HIGH_LEVEL, &old);
    while (InterlockedCompareExchange(&gLogLock, 1, 0) != 0) YieldProcessor();
    used = gHdr->Used;
    if (used + l->n <= GOYALOG_TEXT_MAX) {
        for (i = 0; i < l->n; i++) gLog[GOYALOG_TEXT_OFF + used + i] = (UCHAR)l->b[i];
        gHdr->Used = used + l->n;
    }
    InterlockedExchange(&gLogLock, 0);
    KeLowerIrql(old);
}

static VOID LogS(const char *s) { LINE l; LBegin(&l); LS(&l, s); LEnd(&l); }

/* Stop text: no lock (single CPU at the stop, nothing else writes the crash area) */
static VOID CrashCh(char c)
{
    ULONG u;
    if (gHdr == NULL || !gCrashLog) return;
    u = gHdr->CrashUsed;
    if (c == '\n') { if (u + 2 > GOYALOG_CRASH_MAX) return; gLog[GOYALOG_CRASH_OFF + u] = '\r'; u++; }
    if (u + 1 > GOYALOG_CRASH_MAX) return;
    gLog[GOYALOG_CRASH_OFF + u] = (UCHAR)c;
    gHdr->CrashUsed = u + 1;
}

/* ---- module snapshot (PASSIVE_LEVEL) --------------------------------------------------------- */

/* Modules that disappeared from the list (unloaded drivers): code that still runs there after the unload
   would otherwise show up as "in no module". Names are stored with a leading '~'. */
#define GONE_MAX 16
typedef struct _GONE { ULONG Base, Size; char Name[16]; } GONE;
static GONE gGone[GONE_MAX];
static ULONG gGoneNext;

static ULONG ListCount(PUCHAR Buf)
{
    ULONG n = ((MODLIST *)Buf)->Count, max = (LIST_BYTES - sizeof(ULONG)) / sizeof(MODINFO);
    return n < max ? n : max;
}

static VOID Snapshot(VOID)
{
    LONG cur = gActive, next = (cur == 0) ? 1 : 0;
    ULONG ret = 0, i, j;
    if (gList[next] == NULL) return;
    if (!NT_SUCCESS(ZwQuerySystemInformation(SystemModuleInformation, gList[next], LIST_BYTES, &ret))) return;
    if (cur >= 0) {
        MODLIST *o = (MODLIST *)gList[cur], *n = (MODLIST *)gList[next];
        ULONG oc = ListCount(gList[cur]), nc = ListCount(gList[next]);
        for (i = 0; i < oc; i++) {
            for (j = 0; j < nc; j++)
                if (n->M[j].ImageBase == o->M[i].ImageBase && n->M[j].ImageSize == o->M[i].ImageSize) break;
            if (j == nc) {
                GONE *g = &gGone[gGoneNext++ % GONE_MAX];
                USHORT off = o->M[i].OffsetToFileName;
                const UCHAR *nm = o->M[i].FullPathName + (off < 255 ? off : 0);
                ULONG k;
                g->Base = 0;                                    /* invalid while being filled */
                g->Name[0] = '~';
                for (k = 0; k < sizeof(g->Name) - 2 && nm[k]; k++) g->Name[k + 1] = (char)nm[k];
                g->Name[k + 1] = 0;
                g->Size = o->M[i].ImageSize;
                g->Base = (ULONG)(ULONG_PTR)o->M[i].ImageBase;
            }
        }
    }
    InterlockedExchange(&gActive, next);
}

/* Test: holding Volume Up + Volume Down together for ~3 s stops Windows on purpose
   (MANUALLY_INITIATED_CRASH 0xE2, P1 = 0x60A7B06), to check that the goyabug box appears.
   Keys from goyakeys: GPLR bank 0 of the GPIO block, bits 1 (up) and 2 (down), active low. */
#define GPIO_PHYS 0xD4019000ULL
#define VOL_BITS  ((1UL << 1) | (1UL << 2))
static PULONG gGplr;
static VOID SmpProbe(const char *When);

static VOID SnapshotThread(PVOID Context)
{
    LARGE_INTEGER t;
    ULONG n = 0, held = 0;
    UNREFERENCED_PARAMETER(Context);
    t.QuadPart = -10000000LL;                /* 1 s */
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        Snapshot();                          /* module list every second */
        n++;
        if (n == 60 && gHdr != NULL) SmpProbe("after 60 s");
        if (gHdr != NULL) {
            gHdr->Uptime = (ULONG)(KeQueryInterruptTime() / 10000000ULL);
            if (n % 60 == 0) {
                LINE l; LBegin(&l); LS(&l, "alive, "); LD(&l, ListCount(gList[gActive >= 0 ? gActive : 0]));
                LS(&l, " modules"); LEnd(&l);
            }
        }
        if (gGplr != NULL) {
            held = ((READ_REGISTER_ULONG(gGplr) & VOL_BITS) == 0) ? held + 1 : 0;
            if (held >= 3) KeBugCheckEx(0xE2, 0x60A7B06, 0, 0, 0);
        }
    }
}

/* ---- text output at stop time (no allocation, no locks) ---------------------------------------- */

#define TS 2                                 /* font scale: 14 x 24 pixel cells */
#define CW (7 * TS)
#define CH (12 * TS)
#define COLS (GT_FB_W / CW)
#define BOX_LINES 12
#define BOX_Y (GT_FB_H - BOX_LINES * CH)

static ULONG gCol, gRow;

static VOID PutCh(PULONG fb, char c, ULONG fg)
{
    const UCHAR *g;
    ULONG x0, y0, x, y;
    CrashCh(c);                              /* the log gets every line, also past the box */
    if (c == '\n' || gCol >= COLS) { gCol = 0; gRow++; if (c == '\n') return; }
    if (gRow >= BOX_LINES || fb == NULL) return;
    if (c < 32 || c > 126) c = '?';
    g = gGtFont[c - 32];
    x0 = gCol * CW; y0 = BOX_Y + gRow * CH;
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++)
            fb[(y0 + y) * GT_FB_W + x0 + x] = ((g[y / TS] >> (6 - x / TS)) & 1) ? fg : 0;
    gCol++;
}

static VOID PutS(PULONG fb, const char *s, ULONG fg) { while (*s) PutCh(fb, *s++, fg); }

static VOID PutHex(PULONG fb, ULONG v, ULONG digits, ULONG fg)
{
    while (digits--) PutCh(fb, "0123456789ABCDEF"[(v >> (digits * 4)) & 15], fg);
}

static VOID PutDec(PULONG fb, ULONG v, ULONG fg)
{
    char b[11]; ULONG n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) PutCh(fb, b[--n], fg);
}

/* Find the module containing Addr in the snapshot. Returns the file name (or NULL) and the offset. */
static const char *Lookup(ULONG Addr, PULONG Off)
{
    LONG a = gActive;
    MODLIST *l;
    ULONG i;
    if (Addr < 0x80000000UL) return NULL;
    l = (a >= 0) ? (MODLIST *)gList[a] : NULL;
    for (i = 0; l != NULL && i < ListCount(gList[a]); i++) {
        ULONG b = (ULONG)(ULONG_PTR)l->M[i].ImageBase;
        if (Addr >= b && Addr - b < l->M[i].ImageSize) {
            USHORT o = l->M[i].OffsetToFileName;
            *Off = Addr - b;
            return (const char *)l->M[i].FullPathName + (o < 255 ? o : 0);
        }
    }
    for (i = 0; i < GONE_MAX; i++) {
        if (gGone[i].Base != 0 && Addr >= gGone[i].Base && Addr - gGone[i].Base < gGone[i].Size) {
            *Off = Addr - gGone[i].Base;
            return gGone[i].Name;
        }
    }
    return NULL;
}

/* "name+offset" (name cut to 14 characters); returns FALSE if Addr is in no known module */
static BOOLEAN PutAddr(PULONG fb, ULONG Addr, ULONG fg)
{
    ULONG off = 0, n = 0;
    const char *name = Lookup(Addr, &off);
    if (name == NULL) return FALSE;
    while (name[n] && n < 14) PutCh(fb, name[n++], fg);
    PutCh(fb, '+', fg);
    PutHex(fb, off, (off >> 16) ? 6 : 4, fg);
    return TRUE;
}

static BOOLEAN Valid(ULONG_PTR A, ULONG Len)
{
    if (A < 0x80000000UL || A + Len < A) return FALSE;
    if (!MmIsAddressValid((PVOID)A)) return FALSE;
    return ((A & (PAGE_SIZE - 1)) + Len <= PAGE_SIZE) || MmIsAddressValid((PVOID)(A + Len - 1));
}

/* Image containing Addr, found without any list: walk back page by page to an MZ/PE header whose
   SizeOfImage covers Addr. Name = file name of the PDB path in the CodeView debug entry. Returns the base
   (0 = none; *Gap = first non-resident page met, 0 if the 8 MB limit was hit). */
static ULONG FindImage(ULONG Addr, char *Name, ULONG NameLen, PULONG Gap)
{
    ULONG b, nt, opt, dbg, dsz, i;
    *Gap = 0; Name[0] = 0;
    for (b = Addr & ~(PAGE_SIZE - 1); Addr - b < 0x800000UL && b >= 0x80000000UL; b -= PAGE_SIZE) {
        if (!MmIsAddressValid((PVOID)(ULONG_PTR)b)) { *Gap = b; return 0; }
        if (*(volatile USHORT *)b != 0x5A4D) continue;
        nt = *(volatile ULONG *)(b + 0x3C);
        if (nt > PAGE_SIZE - 0xF8 || *(volatile ULONG *)(b + nt) != 0x00004550) continue;
        opt = b + nt + 24;
        if (*(volatile ULONG *)(opt + 56) <= Addr - b) continue;       /* SizeOfImage */
        dbg = *(volatile ULONG *)(opt + 96 + 6 * 8);
        dsz = *(volatile ULONG *)(opt + 96 + 6 * 8 + 4);
        for (i = 0; dbg != 0 && i + 28 <= dsz && i < 8 * 28; i += 28) {
            ULONG e = b + dbg + i, rsds, k, last = 0;
            if (!Valid(e, 28) || *(volatile ULONG *)(e + 12) != 2) continue;   /* CODEVIEW */
            rsds = b + *(volatile ULONG *)(e + 20);
            if (!Valid(rsds, 24 + 128) || *(volatile ULONG *)rsds != 0x53445352) continue;  /* 'RSDS' */
            for (k = 0; k < 128 && ((volatile char *)rsds)[24 + k]; k++)
                if (((volatile char *)rsds)[24 + k] == '\\') last = k + 1;
            for (k = 0; k + 1 < NameLen && ((volatile char *)rsds)[24 + last + k]; k++) Name[k] = ((volatile char *)rsds)[24 + last + k];
            Name[k] = 0;
            break;
        }
        return b;
    }
    return 0;
}

static ULONG gStackStart;
#define STACK_SCAN (24 * 1024)

/* Exception address details: instruction halfwords, owning image, and the caller from the trap frame /
   CONTEXT on the stack (the word before Pc is Lr in a CONTEXT, two before in a KTRAP_FRAME). */
static VOID ExceptionDetails(PULONG fb, ULONG Pc)
{
    ULONG a = Pc & ~1UL, base, gap, off, i;
    ULONG_PTR sp, end;
    char name[24];

    PutS(fb, "at ", 0x00FFFFFF); PutHex(fb, a, 8, 0x0000FFFF); PutS(fb, ":", 0x00FFFFFF);
    for (i = 0; i < 5; i++) {
        ULONG h = a - 4 + i * 2;
        PutCh(fb, (char)(i == 2 ? '[' : ' '), 0x00FFFFFF);
        if (Valid(h, 2)) PutHex(fb, *(volatile USHORT *)h, 4, 0x00FFFF00); else PutS(fb, "----", 0x00808080);
        if (i == 2) PutCh(fb, ']', 0x00FFFFFF);
    }
    PutS(fb, "  ", 0);
    if (Lookup(a, &off) != NULL) PutAddr(fb, a, 0x0000FF00);
    else {
        base = FindImage(a, name, sizeof(name), &gap);
        if (base != 0) {
            PutS(fb, "img ", 0x00FFFFFF); PutS(fb, name[0] ? name : "?", 0x0000FF00);
            PutCh(fb, '+', 0x0000FF00); PutHex(fb, a - base, 6, 0x0000FF00);
        } else {
            PutS(fb, "no image", 0x00FF6060);
            if (gap) { PutS(fb, " gap ", 0x00FFFFFF); PutHex(fb, gap, 8, 0x0000FFFF); }
        }
    }
    PutCh(fb, '\n', 0);

    PutS(fb, "caller:", 0x00FFFFFF);
    sp = gStackStart; end = sp + STACK_SCAN;
    for (i = 0; sp < end && i < 2; sp += 4) {
        ULONG v, k;
        if (((sp & (PAGE_SIZE - 1)) == 0 || sp == gStackStart) && !MmIsAddressValid((PVOID)sp)) break;
        v = *(volatile ULONG *)sp;
        if ((v | 1) != (Pc | 1) || sp < gStackStart + 16) continue;
        for (k = 1; k <= 3; k++) {
            ULONG lr = *(volatile ULONG *)(sp - 4 * k);
            if ((lr & 1) && lr >= 0x80000000UL) {
                PutS(fb, " LR", 0x00FFFFFF); PutCh(fb, (char)('0' + k), 0x00FFFFFF); PutCh(fb, ' ', 0);
                if (!PutAddr(fb, (lr & ~1UL) - 2, 0x0000FF00)) PutHex(fb, lr, 8, 0x0000FFFF);
            }
        }
        i++;
    }
    if (i == 0) PutS(fb, " (no trap frame found)", 0x00808080);
    PutCh(fb, '\n', 0);
}

static const char *StopName(ULONG Code)
{
    switch (Code) {
    case 0x0A: return "IRQL_NOT_LESS_OR_EQUAL";
    case 0x1E: case 0x1000001E: return "KMODE_EXCEPTION_NOT_HANDLED";
    case 0x3B: return "SYSTEM_SERVICE_EXCEPTION";
    case 0x50: return "PAGE_FAULT_IN_NONPAGED_AREA";
    case 0x5C: return "HAL_INITIALIZATION_FAILED";
    case 0x7B: return "INACCESSIBLE_BOOT_DEVICE";
    case 0x7E: case 0x1000007E: return "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED";
    case 0x7F: return "UNEXPECTED_KERNEL_MODE_TRAP";
    case 0x8E: case 0x1000008E: return "KERNEL_MODE_EXCEPTION_NOT_HANDLED";
    case 0x9F: return "DRIVER_POWER_STATE_FAILURE";
    case 0xC4: return "DRIVER_VERIFIER_DETECTED_VIOLATION";
    case 0xD1: return "DRIVER_IRQL_NOT_LESS_OR_EQUAL";
    case 0xE2: return "MANUALLY_INITIATED_CRASH (test)";
    case 0x133: return "DPC_WATCHDOG_VIOLATION";
    case 0x139: return "KERNEL_SECURITY_CHECK_FAILURE";
    default: return "";
    }
}

static VOID Draw(PULONG fb)
{
    ULONG code, p[4], i, hits = 0;
    ULONG_PTR sp, end;

    code = (ULONG)gBugData[0];
    for (i = 0; i < 4; i++) p[i] = (ULONG)gBugData[i + 1];

    if (fb != NULL) GtRect(fb, 0, BOX_Y, GT_FB_W, GT_FB_H - BOX_Y, 0);
    gCol = 0; gRow = 0;
    PutS(fb, "goyabug  ", 0x00FF40FF); PutS(fb, StopName(code), 0x00FFFFFF);
    {
        ULONG up = (ULONG)(KeQueryInterruptTime() / 10000000ULL);
        PEPROCESS pr = IoGetCurrentProcess();
        const char *pn = (pr != NULL) ? (const char *)PsGetProcessImageFileName(pr) : NULL;
        PutS(fb, "  up ", 0x00FFFFFF); PutDec(fb, up, 0x0000FFFF); PutS(fb, "s", 0x00FFFFFF);
        if (pn != NULL && Valid((ULONG_PTR)pn, 15)) { PutS(fb, "  ", 0); PutS(fb, pn, 0x00FFFFFF); }
        if (gActive >= 0) {
            ULONG c = ListCount(gList[gActive]);
            MODINFO *m = &((MODLIST *)gList[gActive])->M[c ? c - 1 : 0];
            USHORT o = m->OffsetToFileName;
            PutS(fb, "  last ", 0x00FFFFFF); if (c) PutS(fb, (const char *)m->FullPathName + (o < 255 ? o : 0), 0x00C0C0C0);
        }
    }
    PutCh(fb, '\n', 0);
    PutS(fb, "STOP ", 0x00FFFFFF); PutHex(fb, code, 8, 0x00FFFF00);
    for (i = 0; i < 4; i++) {
        PutS(fb, "  P", 0x00FFFFFF); PutCh(fb, (char)('1' + i), 0x00FFFFFF); PutCh(fb, ' ', 0);
        PutHex(fb, p[i], 8, 0x0000FFFF);
    }
    PutCh(fb, '\n', 0);
    if (gActive < 0) PutS(fb, "(no module list yet)\n", 0x00808080);

    for (i = 0; i < 4; i++) {
        ULONG c0 = gCol, l0 = gHdr != NULL ? gHdr->CrashUsed : 0;
        PutS(fb, "P", 0x00FFFFFF); PutCh(fb, (char)('1' + i), 0x00FFFFFF); PutCh(fb, ' ', 0);
        if (PutAddr(fb, p[i], 0x0000FF00)) { PutS(fb, "   ", 0); hits++; }
        else { gCol = c0; if (gCrashLog) gHdr->CrashUsed = l0; }  /* not an address: take it back */
    }
    if (hits) PutCh(fb, '\n', 0);

    /* Exception stops: P2 is the exception address */
    if (code == 0x1E || code == 0x1000001E || code == 0x7E || code == 0x1000007E || code == 0x8E || code == 0x1000008E)
        ExceptionDetails(fb, p[1]);

    /* SYSTEM_THREAD_EXCEPTION_NOT_HANDLED: P3 = exception record, P4 = context record */
    if ((code == 0x7E || code == 0x1000007E) && p[3] >= 0x80000000UL && MmIsAddressValid((PVOID)(ULONG_PTR)p[3])) {
        PCONTEXT ctx = (PCONTEXT)(ULONG_PTR)p[3];
        PutS(fb, "PC ", 0x00FFFFFF);
        if (!PutAddr(fb, ctx->Pc, 0x0000FF00)) PutHex(fb, ctx->Pc, 8, 0x0000FFFF);
        PutS(fb, "  LR ", 0x00FFFFFF);
        if (!PutAddr(fb, ctx->Lr & ~1UL, 0x0000FF00)) PutHex(fb, ctx->Lr, 8, 0x0000FFFF);
        PutCh(fb, '\n', 0);
    }

    /* Heuristic call chain: odd words (Thumb return addresses) on this stack that lie in a module.
       Pages are checked before they are read, so the scan stops at the end of the stack. */
    PutS(fb, "stack:", 0x00FFFFFF);
    sp = gStackStart;
    end = sp + STACK_SCAN;
    hits = 0;
    for (; sp < end && hits < 14 && gRow < BOX_LINES; sp += 4) {
        ULONG v, off;
        if ((sp & (PAGE_SIZE - 1)) == 0 || sp == gStackStart) {
            if (!MmIsAddressValid((PVOID)sp)) break;
        }
        v = *(volatile ULONG *)sp;
        if (!(v & 1) || Lookup(v & ~1UL, &off) == NULL) continue;
        if (gCol + 24 > COLS) PutCh(fb, '\n', 0);
        PutCh(fb, ' ', 0);
        PutAddr(fb, (v & ~1UL) - 2, (hits & 1) ? 0x00C0C0C0 : 0x00FFFFFF);   /* -2: the call, not the return */
        hits++;
    }
}

/* Watchdog reset, the firmware's sequence (Pxa988ResetSystemLib): hold the CP, enable the WDT clock,
   arm the WDT for a reset a few ticks from now. A warm reset keeps the DRAM, so the log survives. */
#define MPMU_PHYS    0xD4050000ULL
#define WDT_PHYS     0xD4080000ULL
static volatile UCHAR *gMpmu, *gWdt;
static ULONG gWdtWasArmed;
#define MPMU(o) (*(volatile ULONG *)(gMpmu + (o)))
#define WDT(o)  (*(volatile ULONG *)(gWdt + (o)))
#define WDT_UNLOCK() (WDT(0x9C) = 0xBABA, WDT(0xA0) = 0xEB10)

static VOID WatchdogReset(VOID)
{
    ULONG v;
    if (gMpmu == NULL || gWdt == NULL) return;
    KeMemoryBarrier();
    MPMU(0x1020) |= 1;                       /* APRR.CPR: hold the CP */
    KeStallExecutionProcessor(10);
    MPMU(0x20) |= (1 << 2) | (1 << 3);       /* CPRR: reset the MSA */
    KeStallExecutionProcessor(10);
    MPMU(0x200) = 7; (VOID)MPMU(0x200);      /* WDTPCR: reset + enable the WDT clock */
    MPMU(0x200) = 3; (VOID)MPMU(0x200);
    WDT_UNLOCK(); WDT(0x64) = 3;             /* WMER: enable, reset on match */
    MPMU(0x1020) |= 1 << 4;                  /* APRR.WDTR */
    WDT_UNLOCK(); WDT(0x70) = 0;             /* WSR */
    v = WDT(0x6C);                           /* WVR */
    WDT_UNLOCK(); WDT(0x68) = (v + 0x20) & 0xFFFF;   /* WMR */
    for (;;) KeStallExecutionProcessor(1000);
}

#define RESET_AFTER 20                       /* redraws of 0.5 s before the watchdog reset */

/* Runs at HIGH_LEVEL during the stop. Marks entry first (magenta bar, bottom right), writes the stop
   text to the log, then keeps redrawing the box (the stop screen may be painted or updated after the
   callbacks run) and resets the tablet after ~10 s so the firmware can save the log. Never returns. */
static VOID BugCheckCallback(PVOID Buffer, ULONG Length)
{
    PULONG fb = gGtFb;
    ULONG local, i;
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    if (fb != NULL) GtRect(fb, GT_FB_W - 64, GT_FB_H - 8, 64, 8, 0x00FF00FF);
    if (gBugData == NULL) {
        if (gHdr != NULL) { gHdr->StopCode = 0xFFFFFFFF; gHdr->State = GOYALOG_CRASHED; }
        if (fb == NULL) return;
        for (i = 0; i < RESET_AFTER; i++) KeStallExecutionProcessor(500000);
        WatchdogReset();
        return;
    }
    gStackStart = (ULONG)(ULONG_PTR)&local & ~3UL;
    if (gHdr != NULL) {
        gHdr->StopCode = (ULONG)gBugData[0];
        gHdr->CrashUsed = 0;
        gCrashLog = TRUE;
        Draw(fb);
        gCrashLog = FALSE;
        gHdr->Uptime = (ULONG)(KeQueryInterruptTime() / 10000000ULL);
        gHdr->State = GOYALOG_CRASHED;
    }
    for (i = 0;; i++) {
        if (fb != NULL) Draw(fb);
        KeStallExecutionProcessor(500000);
        if (i == RESET_AFTER && gHdr != NULL) WatchdogReset();
    }
}

/* ---- log sources ------------------------------------------------------------------------------- */

/* Every image Windows maps: drivers (ProcessId 0 / system images) and program .exe files. */
static VOID ImageNotify(PUNICODE_STRING Name, HANDLE ProcessId, PIMAGE_INFO Info)
{
    LINE l;
    BOOLEAN sys = (ProcessId == NULL) || (Info != NULL && Info->SystemModeImage);
    if (!sys) {
        USHORT n = (Name != NULL && Name->Buffer != NULL) ? Name->Length / sizeof(WCHAR) : 0;
        if (n < 4 || (Name->Buffer[n - 1] | 0x20) != L'e' || (Name->Buffer[n - 2] | 0x20) != L'x' ||
            (Name->Buffer[n - 3] | 0x20) != L'e' || Name->Buffer[n - 4] != L'.') return;
    }
    LBegin(&l);
    LS(&l, sys ? "load " : "exe  "); LU(&l, Name);
    if (sys && Info != NULL) { LS(&l, "  base "); LH(&l, (ULONG)(ULONG_PTR)Info->ImageBase, 8); LS(&l, " size "); LH(&l, (ULONG)Info->ImageSize, 6); }
    if (!sys) { LS(&l, "  pid "); LD(&l, (ULONG)(ULONG_PTR)ProcessId); }
    LEnd(&l);
}

/* DbgPrint output (only what the kernel passes to print callbacks) */
typedef VOID (*GOYA_DBGPRINT_CB)(PSTRING Output, ULONG ComponentId, ULONG Level);
typedef NTSTATUS (NTAPI *GOYA_SETDBGCB)(GOYA_DBGPRINT_CB Callback, BOOLEAN Enable);
typedef NTSTATUS (NTAPI *GOYA_SETFILTER)(ULONG ComponentId, ULONG Level, BOOLEAN State);

static VOID DbgPrintCallback(PSTRING Output, ULONG ComponentId, ULONG Level)
{
    LINE l;
    USHORT i;
    UNREFERENCED_PARAMETER(ComponentId);
    UNREFERENCED_PARAMETER(Level);
    if (Output == NULL || Output->Buffer == NULL) return;
    LBegin(&l);
    LS(&l, "dbg  ");
    for (i = 0; i < Output->Length; i++) {
        char c = Output->Buffer[i];
        if (c == '\n' || c == '\r') continue;
        LC(&l, (c >= 32 && c < 127) ? c : '?');
    }
    LEnd(&l);
}

static NTSTATUS ShutdownDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    LogS("shutdown / restart");
    if (gHdr != NULL) {
        gHdr->Uptime = (ULONG)(KeQueryInterruptTime() / 10000000ULL);
        gHdr->State = GOYALOG_SHUTDOWN;
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ---- second core: what Windows made of it -------------------------------------------------------
   The firmware leaves CPU1 in its ACPI parking-protocol page (0x08181000; CPU0's is 0x08180000)
   and the MADT lists it. Logged: the processor counts, the 8520 hal.dll's processor table (built
   from the MADT GICC entries; layout and offsets read from its code, this build only) and the two
   parking pages, where the HAL writes JumpAddress (+8) and ProcessorId (+0) to start a core. */
#define PARK_PHYS        0x08180000ULL
#define HAL8520_SIZE     0x3A000
#define HAL8520_PROC     0x1CBF8      /* struct: +0 table (40-byte entries), +0x24 entries */
#define HAL8520_STARTED  0x1C2B4      /* processors started */
#define HAL8520_INTCTRL  0x1CC0C      /* primary interrupt controller: +0x60 type (3 = GIC) */

static VOID LWords(LINE *l, const ULONG *p, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++) { LC(l, ' '); LH(l, p[i], 8); }
}

static ULONG HalBase(VOID)
{
    MODLIST *m;
    ULONG i;
    if (gActive < 0) return 0;
    m = (MODLIST *)gList[gActive];
    for (i = 0; i < ListCount(gList[gActive]); i++) {
        USHORT o = m->M[i].OffsetToFileName;
        const char *nm = (const char *)m->M[i].FullPathName + (o < 255 ? o : 0);
        if (_stricmp(nm, "hal.dll") == 0)
            return m->M[i].ImageSize == HAL8520_SIZE ? (ULONG)(ULONG_PTR)m->M[i].ImageBase : 0;
    }
    return 0;
}

static VOID SmpProbe(const char *When)
{
    PHYSICAL_ADDRESS pa;
    PULONG park;
    ULONG hal, i;
    LINE l;

    LBegin(&l); LS(&l, "smp "); LS(&l, When); LS(&l, ": active ");
    LD(&l, KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)); LS(&l, ", max ");
    LD(&l, KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS)); LS(&l, ", this CPU ");
    LD(&l, KeGetCurrentProcessorNumberEx(NULL)); LEnd(&l);

    hal = HalBase();
    if (hal != 0 && MmIsAddressValid((PVOID)(ULONG_PTR)(hal + HAL8520_PROC))) {
        const ULONG *s = (const ULONG *)(ULONG_PTR)(hal + HAL8520_PROC);
        const ULONG *ic = *(const ULONG * const *)(ULONG_PTR)(hal + HAL8520_INTCTRL);
        LBegin(&l); LS(&l, "smp hal "); LH(&l, hal, 8); LS(&l, " proc struct:"); LWords(&l, s, 12); LEnd(&l);
        LBegin(&l); LS(&l, "smp hal started "); LH(&l, *(const ULONG *)(ULONG_PTR)(hal + HAL8520_STARTED), 8);
        LS(&l, ", interrupt controller type ");
        if (ic != NULL && MmIsAddressValid((PVOID)(ic + 0x18))) LH(&l, ic[0x18], 8); else LS(&l, "?");
        LEnd(&l);
        if (s[0] != 0 && s[9] <= 4 && MmIsAddressValid((PVOID)(ULONG_PTR)s[0])) {
            for (i = 0; i < s[9]; i++) {
                LBegin(&l); LS(&l, "smp hal cpu "); LD(&l, i); LC(&l, ':');
                LWords(&l, (const ULONG *)(ULONG_PTR)(s[0] + i * 40), 10); LEnd(&l);
            }
        }
    } else {
        LogS("smp hal: not the 8520 hal.dll layout, not read");
    }

    pa.QuadPart = PARK_PHYS;
    park = (PULONG)MmMapIoSpace(pa, 0x2000, MmNonCached);
    if (park != NULL) {
        for (i = 0; i < 2; i++) {
            const ULONG *p = park + i * 0x400;
            LBegin(&l); LS(&l, "smp parking page "); LD(&l, i); LC(&l, ':'); LWords(&l, p, 8);
            LS(&l, " | +7F8"); LWords(&l, p + 0x1FE, 2);
            LS(&l, " | mailbox state "); LH(&l, p[0x3C9], 8); LS(&l, " wakes "); LD(&l, p[0x3CA]);
            LEnd(&l);
        }
        MmUnmapIoSpace(park, 0x2000);
    }
}

static VOID LogInit(PDRIVER_OBJECT Driver)
{
    PHYSICAL_ADDRESS pa;
    UNICODE_STRING n;
    RTL_OSVERSIONINFOW v;
    PDEVICE_OBJECT dev;
    ULONG boot = 1, i;
    LINE l;

    pa.QuadPart = GOYALOG_PHYS;
    gLog = (volatile UCHAR *)MmMapIoSpace(pa, GOYALOG_SIZE, MmNonCached);
    pa.QuadPart = MPMU_PHYS; gMpmu = (volatile UCHAR *)MmMapIoSpace(pa, 0x2000, MmNonCached);
    pa.QuadPart = WDT_PHYS;  gWdt  = (volatile UCHAR *)MmMapIoSpace(pa, 0x1000, MmNonCached);
    /* the firmware arms the watchdog at ExitBootServices as a hang guard for the early boot (second
       core bring-up); goyabug loading means the boot got this far */
    if (gWdt != NULL) {
        gWdtWasArmed = WDT(0x64);
        if (gWdtWasArmed & 1) { WDT_UNLOCK(); WDT(0x64) = 0; }
    }
    if (gLog == NULL) return;

    {
        GOYALOG_HDR *h = (GOYALOG_HDR *)gLog;
        if (h->Magic0 == GOYALOG_MAGIC0 && h->Magic1 == GOYALOG_MAGIC1 && h->Boot < 100000) boot = h->Boot + 1;
        h->Magic0 = 0;                                   /* invalid while it is being reset */
        h->State = GOYALOG_RUNNING;
        h->Boot = boot;
        h->Used = 0; h->Uptime = 0; h->CrashUsed = 0; h->StopCode = 0;
        for (i = 0; i < GOYALOG_SLOTS; i++) { h->Slot[i].Site = 0; h->Slot[i].Count = 0; h->Slot[i].ExcCode = 0; h->Slot[i].ExcInfo = 0; }
        h->Magic1 = GOYALOG_MAGIC1;
        h->Magic0 = GOYALOG_MAGIC0;
        gHdr = h;
    }

    v.dwOSVersionInfoSize = sizeof(v);
    LBegin(&l); LS(&l, "goyabug: boot "); LD(&l, boot);
    if (NT_SUCCESS(RtlGetVersion(&v))) { LS(&l, ", Windows "); LD(&l, v.dwMajorVersion); LC(&l, '.'); LD(&l, v.dwMinorVersion); LC(&l, '.'); LD(&l, v.dwBuildNumber); }
    LEnd(&l);

    /* drivers the boot loader loaded before goyabug */
    if (gActive >= 0) {
        MODLIST *m = (MODLIST *)gList[gActive];
        for (i = 0; i < ListCount(gList[gActive]); i++) {
            USHORT o = m->M[i].OffsetToFileName;
            LBegin(&l); LS(&l, "boot "); LS(&l, (const char *)m->M[i].FullPathName + (o < 255 ? o : 0));
            LS(&l, "  base "); LH(&l, (ULONG)(ULONG_PTR)m->M[i].ImageBase, 8); LS(&l, " size "); LH(&l, m->M[i].ImageSize, 6);
            LEnd(&l);
        }
    }

    LBegin(&l); LS(&l, "image notify ");
    LS(&l, NT_SUCCESS(PsSetLoadImageNotifyRoutine(ImageNotify)) ? "on" : "FAILED"); LEnd(&l);

    {
        GOYA_SETDBGCB setcb; GOYA_SETFILTER setf; NTSTATUS st = STATUS_PROCEDURE_NOT_FOUND;
        RtlInitUnicodeString(&n, L"DbgSetDebugPrintCallback");
        setcb = (GOYA_SETDBGCB)MmGetSystemRoutineAddress(&n);
        RtlInitUnicodeString(&n, L"DbgSetDebugFilterState");
        setf = (GOYA_SETFILTER)MmGetSystemRoutineAddress(&n);
        if (setcb != NULL) st = setcb(DbgPrintCallback, TRUE);
        if (setf != NULL) {
            setf(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, TRUE);       /* plain DbgPrint */
            setf(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, TRUE);
        }
        LBegin(&l); LS(&l, "DbgPrint capture "); LS(&l, NT_SUCCESS(st) ? "on" : "not available"); LEnd(&l);
    }

    if (NT_SUCCESS(IoCreateDevice(Driver, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev))) {
        Driver->MajorFunction[IRP_MJ_SHUTDOWN] = ShutdownDispatch;
        dev->Flags &= ~DO_DEVICE_INITIALIZING;
        LogS(NT_SUCCESS(IoRegisterShutdownNotification(dev)) ? "shutdown notification on" : "shutdown notification FAILED");
    }

    LBegin(&l); LS(&l, "firmware watchdog (WMER "); LH(&l, gWdtWasArmed, 2);
    LS(&l, (gWdtWasArmed & 1) ? ") was armed, disarmed" : ") was not armed"); LEnd(&l);
    SmpProbe("at goyabug start");
}

/* ---- entry ------------------------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING n;
    HANDLE h;
    UNREFERENCED_PARAMETER(RegistryPath);            /* no DriverUnload: stays loaded */

    GtFb();                                          /* map the framebuffer now: not possible at stop time */
    RtlInitUnicodeString(&n, L"KiBugCheckData");
    gBugData = (PULONG_PTR)MmGetSystemRoutineAddress(&n);
    gList[0] = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, LIST_BYTES, 'gubG');
    gList[1] = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, LIST_BYTES, 'gubG');
    Snapshot();
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = GPIO_PHYS;
        gGplr = (PULONG)MmMapIoSpace(pa, sizeof(ULONG), MmNonCached);
    }
    LogInit(Driver);                                 /* after Snapshot(): logs the boot drivers */

    KeInitializeCallbackRecord(&gRecord);
    if (!KeRegisterBugCheckCallback(&gRecord, BugCheckCallback, NULL, 0, (PUCHAR)"goyabug")) {
        GoyaTraceStep(0xEE);
        return STATUS_UNSUCCESSFUL;
    }
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, SnapshotThread, NULL))) ZwClose(h);
    GoyaTraceStep(gBugData != NULL && gActive >= 0 ? 0x01 : 0x0E);
    return STATUS_SUCCESS;
}
