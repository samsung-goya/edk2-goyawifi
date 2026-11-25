/* On-screen trace for the Goya drivers (bring-up aid; the Windows 8 build 8520 stop screen shows no
   parameters and WinPE keeps no crash dump).

   - GoyaTraceStep(Site): every guarded callback writes "<driver> <site>" on its own line at the top
     right of the screen, so the last callback each driver entered is visible (in a photo, or in the
     last video frame before a stop screen).
   - GT_GUARD: callbacks run inside __try/__except. An exception in our code (or in a routine we call)
     is caught instead of becoming KMODE/SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, the callback fails, and
     a red line at the top left shows: driver, site, exception code, offset of the faulting address in
     the driver image (or the raw address if it is outside), raw address.

   Driver ids: 1 goyasdhc, 2 goyabatt (was goyapinmux), 3 goyagpio, 4 goyai2c, 5 goyatouch, 6 goyakeys, 7 goyabug, 8 goyasdio.
   Framebuffer: 0x17000000, 1024 x 600, 32 bpp (reserved by the firmware).

   Header-only (static); define GOYA_TRACE_ID before including. */
#pragma once
#include <ntddk.h>
#include "goyalog.h"

#ifndef GOYA_TRACE_ID
#error define GOYA_TRACE_ID
#endif

#define GT_FB_PHYS   0x17000000ULL
#define GT_FB_W      1024
#define GT_FB_H      600
#define GT_SCALE     3

extern UCHAR __ImageBase;      /* linker-provided start of this driver image */

static volatile PULONG gGtFb;
static volatile LONG   gGtCaught;
static GOYALOG_HDR * volatile gGtLog;

/* Header of the persistent log (goyalog.h): this driver's slot. Mapped on first use at
   <= DISPATCH_LEVEL; plain stores only (no interlocked operations on uncached memory). */
static GOYALOG_HDR *GtLog(VOID)
{
    PHYSICAL_ADDRESS pa;
    if (gGtLog == NULL && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        GOYALOG_HDR *p;
        pa.QuadPart = GOYALOG_PHYS;
        p = (GOYALOG_HDR *)MmMapIoSpace(pa, PAGE_SIZE, MmNonCached);
        if (p != NULL && InterlockedCompareExchangePointer((PVOID volatile *)&gGtLog, p, NULL) != NULL) {
            MmUnmapIoSpace(p, PAGE_SIZE);
        }
    }
    return gGtLog;
}

/* 3x5 hex glyphs, 15 bits, row-major, MSB = top-left */
static const USHORT gGtGlyph[16] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292,
    0x7BEF, 0x7BCF, 0x2BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4
};

static PULONG GtFb(VOID)
{
    PHYSICAL_ADDRESS pa;
    if (gGtFb == NULL && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        PULONG p;
        pa.QuadPart = GT_FB_PHYS;
        p = (PULONG)MmMapIoSpace(pa, GT_FB_W * GT_FB_H * 4, MmNonCached);
        if (p != NULL && InterlockedCompareExchangePointer((PVOID volatile *)&gGtFb, p, NULL) != NULL) {
            MmUnmapIoSpace(p, GT_FB_W * GT_FB_H * 4);
        }
    }
    return gGtFb;
}

static VOID GtRect(PULONG Fb, ULONG X, ULONG Y, ULONG W, ULONG H, ULONG Color)
{
    ULONG x, y;
    for (y = Y; y < Y + H && y < GT_FB_H; y++)
        for (x = X; x < X + W && x < GT_FB_W; x++)
            Fb[y * GT_FB_W + x] = Color;
}

/* Draw Digits hex digits of Value at (X, Y); returns the next X. */
static ULONG GtHex(PULONG Fb, ULONG X, ULONG Y, ULONG Value, ULONG Digits, ULONG Fg, ULONG Bg)
{
    ULONG d, r, c;
    GtRect(Fb, X, Y, Digits * 4 * GT_SCALE + GT_SCALE * 2, 5 * GT_SCALE, Bg);
    for (d = 0; d < Digits; d++) {
        USHORT g = gGtGlyph[(Value >> (4 * (Digits - 1 - d))) & 0xF];
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (g & (1u << (14 - (r * 3 + c))))
                    GtRect(Fb, X + (d * 4 + c) * GT_SCALE, Y + r * GT_SCALE, GT_SCALE, GT_SCALE, Fg);
    }
    return X + Digits * 4 * GT_SCALE + GT_SCALE * 2;
}

/* "<id> <site>" at the top right, one line per driver. */
/* Screen rows at the top right: step lines of drivers 1..7 in rows 0..6, then two value rows per driver
   from row 7 (driver 1: rows 7-8 ... driver 8: rows 21-22). A driver id above 7 gets its step line below
   the value rows (row 23 for goyasdio), not in row id-1, which is driver 1's first value row. */
#define GT_STEP_ROW  (GOYA_TRACE_ID <= 7 ? GOYA_TRACE_ID - 1 : 23 + (GOYA_TRACE_ID - 8))

static VOID GoyaTraceStep(ULONG Site)
{
    PULONG fb = GtFb();
    ULONG y = 8 + GT_STEP_ROW * 6 * GT_SCALE, x = GT_FB_W - 12 * 4 * GT_SCALE;
    GOYALOG_HDR *lg = GtLog();
    if (lg != NULL && GOYA_TRACE_ID < GOYALOG_SLOTS) {
        lg->Slot[GOYA_TRACE_ID].Site = Site;
        lg->Slot[GOYA_TRACE_ID].Count++;
    }
    if (fb == NULL) return;
    x = GtHex(fb, x, y, GOYA_TRACE_ID, 1, 0x00FFFFFF, 0x00202020);
    GtHex(fb, x, y, Site, 2, 0x0000FF00, 0x00202020);
}

/* A 32-bit value at the top right, below the step lines (Slot 0..1 per driver). */
static VOID GoyaTraceValue(ULONG Slot, ULONG Value)
{
    PULONG fb = GtFb();
    ULONG y = 8 + (7 + (GOYA_TRACE_ID - 1) * 2 + Slot) * 6 * GT_SCALE, x = GT_FB_W - 12 * 4 * GT_SCALE;
    if (fb == NULL || Slot > 1) return;
    x = GtHex(fb, x, y, GOYA_TRACE_ID, 1, 0x00FFFFFF, 0x00202020);
    x = GtHex(fb, x, y, Slot, 1, 0x00808080, 0x00202020);
    GtHex(fb, x, y, Value, 8, 0x0000FFFF, 0x00202020);
}

/* Exception filter: report once per driver and handle the exception. */
static LONG GoyaTraceFilter(PEXCEPTION_POINTERS Ep, ULONG Site)
{
    PULONG fb;
    ULONG code = 0, addr = 0, off, base = (ULONG)(ULONG_PTR)&__ImageBase;
    if (Ep != NULL && Ep->ExceptionRecord != NULL) {
        code = (ULONG)Ep->ExceptionRecord->ExceptionCode;
        addr = (ULONG)(ULONG_PTR)Ep->ExceptionRecord->ExceptionAddress;
    }
    off = (addr >= base && addr - base < 0x100000) ? addr - base : 0xFFFFFFFF;
    DbgPrint("goya%u: exception %08x at %08x (image+%08x) in site %02x\n", GOYA_TRACE_ID, code, addr, off, Site);
    {
        GOYALOG_HDR *lg = gGtLog;                 /* already mapped by GoyaTraceStep (any IRQL here) */
        if (lg != NULL && GOYA_TRACE_ID < GOYALOG_SLOTS && lg->Slot[GOYA_TRACE_ID].ExcCode == 0) {
            lg->Slot[GOYA_TRACE_ID].ExcInfo = (Site << 24) | (off & 0x00FFFFFF);
            lg->Slot[GOYA_TRACE_ID].ExcCode = code ? code : 1;
        }
    }
    if (InterlockedExchange(&gGtCaught, 1) == 0 && (fb = GtFb()) != NULL) {
        ULONG y = 8 + (GOYA_TRACE_ID - 1) * 6 * GT_SCALE, x = 8;
        x = GtHex(fb, x, y, GOYA_TRACE_ID, 1, 0x00FFFFFF, 0x00800000);
        x = GtHex(fb, x, y, Site, 2, 0x00FFFFFF, 0x00800000);
        x = GtHex(fb, x, y, code, 8, 0x00FFFF00, 0x00800000);
        x = GtHex(fb, x, y, off, 8, 0x00FFFFFF, 0x00800000);
        GtHex(fb, x, y, addr, 8, 0x00C0C0C0, 0x00800000);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Wrap a callback body:  GT_GUARD(site, statement, statement-on-exception) */
#define GT_GUARD(Site, Body, OnException) \
    do { GoyaTraceStep(Site); __try { Body; } __except (GoyaTraceFilter(GetExceptionInformation(), Site)) { OnException; } } while (0)
