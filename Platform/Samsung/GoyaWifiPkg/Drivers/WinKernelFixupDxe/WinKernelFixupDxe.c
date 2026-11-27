/** @file
  Windows 8 build 8520 (ARM) kernel fix-up for the PXA986.

  The Cortex-A9 in this SoC has its debug logic locked for software: every
  extended CP14 debug register (DBGOSLSR, DBGPRSR, DBGDSCRext, DBGVCR,
  DBGBCR/BVR/WCR) is UNDEFINED, and its memory-mapped view does not answer
  either (GoyaWifiDiag page 1). Samsung production parts fuse this off.

  The 8520 kernel initialises those registers unconditionally on every
  processor (it bugchecks 0x5D if ID_DFR0 reports no CP14 debug), so it takes
  an undefined instruction while starting and stops with
  DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4 / 0x91.

  At ExitBootServices winload has loaded and relocated ntoskrnl.exe but not
  started it. This driver finds the two calls to the per-processor debug
  register initialisation in the loaded image and replaces them with NOPs.
  The kernel then records 0 breakpoints/watchpoints for the processor
  (PRCB fields stay zero) and never touches the debug registers again:
  hardware breakpoints are simply unavailable, which nothing in WinPE needs.

  A third site keeps PatchGuard from touching DBGDSCRext: its check routines
  run minutes after boot from encrypted copies in pool and would otherwise
  stop Windows with KMODE_EXCEPTION_NOT_HANDLED C000001D (see mSites).

  Byte patterns are taken from ntoskrnl.exe 6.2.8520.0 (fbl_eeap.120722-1632).
  The code is position independent (relative branches, PC-relative
  literals), so the bytes are the same wherever the image was loaded. If the
  patterns are not found nothing is changed.

  A small square in the bottom-right corner of the screen reports the result
  (it stays visible behind the Windows logo and the stop screen):
    green  = all sites patched       yellow = only some sites found
    red    = not found (other kernel build or not Windows)
    cyan   = all sites patched, and an asynchronous abort was pending and has been taken here
    blue   = all sites patched, an asynchronous abort is pending and could not be taken here

  Asynchronous (imprecise) aborts: a RELEASE firmware runs with them masked (ArmPkg CpuDxe only
  unmasks them in DEBUG builds), so a bus error caused by firmware code stays pending and is
  delivered the moment the OS unmasks aborts - early in the kernel, before the HAL timer is set
  up, as KMODE_EXCEPTION_NOT_HANDLED. That matches the intermittent early stop seen with WinPE.
  At ExitBootServices this driver reads ISR.A and, if an abort is pending, unmasks aborts for a
  moment with its own Data Abort handler installed so the abort is consumed here.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>

UINT32 GoyaReadIsr (VOID);
UINT32 GoyaReadCpsr (VOID);
UINT32 GoyaReadDfsr (VOID);
VOID   GoyaTakeAsyncAbort (VOID);

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC volatile UINT32        mAbortTaken;
STATIC volatile UINT32        mAbortDfsr;

/**
  Data Abort handler used only around GoyaTakeAsyncAbort: note the abort and return to its caller
  with asynchronous aborts masked again (CPSR.A), Thumb state from lr.
**/
STATIC
VOID
EFIAPI
AsyncAbortHandler (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_ARM  *Ctx = SystemContext.SystemContextArm;

  mAbortTaken++;
  mAbortDfsr = GoyaReadDfsr ();
  Ctx->CPSR  = (Ctx->CPSR & ~(UINT32)(0x20 | 0x0600FC00)) | 0x100 | ((Ctx->LR & 1) ? 0x20 : 0);
  Ctx->PC    = Ctx->LR & ~(UINT32)1;
}

/**
  Returns 0 = no abort pending, 1 = a pending abort was taken here, 2 = pending, could not take it.
**/
STATIC
UINTN
FlushAsyncAbort (
  VOID
  )
{
  UINTN  Result;

  if ((GoyaReadIsr () & BIT8) == 0) {
    return 0;
  }
  if ((mCpu == NULL) || EFI_ERROR (mCpu->RegisterInterruptHandler (mCpu, EXCEPT_ARM_DATA_ABORT, AsyncAbortHandler))) {
    return 2;
  }
  mAbortTaken = 0;
  GoyaTakeAsyncAbort ();
  mCpu->RegisterInterruptHandler (mCpu, EXCEPT_ARM_DATA_ABORT, NULL);
  Result = ((mAbortTaken != 0) && ((GoyaReadIsr () & BIT8) == 0)) ? 1 : 2;
  DEBUG ((DEBUG_WARN, "WinKernelFixup: pending asynchronous abort %a (DFSR 0x%x)\n", (Result == 1) ? "taken" : "NOT cleared", mAbortDfsr));
  return Result;
}

typedef struct {
  CONST CHAR8    *Name;
  UINT8          Pattern[24];
  UINTN          Length;
  UINTN          PatchOffset;     // offset of the bytes to replace
  UINT8          Patch[4];
  UINTN          PatchLength;
} KERNEL_SITE;

STATIC CONST KERNEL_SITE  mSites[] = {
  //  mrc p15,0,r3,c0,c1,2 (ID_DFR0); ands r3,#0xf; cmp r3,#4; blo.w <bugcheck 0x5D>;
  //  bl <KiInitializeDebugRegisters>; pop {r2,r3,fp,pc}
  {
    "KiInitializeProcessor debug init",
    { 0x10, 0xee, 0x51, 0x3f, 0x13, 0xf0, 0x0f, 0x03, 0x04, 0x2b, 0xd3, 0xf0,
      0xf9, 0xad, 0x00, 0xf0, 0xd1, 0xf9, 0xbd, 0xe8, 0x0c, 0x88 },
    22, 14, { 0x00, 0xbf, 0x00, 0xbf }, 4                   // nop; nop
  },
  //  ldr r3,=<flag>; ldrb r3,[r3]; cbz r3,...; bl <KiInitializeDebugRegisters>
  {
    "resume path debug init",
    { 0x0f, 0x4b, 0x1b, 0x78, 0x9b, 0xb1, 0xff, 0xf7, 0x70, 0xfd },
    10, 6, { 0x00, 0xbf, 0x00, 0xbf }, 4                    // nop; nop
  },
  //  PatchGuard context setup (INIT): ldr r3,=<CP14 debug present flag>; ldrb r2,[r3];
  //  ldrb r3,[r8,#0x2e8]; eor r3,r3,r2,lsl #5; ands r2,r3,#0x20  -> context flag 0x20.
  //  With 0x20 set, the PatchGuard check routines (INITKDBG code that runs minutes after boot from
  //  encrypted copies in pool) save and clear DBGDSCRext with MRC/MCR p14,0,rX,c0,c2,2, which is
  //  UNDEFINED here: KMODE_EXCEPTION_NOT_HANDLED C000001D at an address in no module, after a few
  //  idle minutes. ldrb r2,[r3] -> movs r2,#0 keeps 0x20 clear in every context. INIT runs before
  //  PatchGuard takes its baseline and is discarded afterwards, so nothing checks this change.
  {
    "PatchGuard CP14 debug flag",
    { 0x1a, 0x78, 0x98, 0xf8, 0xe8, 0x32, 0x83, 0xea, 0x42, 0x13, 0x13, 0xf0, 0x20, 0x02 },
    14, 0, { 0x00, 0x22 }, 2                                // movs r2,#0
  },
  //  KeRestoreProcessorState, which loads a processor's state block (SCTLR, DACR, PRRR/NMRR, VBAR,
  //  TPIDRs) - the same block the HAL hands a secondary processor in its ACPI parking page - then,
  //  if the CP14-debug-present flag is set, saves/restores the debug registers (DBGOSLSR, DBGDSCRext,
  //  breakpoints), UNDEFINED here. On one processor only the resume path calls it; a starting second
  //  processor would fault there with interrupts off while the first waits for it.
  //  Both flag tests: ldrb.w r3,[r5,#-0x76] -> movs r3,#0; nop, so the debug part is always skipped.
  //    ldrb.w r3,[r5,#-0x76]; cmp r3,#0; beq; mrc p15,0,r6,c13,c0,4; bfc r6,#0,#12; mrc p14,0,r3,c1,c1,4
  {
    "KeRestoreProcessorState CP14 restore",
    { 0x15, 0xf8, 0x76, 0x3c, 0x00, 0x2b, 0x73, 0xd0, 0x1d, 0xee, 0x90, 0x6f, 0x6f, 0xf3, 0x0b, 0x06,
      0x11, 0xee, 0x91, 0x3e },
    20, 0, { 0x00, 0x23, 0x00, 0xbf }, 4                    // movs r3,#0; nop
  },
  //    ldrb.w r3,[r5,#-0x76]; cbz r3; mrc p14,0,r3,c0,c2,2; cmp r2,#0; bne.w ...
  {
    "KeRestoreProcessorState CP14 DSCR check",
    { 0x15, 0xf8, 0x76, 0x3c, 0x3b, 0xb1, 0x10, 0xee, 0x52, 0x3e, 0x00, 0x2a, 0x6f, 0xf0, 0x33, 0x8b },
    16, 0, { 0x00, 0x23, 0x00, 0xbf }, 4                    // movs r3,#0; nop
  },
};

#define SITE_COUNT  (sizeof (mSites) / sizeof (mSites[0]))

STATIC EFI_EVENT              mExitBootServicesEvent;
STATIC EFI_MEMORY_DESCRIPTOR  *mMap;
STATIC UINTN                  mMapBufferSize;

STATIC
VOID
DrawMarker (
  IN UINT32  Color
  )
{
  UINT32  *Fb     = (UINT32 *)(UINTN)FixedPcdGet32 (PcdFrameBufferBase);
  UINT32  Stride  = FixedPcdGet32 (PcdFrameBufferStride);
  UINT32  Width   = FixedPcdGet32 (PcdFrameBufferWidth);
  UINT32  Height  = FixedPcdGet32 (PcdFrameBufferHeight);
  UINT32  X, Y;

  for (Y = Height - 20; Y < Height - 4; Y++) {
    for (X = Width - 20; X < Width - 4; X++) {
      Fb[Y * Stride + X] = Color;
    }
  }

  WriteBackDataCacheRange (Fb + (Height - 20) * Stride, 16 * Stride * 4);
}

/**
  Scan one region (2-byte aligned, Thumb code) for every site.
**/
STATIC
VOID
ScanRegion (
  IN     UINT8  *Base,
  IN     UINTN  Size,
  IN OUT UINTN  *Found
  )
{
  UINT16  *P;
  UINT16  *End;
  UINTN   S;
  UINT16  First[SITE_COUNT];

  for (S = 0; S < SITE_COUNT; S++) {
    First[S] = (UINT16)(mSites[S].Pattern[0] | (mSites[S].Pattern[1] << 8));
  }

  End = (UINT16 *)(Base + Size - 32);
  for (P = (UINT16 *)Base; P < End; P++) {
    for (S = 0; S < SITE_COUNT; S++) {
      if ((*P == First[S]) && (CompareMem (P, mSites[S].Pattern, mSites[S].Length) == 0)) {
        UINT8  *Bl = (UINT8 *)P + mSites[S].PatchOffset;

        CopyMem (Bl, mSites[S].Patch, mSites[S].PatchLength);
        WriteBackInvalidateDataCacheRange (Bl, mSites[S].PatchLength);
        InvalidateInstructionCacheRange (Bl, mSites[S].PatchLength);
        Found[S]++;
        DEBUG ((DEBUG_INFO, "WinKernelFixup: patched %a at %p\n", mSites[S].Name, Bl));
      }
    }
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN                  MapSize = mMapBufferSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINT8                  *D;
  UINTN                  Found[SITE_COUNT];
  UINTN                  S;
  UINTN                  Sites = 0;
  UINTN                  Abort;

  ZeroMem (Found, sizeof (Found));
  if ((mMap == NULL) || EFI_ERROR (gBS->GetMemoryMap (&MapSize, mMap, &MapKey, &DescSize, &DescVer))) {
    DrawMarker (0x00FF00FF);    // magenta: could not read the memory map
    return;
  }

  for (D = (UINT8 *)mMap; D < (UINT8 *)mMap + MapSize; D += DescSize) {
    EFI_MEMORY_DESCRIPTOR  *Desc = (EFI_MEMORY_DESCRIPTOR *)D;

    // winload loads the kernel into loader memory; boot services data is scanned too in case
    // a loader allocates its image buffers there
    if (((Desc->Type == EfiLoaderCode) || (Desc->Type == EfiLoaderData) || (Desc->Type == EfiBootServicesData)) &&
        (Desc->NumberOfPages > 0))
    {
      ScanRegion ((UINT8 *)(UINTN)Desc->PhysicalStart, EFI_PAGES_TO_SIZE ((UINTN)Desc->NumberOfPages), Found);
    }
  }

  for (S = 0; S < SITE_COUNT; S++) {
    Sites += (Found[S] != 0) ? 1 : 0;
  }

  Abort = FlushAsyncAbort ();
  if (Sites != SITE_COUNT) {
    DrawMarker ((Sites != 0) ? 0x00FFFF00 : 0x00FF0000);
  } else {
    DrawMarker ((Abort == 0) ? 0x0000FF00 : (Abort == 1) ? 0x0000FFFF : 0x000000FF);
  }
}

EFI_STATUS
EFIAPI
WinKernelFixupDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN   MapSize = 0;
  UINTN   MapKey;
  UINTN   DescSize;
  UINT32  DescVer;

  // The map grows while the OS loader runs; leave plenty of room (no allocation is allowed at EBS)
  gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  mMapBufferSize = MapSize + 256 * DescSize;
  mMap           = AllocateRuntimePool (mMapBufferSize);
  gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);

  return gBS->CreateEvent (
                EVT_SIGNAL_EXIT_BOOT_SERVICES,
                TPL_NOTIFY,
                OnExitBootServices,
                NULL,
                &mExitBootServicesEvent
                );
}
