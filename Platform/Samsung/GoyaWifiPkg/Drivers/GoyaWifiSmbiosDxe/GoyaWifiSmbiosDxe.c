/** @file
  SMBIOS tables for the goyawifi (Samsung SM-T110, Marvell PXA986).

  Windows on ARM takes the processor name it shows (Task Manager, System, the
  ProcessorNameString value under HARDWARE\DESCRIPTION\System\CentralProcessor) from the SMBIOS
  type 4 "Processor Version" string. Without any SMBIOS table the processor had no name.

  Published through MdeModulePkg SmbiosDxe (SMBIOS 2.7, 32-bit entry point only: Windows 8 does
  not know the 3.0 entry point):
    type 0  BIOS             type 1  system           type 3  chassis (tablet)
    type 4  processor        type 16 memory array     type 17 memory device
    type 19 memory mapping   type 32 boot status

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define GOYA_CPU_NAME      "Marvell PXA986 Dual-Core Cortex-A9"
#define GOYA_CPU_MAX_MHZ   1200
#define GOYA_CPU_DVFS_MHZ  1066

STATIC EFI_SMBIOS_PROTOCOL  *mSmbios;

/**
  Add one record: the formatted part (Length bytes, Header.Length already set) followed by its
  strings, NULL-terminated list; string numbers in the record are 1-based in this order.
**/
STATIC
EFI_STATUS
AddRecord (
  IN  SMBIOS_STRUCTURE    *Record,
  OUT SMBIOS_HANDLE       *HandleOut  OPTIONAL,
  ...
  )
{
  VA_LIST           Args;
  CONST CHAR8       *Str;
  UINTN             Size;
  UINTN             Len;
  UINT8             *Buf;
  UINT8             *P;
  SMBIOS_HANDLE     Handle;
  EFI_STATUS        Status;

  Size = Record->Length;
  VA_START (Args, HandleOut);
  while ((Str = VA_ARG (Args, CONST CHAR8 *)) != NULL) {
    Size += AsciiStrLen (Str) + 1;
  }

  VA_END (Args);
  Size += 2;                                            // terminating double NUL (one if strings exist)

  Buf = AllocateZeroPool (Size);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Buf, Record, Record->Length);
  P = Buf + Record->Length;
  VA_START (Args, HandleOut);
  while ((Str = VA_ARG (Args, CONST CHAR8 *)) != NULL) {
    Len = AsciiStrLen (Str);
    CopyMem (P, Str, Len);
    P += Len + 1;
  }

  VA_END (Args);

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = mSmbios->Add (mSmbios, NULL, &Handle, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  FreePool (Buf);
  if (HandleOut != NULL) {
    *HandleOut = Handle;
  }

  DEBUG ((DEBUG_INFO, "GoyaWifiSmbios: type %u -> handle 0x%04x (%r)\n", Record->Type, Handle, Status));
  return Status;
}

STATIC
VOID
AddBios (
  VOID
  )
{
  SMBIOS_TABLE_TYPE0  T;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type                                        = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  T.Hdr.Length                                      = (UINT8)OFFSET_OF (SMBIOS_TABLE_TYPE0, ExtendedBiosSize);
  T.Vendor                                          = 1;
  T.BiosVersion                                     = 2;
  T.BiosSegment                                     = 0;
  T.BiosReleaseDate                                 = 3;
  T.BiosSize                                        = 0x1F;           // (n + 1) * 64 KiB = 2 MiB boot.img
  T.BiosCharacteristics.BiosCharacteristicsNotSupported = 0;
  T.BiosCharacteristics.BiosIsUpgradable            = 1;
  T.BiosCharacteristics.BootFromCdIsSupported       = 0;
  T.BiosCharacteristics.SelectableBootIsSupported   = 1;
  T.BIOSCharacteristicsExtensionBytes[1]            = BIT3;           // UEFI is supported
  T.SystemBiosMajorRelease                          = 1;
  T.SystemBiosMinorRelease                          = 0;
  T.EmbeddedControllerFirmwareMajorRelease          = 0xFF;           // no EC
  T.EmbeddedControllerFirmwareMinorRelease          = 0xFF;
  AddRecord (&T.Hdr, NULL, "edk2-goyawifi", "1.0 (EDK2 " __DATE__ ")", "08/01/2012", NULL);
}

STATIC
VOID
AddSystem (
  VOID
  )
{
  SMBIOS_TABLE_TYPE1  T;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type     = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
  T.Hdr.Length   = sizeof (SMBIOS_TABLE_TYPE1);
  T.Manufacturer = 1;
  T.ProductName  = 2;
  T.Version      = 3;
  T.SerialNumber = 0;
  T.WakeUpType   = SystemWakeupTypePowerSwitch;
  T.SKUNumber    = 4;
  T.Family       = 5;
  AddRecord (&T.Hdr, NULL, "Samsung", "Galaxy Tab 3 Lite 7.0 (SM-T110)", "goyawifi", "SM-T110", "Galaxy Tab", NULL);
}

STATIC
VOID
AddChassis (
  VOID
  )
{
  SMBIOS_TABLE_TYPE3  T;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type                     = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
  T.Hdr.Length                   = (UINT8)OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements);
  T.Manufacturer                 = 1;
  T.Type                         = 0x1E;                         // Tablet (SMBIOS 3.0; not in this MdePkg enum)
  T.BootupState                  = ChassisStateSafe;
  T.PowerSupplyState             = ChassisStateSafe;
  T.ThermalState                 = ChassisStateSafe;
  T.SecurityStatus               = ChassisSecurityStatusNone;
  T.ContainedElementCount        = 0;
  T.ContainedElementRecordLength = 0;
  AddRecord (&T.Hdr, NULL, "Samsung", NULL);
}

STATIC
VOID
AddProcessor (
  VOID
  )
{
  SMBIOS_TABLE_TYPE4  T;
  UINT32              Midr;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type              = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
  T.Hdr.Length            = (UINT8)OFFSET_OF (SMBIOS_TABLE_TYPE4, CoreCount2);    // SMBIOS 2.6+ layout
  T.Socket                = 1;
  T.ProcessorType         = CentralProcessor;
  T.ProcessorFamily       = ProcessorFamilyIndicatorFamily2;
  T.ProcessorManufacturer = 2;
  Midr                    = (UINT32)ArmReadMidr ();
  CopyMem (&T.ProcessorId, &Midr, sizeof (Midr));                             // ARM: MIDR in the low half
  T.ProcessorVersion      = 3;
  T.ExternalClock         = 0;
  T.MaxSpeed              = GOYA_CPU_MAX_MHZ;
  T.CurrentSpeed          = FixedPcdGetBool (PcdDvfsEnable) ? GOYA_CPU_DVFS_MHZ : 0;
  T.Status                = 0x41;                                             // populated, enabled
  T.ProcessorUpgrade      = ProcessorUpgradeNone;
  T.L1CacheHandle         = 0xFFFF;
  T.L2CacheHandle         = 0xFFFF;
  T.L3CacheHandle         = 0xFFFF;
  T.SerialNumber          = 0;
  T.AssetTag              = 0;
  T.PartNumber            = 4;
  T.CoreCount             = 2;
  T.EnabledCoreCount      = 2;
  T.ThreadCount           = 2;
  T.ProcessorCharacteristics = BIT3 | BIT5;                                   // multi-core, execute protection
  T.ProcessorFamily2      = ProcessorFamilyARMv7;
  AddRecord (&T.Hdr, NULL, "CPU0", "Marvell", GOYA_CPU_NAME, "PXA986", NULL);
}

STATIC
VOID
AddMemory (
  VOID
  )
{
  SMBIOS_TABLE_TYPE16  A;
  SMBIOS_TABLE_TYPE17  D;
  SMBIOS_TABLE_TYPE19  M;
  SMBIOS_HANDLE        ArrayHandle;
  UINT64               Base;
  UINT64               Size;

  Base = PcdGet64 (PcdSystemMemoryBase);
  Size = PcdGet64 (PcdSystemMemorySize);

  ZeroMem (&A, sizeof (A));
  A.Hdr.Type                     = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  A.Hdr.Length                   = sizeof (SMBIOS_TABLE_TYPE16);
  A.Location                     = MemoryArrayLocationSystemBoard;
  A.Use                          = MemoryArrayUseSystemMemory;
  A.MemoryErrorCorrection        = MemoryErrorCorrectionNone;
  A.MaximumCapacity              = (UINT32)(Size / SIZE_1KB);
  A.MemoryErrorInformationHandle = 0xFFFE;
  A.NumberOfMemoryDevices        = 1;
  if (EFI_ERROR (AddRecord (&A.Hdr, &ArrayHandle, NULL))) {
    return;
  }

  ZeroMem (&D, sizeof (D));
  D.Hdr.Type                     = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  D.Hdr.Length                   = (UINT8)OFFSET_OF (SMBIOS_TABLE_TYPE17, MinimumVoltage);   // SMBIOS 2.7
  D.MemoryArrayHandle            = ArrayHandle;
  D.MemoryErrorInformationHandle = 0xFFFE;
  D.TotalWidth                   = 32;
  D.DataWidth                    = 32;
  D.Size                         = (UINT16)(Size / SIZE_1MB);                               // MB, bit 15 clear
  D.FormFactor                   = MemoryFormFactorChip;
  D.DeviceLocator                = 1;
  D.BankLocator                  = 0;
  D.MemoryType                   = MemoryTypeLpddr2;
  D.TypeDetail.Synchronous       = 1;
  D.Manufacturer                 = 0;
  AddRecord (&D.Hdr, NULL, "DRAM", NULL);

  ZeroMem (&M, sizeof (M));
  M.Hdr.Type          = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  M.Hdr.Length        = sizeof (SMBIOS_TABLE_TYPE19);
  M.StartingAddress   = (UINT32)(Base / SIZE_1KB);
  M.EndingAddress     = (UINT32)((Base + Size - 1) / SIZE_1KB);
  M.MemoryArrayHandle = ArrayHandle;
  M.PartitionWidth    = 1;
  AddRecord (&M.Hdr, NULL, NULL);
}

STATIC
VOID
AddBootInfo (
  VOID
  )
{
  SMBIOS_TABLE_TYPE32  T;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
  T.Hdr.Length = (UINT8)(OFFSET_OF (SMBIOS_TABLE_TYPE32, BootStatus) + 1);   // 0x0B: BootStatus is an enum (4 bytes) in the struct
  T.BootStatus = BootInformationStatusNoError;
  AddRecord (&T.Hdr, NULL, NULL);
}

EFI_STATUS
EFIAPI
GoyaWifiSmbiosDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&mSmbios);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GoyaWifiSmbios: no SMBIOS protocol (%r)\n", Status));
    return Status;
  }

  AddBios ();
  AddSystem ();
  AddChassis ();
  AddProcessor ();
  AddMemory ();
  AddBootInfo ();
  return EFI_SUCCESS;
}
