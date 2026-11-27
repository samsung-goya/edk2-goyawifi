/** @file
  Second core (CPU1) bring-up for the PXA988 / PXA986.

  Release sequence taken from the vendor kernels (4.19 platsmp-pxa988.c, 3.4
  reset-pxa988.c): the boot ROM starts CPU1 at the physical address in
  CIU_WARM_RESET_VECTOR once its reset bits in APMU_CC2_AP are cleared.

  CPU1 runs the stub in Cpu1Stub.S (MMU and caches off) from the firmware half
  of its ACPI parking-protocol page ("Multi-processor Startup for ARM
  Platforms"). After a self-test (alive, ping, heartbeat) it is left waiting
  for the OS in that page, and the page addresses are published with
  gPxa988Cpu1ParkingProtocolGuid for the MADT (GoyaWifiAcpiDxe). Windows on
  ARM 8.x starts secondary cores only this way.

  With PcdCpu1ParkingProtocol FALSE, or if anything fails, CPU1 is put back
  into reset (before ExitBootServices) and the MADT keeps it disabled.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Pxa988Cpu1Parking.h>
#include <Pxa988.h>

//
// Parking-protocol page layout (per core, 4 KiB)
//
#define PP_CPU_ID       0x000           // UINT32, 0xFFFFFFFF = no processor selected
#define PP_JUMP         0x008           // UINT64
#define PP_STUB         0x800           // firmware half: Cpu1Stub.S
#define MB_OFFSET       0xF00           // debug mailbox (Cpu1Stub.S MB_IN_PAGE)

#define MB_ALIVE        0x00
#define MB_MIDR         0x04
#define MB_MPIDR        0x08
#define MB_SCTLR        0x0C
#define MB_HEARTBEAT    0x10
#define MB_ACTLR        0x14
#define MB_ACK          0x18
#define MB_RESULT       0x20
#define MB_STATE        0x24
#define MB_WAKES        0x28
#define MB_CMD          0x40
#define MB_ARG          0x44
#define MB_GICCCTLR     0x48
#define MB_DIAG         0x4C            // CPU0's Cortex-A9 diagnostic register, set on CPU1 at the OS entry
#define ALIVE_MAGIC     0xA9A9C0DE

#define STATE_RUNNING   1
#define STATE_PARKED    2
#define STATE_OS_PARKED 3
#define STATE_OS_JUMPED 4
#define HALX_OFFSET     0xE00           // HalExtMmpTimer breadcrumbs in the CPU0 page
#define HALX_MAGIC      0x584C4148      // "HALX"

#define CMD_PARK        1
#define CMD_PING        2
#define CMD_OS_PARK     3

#define SCU_CTRL        0x00            // PXA988_SCU_BASE
#define SCU_CONFIG      0x04

extern CONST UINT8  Pxa988Cpu1StubStart[];
extern CONST UINT8  Pxa988Cpu1StubEnd[];

STATIC UINT8                         *mPages;     // CPU0's page, then CPU1's
STATIC UINT8                         *mPage;      // CPU1's page
STATIC UINT32                        mSavedVector;
STATIC UINT32                        mSavedCc2;
STATIC BOOLEAN                       mCpu1Released;
STATIC EFI_EVENT                     mExitBsEvent;
STATIC EFI_EVENT                     mEndOfDxeEvent;
STATIC CHAR8                         mReport[2048];
STATIC UINTN                         mReportLen;
STATIC PXA988_CPU1_PARKING_PROTOCOL  mParking;

STATIC
VOID
SendEvent (
  VOID
  )
{
  __asm__ volatile ("dsb\n\tsev" ::: "memory");
}

/**
  Print a line to the debug log and keep it for the "Cpu1Status" variable
  (shown by the UEFI Shell with: dmpstore Cpu1Status).
**/
STATIC
VOID
Log (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[256];
  UINTN    Len;

  VA_START (Args, Format);
  Len = AsciiVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);
  DEBUG ((DEBUG_INFO, "Pxa988Smp: %a", Line));
  if (mReportLen + Len < sizeof (mReport)) {
    CopyMem (mReport + mReportLen, Line, Len);
    mReportLen += Len;
  }
}

STATIC
VOID
EFIAPI
PublishReport (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);
  gRT->SetVariable (
         L"Cpu1Status",
         &gPxa988TokenSpaceGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         mReportLen,
         mReport
         );
}

STATIC
UINT32
MbRead (
  IN UINTN  Offset
  )
{
  //
  // CPU1 writes with caches off; drop whatever CPU0 has cached of the line
  //
  InvalidateDataCacheRange (mPage + MB_OFFSET, 0x40);        // CPU1's lines only
  return *(volatile UINT32 *)(mPage + MB_OFFSET + Offset);
}

STATIC
VOID
MbWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)(mPage + MB_OFFSET + Offset) = Value;
  WriteBackDataCacheRange (mPage + MB_OFFSET + 0x40, 0x40);   // CPU0's own lines only
  ArmDataSynchronizationBarrier ();
}

STATIC
BOOLEAN
MbWaitFor (
  IN UINTN   Offset,
  IN UINT32  Value,
  IN UINTN   TimeoutMs
  )
{
  while (TimeoutMs-- > 0) {
    if (MbRead (Offset) == Value) {
      return TRUE;
    }

    MicroSecondDelay (1000);
  }

  return MbRead (Offset) == Value;
}

STATIC
VOID
Cpu1Hold (
  VOID
  )
{
  MmioWrite32 (APMU_CC2_AP, MmioRead32 (APMU_CC2_AP) | CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST);
  MmioRead32 (APMU_CC2_AP);
}

STATIC
VOID
Cpu1Release (
  VOID
  )
{
  UINT32  Cc2;

  Cc2 = MmioRead32 (APMU_CC2_AP);
  if ((Cc2 & CC2_CPU1_RST) == 0) {
    //
    // The previous bootloader left CPU1 running: reset it first
    //
    MmioWrite32 (APMU_CC2_AP, Cc2 | CC2_CPU1_RST);
    MmioRead32 (APMU_CC2_AP);
    MicroSecondDelay (10);
    Cc2 = MmioRead32 (APMU_CC2_AP);
  }

  MmioWrite32 (APMU_CC2_AP, Cc2 & ~(CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST));
  MmioRead32 (APMU_CC2_AP);
  MmioWrite32 (APMU_CORE_WAKEUP_CTL, APMU_WAKEUP_CORE1);
  ArmDataSynchronizationBarrier ();
}

/**
  Put CPU1 back into reset and restore what the boot ROM vector held.
**/
STATIC
VOID
Cpu1Stop (
  VOID
  )
{
  if (!mCpu1Released) {
    return;
  }

  MbWrite (MB_CMD, CMD_PARK);
  SendEvent ();
  MbWaitFor (MB_STATE, STATE_PARKED, 20);

  MmioWrite32 (APMU_CC2_AP, (MmioRead32 (APMU_CC2_AP) & ~(CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST)) |
                            (mSavedCc2 & (CC2_CPU1_RST | CC2_CPU1_DBG_RST | CC2_CPU1_WDOG_RST)));
  Cpu1Hold ();
  MmioWrite32 (CIU_WARM_RESET_VECTOR, mSavedVector);
  mCpu1Released = FALSE;
  Log ("CPU1 held in reset\n");
}

//
// Hang guard while CPU1 is parked for the OS: the AP watchdog is armed at
// ExitBootServices for about WDT_GUARD_SECONDS, and goyabug (an early boot
// driver) disarms it when it loads. A boot that hangs before that ends in a
// watchdog reset, which keeps the DRAM, so this driver can report what CPU1
// did and goyalog can save the Windows log. The tick rate is measured here.
//
#define WDT_GUARD_SECONDS  240

STATIC UINT32     mWdtTicksPerSecond;
STATIC EFI_EVENT  mWdtEvent;

STATIC
VOID
WdtUnlock (
  VOID
  )
{
  MmioWrite32 (WDT_WFAR, 0xBABA);
  MmioWrite32 (WDT_WSAR, 0xEB10);
}

STATIC
VOID
WdtMeasure (
  VOID
  )
{
  UINT32  V0;
  UINT32  V1;

  MmioWrite32 (MPMU_WDTPCR, 0x7);           // reset + enable the WDT clock
  MmioRead32 (MPMU_WDTPCR);
  MmioWrite32 (MPMU_WDTPCR, 0x3);
  MmioRead32 (MPMU_WDTPCR);
  WdtUnlock ();
  MmioWrite32 (WDT_WSR, 0);
  WdtUnlock ();
  MmioWrite32 (WDT_WMR, 0xFFFF);
  WdtUnlock ();
  MmioWrite32 (WDT_WMER, 0x3);              // count (needs both bits); the match is 0xFFFF ticks away
  //
  // 1 ms first: a fast counter would reach the match (a reset) within the
  // 250 ms measurement
  //
  V0 = MmioRead32 (WDT_WVR) & 0xFFFF;
  MicroSecondDelay (1000);
  V1 = MmioRead32 (WDT_WVR) & 0xFFFF;
  if (((V1 - V0) & 0xFFFF) > 60) {
    WdtUnlock ();
    MmioWrite32 (WDT_WMER, 0);
    mWdtTicksPerSecond = 0;
    Log ("watchdog: counter too fast (%u ticks in 1 ms)\n", (V1 - V0) & 0xFFFF);
    return;
  }

  V0 = MmioRead32 (WDT_WVR) & 0xFFFF;
  MicroSecondDelay (250000);
  V1 = MmioRead32 (WDT_WVR) & 0xFFFF;
  WdtUnlock ();
  MmioWrite32 (WDT_WMER, 0);
  mWdtTicksPerSecond = ((V1 - V0) & 0xFFFF) * 4;
  Log ("watchdog: %u ticks/s (WVR %u -> %u in 250 ms)\n", mWdtTicksPerSecond, V0, V1);
}

STATIC
VOID
EFIAPI
WdtArmAtExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  Ticks;

  Ticks = mWdtTicksPerSecond * WDT_GUARD_SECONDS;
  if (Ticks > 0xFF00) {
    Ticks = 0xFF00;
  }

  WdtUnlock ();
  MmioWrite32 (WDT_WSR, 0);
  WdtUnlock ();
  MmioWrite32 (WDT_WMR, (MmioRead32 (WDT_WVR) + Ticks) & 0xFFFF);
  WdtUnlock ();
  MmioWrite32 (WDT_WMER, 0x3);              // enable, reset on match
  MmioOr32 (MPMU_APRR, MPMU_APRR_WDTR);
}

STATIC
VOID
WdtGuardSetup (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Seconds;

  WdtMeasure ();
  if ((mWdtTicksPerSecond < 1) || (mWdtTicksPerSecond > 0xFF00 / 30)) {
    Log ("watchdog: tick rate unusable, no hang guard\n");
    return;
  }

  Seconds = MIN (WDT_GUARD_SECONDS, 0xFF00 / mWdtTicksPerSecond);
  Status  = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, WdtArmAtExitBootServices, NULL, &gEfiEventExitBootServicesGuid, &mWdtEvent);
  Log ("watchdog: hang guard %u s from ExitBootServices (goyabug disarms it): %r\n", Seconds, Status);
}

STATIC
VOID
EFIAPI
Cpu1ExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  Cpu1Stop ();
}

/**
  The Cortex-A9 Snoop Control Unit keeps the two L1 data caches coherent. The
  OS expects it on; the vendor bootloader may not have enabled it on a boot
  that never started CPU1.
**/
STATIC
VOID
ScuEnable (
  VOID
  )
{
  UINT32  Ctrl;

  Ctrl = MmioRead32 (PXA988_SCU_BASE + SCU_CTRL);
  Log ("SCU ctrl=0x%08x config=0x%08x\n", Ctrl, MmioRead32 (PXA988_SCU_BASE + SCU_CONFIG));
  if ((Ctrl & BIT0) == 0) {
    MmioWrite32 (PXA988_SCU_BASE + SCU_CTRL, Ctrl | BIT0);
    ArmDataSynchronizationBarrier ();
    WriteBackInvalidateDataCache ();
    Log ("SCU enabled\n");
  }
}

/**
  Start CPU1 in its parking page and test it. Returns TRUE when CPU1 is
  running the stub and answered.
**/
STATIC
BOOLEAN
Cpu1Start (
  VOID
  )
{
  UINT32  Beat0;
  UINT32  Beat1;

  mSavedVector = MmioRead32 (CIU_WARM_RESET_VECTOR);
  mSavedCc2    = MmioRead32 (APMU_CC2_AP);
  Log ("CC2=0x%08x warm-reset vector=0x%08x, parking pages at 0x%08x\n",
    mSavedCc2,
    mSavedVector,
    (UINT32)(UINTN)mPages);

  MmioWrite32 (CIU_WARM_RESET_VECTOR, (UINT32)(UINTN)(mPage + PP_STUB));
  MmioRead32 (CIU_WARM_RESET_VECTOR);
  mCpu1Released = TRUE;
  Cpu1Release ();

  if (!MbWaitFor (MB_ALIVE, ALIVE_MAGIC, 500)) {
    Log ("CPU1 did not come up (CC2=0x%08x)\n", MmioRead32 (APMU_CC2_AP));
    Cpu1Hold ();
    MmioWrite32 (CIU_WARM_RESET_VECTOR, mSavedVector);
    mCpu1Released = FALSE;
    return FALSE;
  }

  Log ("CPU1 is up: MIDR=0x%08x MPIDR=0x%08x SCTLR=0x%08x ACTLR=0x%08x\n",
    MbRead (MB_MIDR),
    MbRead (MB_MPIDR),
    MbRead (MB_SCTLR),
    MbRead (MB_ACTLR));

  //
  // Round trip: write an argument, expect the bitwise complement back
  //
  MbWrite (MB_ARG, 0x12345678);
  MbWrite (MB_CMD, CMD_PING);
  SendEvent ();
  if (!MbWaitFor (MB_ACK, CMD_PING, 100) || (MbRead (MB_RESULT) != ~0x12345678U)) {
    Log ("CPU1 ping failed (ack=%u result=0x%08x)\n", MbRead (MB_ACK), MbRead (MB_RESULT));
    Cpu1Stop ();
    return FALSE;
  }

  Log ("CPU1 ping OK\n");
  Beat0 = MbRead (MB_HEARTBEAT);
  SendEvent ();
  MicroSecondDelay (1000);
  Beat1 = MbRead (MB_HEARTBEAT);
  Log ("CPU1 heartbeat %u -> %u\n", Beat0, Beat1);
  return TRUE;
}

//
// Windows RT 8520's kernel: stops that happen before goyabug loads show nothing
// on this device, and the system restarts (DRAM kept). Find the kernel image
// in DRAM by its PE header and read KiBugCheckData (stop code, 4 parameters).
//
#define NT8520_TIMESTAMP     0x500CCE68
#define NT8520_SIZE          0x514000
#define NT8520_BUGCHECKDATA  0x1CB200

STATIC
VOID
ReportWindowsStop (
  VOID
  )
{
  UINTN             Addr;
  UINT32            Lfanew;
  volatile UINT32   *Bc;
  UINTN             Found;
  UINTN             Images;

  //
  // From the second page (page 0 may be unmapped): winload can place the kernel in the first 16 MB.
  //
  Found  = 0;
  Images = 0;
  for (Addr = EFI_PAGE_SIZE; Addr + NT8520_SIZE <= 0x40000000; Addr += EFI_PAGE_SIZE) {
    if (*(volatile UINT16 *)Addr != 0x5A4D) {
      continue;
    }

    Lfanew = *(volatile UINT32 *)(Addr + 0x3C);
    if ((Lfanew > 0x800) || (*(volatile UINT32 *)(Addr + Lfanew) != 0x00004550) ||
        (*(volatile UINT32 *)(Addr + Lfanew + 8) != NT8520_TIMESTAMP))
    {
      if ((Lfanew <= 0x800) && (*(volatile UINT32 *)(Addr + Lfanew) == 0x00004550)) {
        Images++;
      }

      continue;
    }

    Bc = (volatile UINT32 *)(Addr + NT8520_BUGCHECKDATA);
    Log ("previous session: Windows kernel image at 0x%08x, KiBugCheckData %08x %08x %08x %08x %08x\n",
      (UINT32)Addr, Bc[0], Bc[1], Bc[2], Bc[3], Bc[4]);

    //
    // Reported once: wipe the signature so that the next boot only finds the next session's kernel.
    //
    *(volatile UINT16 *)Addr = 0;
    WriteBackDataCacheRange ((VOID *)Addr, sizeof (UINT16));
    if (++Found == 4) {
      break;
    }
  }

  if (Found == 0) {
    Log ("previous session: no Windows kernel image left in DRAM (%u other PE images)\n", (UINT32)Images);
  }
}

/**
  What the previous OS session did with CPU1's parking page (DRAM survives a
  warm restart): state 3 with no wake-ups = the OS never sent the wake-up
  SGI; wake-ups but state 3 = it did, but CPU1 did not see ProcessorId and
  JumpAddress; state 4 = CPU1 jumped to the OS entry point.
**/
STATIC
VOID
ReportPreviousSession (
  VOID
  )
{
  volatile UINT32  *Mb;
  volatile UINT32  *Pp;

  InvalidateDataCacheRange (mPages, 2 * EFI_PAGE_SIZE);
  Mb = (volatile UINT32 *)(mPage + MB_OFFSET);
  Pp = (volatile UINT32 *)mPage;
  if (Mb[MB_ALIVE / 4] != ALIVE_MAGIC) {
    Log ("previous session: no parking page left (cold start or CPU1 not parked)\n");
    return;
  }

  Log ("previous session: CPU1 state %u (3 parked, 4 jumped to the OS), %u wake-ups, ProcessorId 0x%08x, JumpAddress 0x%08x%08x, heartbeat %u\n",
    Mb[MB_STATE / 4],
    Mb[MB_WAKES / 4],
    Pp[PP_CPU_ID / 4],
    Pp[PP_JUMP / 4 + 1],
    Pp[PP_JUMP / 4],
    Mb[MB_HEARTBEAT / 4]);
  Log ("previous session: CPU0 page ProcessorId 0x%08x JumpAddress 0x%08x\n",
    *(volatile UINT32 *)(mPages + PP_CPU_ID),
    *(volatile UINT32 *)(mPages + PP_JUMP));

  //
  // HalExtMmpTimer's breadcrumbs (CPU0 page + 0xE00, see WinRT/HalExtMmpTimer/HalExtension.c):
  // which processor took the timer interrupts and called the timer functions. Cleared once read.
  //
  Pp = (volatile UINT32 *)(mPages + HALX_OFFSET);
  if (Pp[0] == HALX_MAGIC) {
    Log ("previous session: timer IRQs %u (%u on CPU1, last on CPU%u), counter reads %u (%u on CPU1), ArmTimer %u (last mode %u ticks %u on CPU%u), CPU1 seen %u\n",
      Pp[1], Pp[2], Pp[3], Pp[4], Pp[5], Pp[6], Pp[7] & 0xFF, Pp[8], (Pp[7] >> 8) & 0xFF, Pp[9]);
    Pp[0] = 0;
    WriteBackDataCacheRange ((VOID *)(mPages + HALX_OFFSET), 64);
  } else {
    Log ("previous session: no timer breadcrumbs\n");
  }

  //
  // Also when CPU1 never entered the OS: a Windows boot can stop before the HAL starts processors.
  //
  ReportWindowsStop ();
}

/**
  Send CPU1 into the parking-protocol loop. Returns TRUE when it got there.
**/
STATIC
BOOLEAN
Cpu1ParkForOs (
  VOID
  )
{
  UINT32  GiccCtlr;
  UINT32  Diag;

  //
  // CPU1's GIC CPU interface is set up like CPU0's (the register is banked)
  //
  GiccCtlr = MmioRead32 (PXA988_GICC_BASE);
  MbWrite (MB_GICCCTLR, GiccCtlr);

  //
  // Cortex-A9 diagnostic register (errata workarounds S-Boot set on CPU0):
  // CPU1 sets the same value before it jumps to the OS
  //
  __asm__ volatile ("mrc p15, 0, %0, c15, c0, 1" : "=r" (Diag));
  MbWrite (MB_DIAG, Diag);
  Log ("CPU0 diagnostic register 0x%08x, SCU diagnostic 0x%08x, power 0x%08x\n",
    Diag,
    MmioRead32 (PXA988_SCU_BASE + 0x30),
    MmioRead32 (PXA988_SCU_BASE + 0x08));
  MbWrite (MB_CMD, CMD_OS_PARK);
  SendEvent ();
  if (!MbWaitFor (MB_STATE, STATE_OS_PARKED, 100)) {
    Log ("CPU1 did not reach the parking loop (state %u)\n", MbRead (MB_STATE));
    Cpu1Stop ();
    return FALSE;
  }

  InvalidateDataCacheRange (mPage, 0x10);
  Log ("CPU1 parked for the OS: page 0x%08x, ProcessorId 0x%08x, GICC_CTLR 0x%x\n",
    (UINT32)(UINTN)mPage,
    *(volatile UINT32 *)(mPage + PP_CPU_ID),
    GiccCtlr);
  return TRUE;
}

EFI_STATUS
EFIAPI
Pxa988SmpInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Pages;
  UINTN                 StubSize;
  UINTN                 Cpu;
  EFI_HANDLE            Handle;
  BOOLEAN               Parked;

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PublishReport, NULL, &gEfiEndOfDxeEventGroupGuid, &mEndOfDxeEvent);
  ZeroMem (&mParking, sizeof (mParking));
  Parked = FALSE;

  // What the OS will inherit (set in Pxa988PlatformLib before the MMU is enabled)
  Log ("ACTLR=0x%08x (SMP %a, FW %a)\n",
    (UINT32)ArmReadAuxCr (),
    (ArmReadAuxCr () & BIT6) ? "on" : "OFF",
    (ArmReadAuxCr () & BIT0) ? "on" : "OFF");

  if (!FixedPcdGetBool (PcdSecondaryCpuBringup)) {
    goto Publish;
  }

  StubSize = (UINTN)(Pxa988Cpu1StubEnd - Pxa988Cpu1StubStart);
  ASSERT (StubSize == MB_OFFSET - PP_STUB + 0x80);

  //
  // One page per MADT GICC, reserved for the OS's lifetime (the OS must not
  // use it): a fixed EfiReservedMemoryType range from GoyaWifiPlatformPeiLib,
  // which a warm restart keeps, so first report what the previous session
  // (WinPE or Windows) did with CPU1's page.
  //
  Pages  = PXA988_PARKING_PAGES;
  mPages = (UINT8 *)(UINTN)Pages;
  mPage  = mPages + EFI_PAGE_SIZE;
  ReportPreviousSession ();
  ZeroMem (mPages, 2 * EFI_PAGE_SIZE);
  for (Cpu = 0; Cpu < 2; Cpu++) {
    *(UINT32 *)(mPages + Cpu * EFI_PAGE_SIZE + PP_CPU_ID) = 0xFFFFFFFF;
  }

  CopyMem (mPage + PP_STUB, Pxa988Cpu1StubStart, StubSize);
  ZeroMem (mPage + MB_OFFSET, 0x80);
  WriteBackInvalidateDataCacheRange (mPages, 2 * EFI_PAGE_SIZE);
  ArmInvalidateInstructionCache ();
  ArmDataSynchronizationBarrier ();

  ScuEnable ();

  if (Cpu1Start ()) {
    if (FixedPcdGetBool (PcdCpu1ParkingProtocol)) {
      Parked = Cpu1ParkForOs ();
    } else {
      //
      // Self-test only: the OS expects CPU1 held in reset
      //
      Status = gBS->CreateEventEx (
                      EVT_NOTIFY_SIGNAL,
                      TPL_CALLBACK,
                      Cpu1ExitBootServices,
                      NULL,
                      &gEfiEventExitBootServicesGuid,
                      &mExitBsEvent
                      );
      ASSERT_EFI_ERROR (Status);
    }
  }

  if (Parked) {
    mParking.ParkedAddress[0] = (UINT64)(UINTN)mPages;
    mParking.ParkedAddress[1] = (UINT64)(UINTN)mPage;
    WdtGuardSetup ();
  }

Publish:
  if (!Parked) {
    Log ("MADT: CPU1 stays disabled\n");
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gPxa988Cpu1ParkingProtocolGuid,
                &mParking,
                NULL
                );
}
