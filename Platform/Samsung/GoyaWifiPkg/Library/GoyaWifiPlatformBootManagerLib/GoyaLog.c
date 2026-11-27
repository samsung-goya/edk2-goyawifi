/** @file
  Saves the persistent Windows log (goyabug, see GoyaLog.h) to the microSD card.

  Windows writes its boot log, and at a stop the stop details, to the reserved
  RAM at 0x08100000; a watchdog reset keeps the DRAM. On the next start this
  writes \goya\logs\boot-NNNN-<state>.txt on the FAT partition that holds
  \EFI\Microsoft\Boot\BCD, marks the log saved, and restarts into recovery
  (TWRP) unless Windows shut down cleanly, so the file can be pulled over adb.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>

#include "GoyaLog.h"

STATIC CONST CHAR8  *mStateName[] = { "saved", "running", "crash", "shutdown" };
STATIC CONST CHAR8  *mSlotName[GOYALOG_SLOTS] = {
  "-", "goyasdhc", "goyapinmux", "goyagpio", "goyai2c", "goyatouch", "goyakeys", "goyabug"
};

#define SUMMARY_SIZE  4096

STATIC
EFI_STATUS
OpenDir (
  IN  EFI_FILE_PROTOCOL  *Parent,
  IN  CHAR16             *Name,
  OUT EFI_FILE_PROTOCOL  **Dir
  )
{
  return Parent->Open (
                   Parent,
                   Dir,
                   Name,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   EFI_FILE_DIRECTORY
                   );
}

/** The Windows card's system partition: the volume with \EFI\Microsoft\Boot\BCD. */
STATIC
EFI_FILE_PROTOCOL *
OpenWindowsSystemVolume (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Bcd;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
    {
      continue;
    }

    if (!EFI_ERROR (Root->Open (Root, &Bcd, L"\\EFI\\Microsoft\\Boot\\BCD", EFI_FILE_MODE_READ, 0))) {
      Bcd->Close (Bcd);
      FreePool (Handles);
      return Root;
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return NULL;
}

STATIC
EFI_STATUS
WriteAll (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST VOID         *Data,
  IN UINTN              Size
  )
{
  UINTN  Len;

  Len = Size;
  return (Size == 0) ? EFI_SUCCESS : File->Write (File, &Len, (VOID *)Data);
}

/** Write the log to \goya\logs\boot-NNNN-<state>.txt on Root. */
STATIC
EFI_STATUS
SaveLog (
  IN EFI_FILE_PROTOCOL  *Root,
  IN GOYALOG_HDR        *Hdr
  )
{
  EFI_FILE_PROTOCOL  *GoyaDir;
  EFI_FILE_PROTOCOL  *LogDir;
  EFI_FILE_PROTOCOL  *File;
  CHAR16             Name[48];
  CHAR8              *Sum;
  UINTN              Len;
  UINTN              Index;
  UINT32             State;
  EFI_STATUS         Status;

  State  = Hdr->State;
  Status = OpenDir (Root, L"\\goya", &GoyaDir);
  if (!EFI_ERROR (Status)) {
    Status = OpenDir (GoyaDir, L"logs", &LogDir);
    GoyaDir->Close (GoyaDir);
  }

  if (EFI_ERROR (Status)) {
    Print (L"goyalog: cannot create \\goya\\logs: %r\n", Status);
    return Status;
  }

  UnicodeSPrint (Name, sizeof (Name), L"boot-%04d-%a.txt", Hdr->Boot, mStateName[State]);
  if (!EFI_ERROR (LogDir->Open (LogDir, &File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);                                  // a file from an older log numbering
  }

  Status = LogDir->Open (
                     LogDir,
                     &File,
                     Name,
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     0
                     );
  LogDir->Close (LogDir);
  if (EFI_ERROR (Status)) {
    Print (L"goyalog: cannot create %s: %r\n", Name, Status);
    return Status;
  }

  Sum = AllocateZeroPool (SUMMARY_SIZE);
  if (Sum != NULL) {
    Len = AsciiSPrint (
            Sum,
            SUMMARY_SIZE,
            "goyalog: Windows boot %d, %a after %d s, stop code 0x%08x\r\n"
            "boot log %d bytes, stop text %d bytes\r\n\r\n"
            "driver slots (last callback site, calls, first caught exception):\r\n",
            Hdr->Boot,
            mStateName[State],
            Hdr->Uptime,
            Hdr->StopCode,
            Hdr->Used,
            Hdr->CrashUsed
            );
    for (Index = 1; Index < GOYALOG_SLOTS; Index++) {
      Len += AsciiSPrint (
               Sum + Len,
               SUMMARY_SIZE - Len,
               "  %d %-10a site %02x  calls %d",
               Index,
               mSlotName[Index],
               Hdr->Slot[Index].Site,
               Hdr->Slot[Index].Count
               );
      if (Hdr->Slot[Index].ExcCode != 0) {
        Len += AsciiSPrint (
                 Sum + Len,
                 SUMMARY_SIZE - Len,
                 "  EXCEPTION %08x in site %02x at image+%06x",
                 Hdr->Slot[Index].ExcCode,
                 Hdr->Slot[Index].ExcInfo >> 24,
                 Hdr->Slot[Index].ExcInfo & 0xFFFFFF
                 );
      }

      Len += AsciiSPrint (Sum + Len, SUMMARY_SIZE - Len, "\r\n");
    }

    Len += AsciiSPrint (Sum + Len, SUMMARY_SIZE - Len, "\r\n---- stop screen ----\r\n");
    WriteAll (File, Sum, Len);
    WriteAll (File, (UINT8 *)Hdr + GOYALOG_CRASH_OFF, Hdr->CrashUsed);
    Len = AsciiSPrint (Sum, SUMMARY_SIZE, "\r\n---- boot log ----\r\n");
    WriteAll (File, Sum, Len);
    FreePool (Sum);
  }

  Status = WriteAll (File, (UINT8 *)Hdr + GOYALOG_TEXT_OFF, Hdr->Used);
  File->Flush (File);
  File->Close (File);
  Print (L"goyalog: wrote \\goya\\logs\\%s (%r)\n", Name, Status);
  return Status;
}

/**
  Append this boot's second-core report (Pxa988SmpDxe's Cpu1Status variable: the
  parking-protocol state of the previous session and of this boot) to
  \goya\logs\cpu1.txt. A Windows session is followed by two firmware boots (WinPE,
  then the restart into recovery), so the file keeps a history; it starts over when
  it grows past 64 KiB.
**/
STATIC
VOID
SaveCpu1Report (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  CHAR8              Report[2048];
  CHAR8              Head[64];
  UINTN              Size;
  EFI_FILE_PROTOCOL  *File;
  UINT64             End;
  EFI_STATUS         Status;

  Size   = sizeof (Report);
  Status = gRT->GetVariable (L"Cpu1Status", &gPxa988TokenSpaceGuid, NULL, &Size, Report);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = Root->Open (
                   Root,
                   &File,
                   L"\\goya\\logs\\cpu1.txt",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    return;
  }

  End = 0;
  if (EFI_ERROR (File->SetPosition (File, MAX_UINT64)) || EFI_ERROR (File->GetPosition (File, &End))) {
    End = 0;                                              // MAX_UINT64 = end of file
  }

  if (End > SIZE_64KB) {
    File->Delete (File);
    if (EFI_ERROR (Root->Open (Root, &File, L"\\goya\\logs\\cpu1.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
      return;
    }

    End = 0;
  }

  File->SetPosition (File, End);
  AsciiSPrint (Head, sizeof (Head), "=== firmware boot (%a)\r\n", (End == 0) ? "new file" : "appended");
  WriteAll (File, Head, AsciiStrLen (Head));
  WriteAll (File, Report, Size);
  File->Close (File);
}

/**
  Runs every boot (BDS, after the console is up), when the Windows card is present:
  - saves an unsaved goyabug log (see the file header),
  - restarts into recovery if Windows did not shut down cleanly, or if Windows asked
    for it with the flag file \goya\reboot-recovery on the system partition
    (WinPE's autorun.cmd creates it), which is deleted first.
**/
VOID
GoyaLogSave (
  VOID
  )
{
  GOYALOG_HDR        *Hdr;
  BOOLEAN            HaveLog;
  BOOLEAN            Recovery;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *Flag;

  Hdr = (GOYALOG_HDR *)(UINTN)GOYALOG_PHYS;
  InvalidateDataCacheRange (Hdr, GOYALOG_SIZE);
  HaveLog = (Hdr->Magic0 == GOYALOG_MAGIC0) && (Hdr->Magic1 == GOYALOG_MAGIC1) &&
            (Hdr->State != GOYALOG_SAVED) && (Hdr->State <= GOYALOG_SHUTDOWN) &&
            (Hdr->Used <= GOYALOG_TEXT_MAX) && (Hdr->CrashUsed <= GOYALOG_CRASH_MAX);
  Recovery = FALSE;

  if (HaveLog) {
    Print (L"goyalog: Windows boot %d ended: %a after %ds - saving the log\n", Hdr->Boot, mStateName[Hdr->State], Hdr->Uptime);
  }

  EfiBootManagerConnectAll ();
  Root = OpenWindowsSystemVolume ();
  if (Root == NULL) {
    if (HaveLog) {
      Print (L"goyalog: no Windows system partition found, log not saved\n");
    }

    return;
  }

  if (HaveLog && !EFI_ERROR (SaveLog (Root, Hdr))) {
    Recovery    = (Hdr->State != GOYALOG_SHUTDOWN);
    Hdr->State = GOYALOG_SAVED;
    WriteBackDataCacheRange (Hdr, sizeof (*Hdr));
  }

  SaveCpu1Report (Root);

  if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\goya\\reboot-recovery", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    Flag->Delete (Flag);
    Print (L"goyalog: Windows asked for a restart into recovery\n");
    Recovery = TRUE;
  }

  Root->Close (Root);

  if (Recovery) {
    Print (L"goyalog: restarting into recovery\n");
    gBS->Stall (2000000);
    gRT->ResetSystem (EfiResetPlatformSpecific, EFI_SUCCESS, sizeof (L"recovery"), L"recovery");
  }
}
