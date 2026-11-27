/** @file
  Minimal ACPI 5.0 table set for Windows RT on the Galaxy Tab 3 Lite
  (Marvell PXA986/988, dual Cortex-A9, GICv1, no Generic Timer).

  FADT (HW_REDUCED_ACPI), FACS, MADT, CSRT (MMP timer 0), SPCR, DSDT.
  There is intentionally no GTDT: Cortex-A9 has no architected timer.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/Pxa988Cpu1Parking.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pxa988.h>
#include "DsdtAml.h"

#define OEM_ID         { 'S','A','M','S','N','G' }
#define OEM_TABLE_ID   SIGNATURE_64 ('G','O','Y','A','W','I','F','I')
#define CREATOR_ID     SIGNATURE_32 ('E','D','K','2')
#define DSDT_AML_NAME_OP      0x08     // NameOp
#define DSDT_AML_BYTE_PREFIX  0x0A     // BytePrefix

#define GIC_DIST_BASE  0xD1DFF000ULL   // verified on hardware (not 0xD1DF1000)
#define GIC_CPU_BASE   0xD1DFE100ULL   // verified on hardware (not 0xD1DF0100)
#define TIMER0_IRQ     45              // SPI 13 + 32
#define TIMER0_FREQ    3250000

#define FILL_HEADER(H, Sig, Len, Rev)  do { \
    (H).Signature = (Sig); (H).Length = (Len); (H).Revision = (Rev); \
    CopyMem ((H).OemId, "SAMSNG", 6); (H).OemTableId = OEM_TABLE_ID; \
    (H).OemRevision = 1; (H).CreatorId = CREATOR_ID; (H).CreatorRevision = 1; \
  } while (0)

STATIC
VOID
Checksum (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *H
  )
{
  UINT8  *P;
  UINT8  Sum;
  UINTN  I;

  H->Checksum = 0;
  P           = (UINT8 *)H;
  Sum         = 0;
  for (I = 0; I < H->Length; I++) {
    Sum = (UINT8)(Sum + P[I]);
  }

  H->Checksum = (UINT8)(0 - Sum);
}

//
// --- FACS
//
STATIC EFI_ACPI_5_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  mFacs;

//
// --- FADT (rev 5, hardware-reduced)
//
STATIC EFI_ACPI_5_0_FIXED_ACPI_DESCRIPTION_TABLE  mFadt;

//
// --- MADT: 2 GICC + 1 GICD
//
#pragma pack(1)
typedef struct {
  EFI_ACPI_5_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  Header;
  EFI_ACPI_5_0_GIC_STRUCTURE                           Gicc[2];
  EFI_ACPI_5_0_GIC_DISTRIBUTOR_STRUCTURE               Gicd;
} MADT_TABLE;

//
// --- CSRT (Core System Resources Table), one resource group "MRVL" with one
// timer descriptor. Layout follows the Microsoft CSRT specification.
//
typedef struct {
  UINT32    Length;
  UINT32    VendorId;
  UINT32    SubvendorId;
  UINT16    DeviceId;
  UINT16    SubdeviceId;
  UINT16    Revision;
  UINT16    Reserved;
  UINT32    SharedInfoLength;
} CSRT_GROUP;

typedef struct {
  UINT32    Length;
  UINT16    Type;
  UINT16    Subtype;
  UINT32    Uid;
} CSRT_DESC_HEADER;

typedef struct {
  CSRT_DESC_HEADER  Header;
  UINT32            TimerType;        // MMP timer
  UINT32            ClockSource;
  UINT32            Frequency;
  UINT32            FrequencyScale;
  UINT32            BaseAddress;
  UINT32            Interrupt;        // GIC IRQ number
} CSRT_TIMER_DESC;

typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER  Header;
  CSRT_GROUP                   Group;
  CSRT_TIMER_DESC              Timer[2];    // [0] counter 0 event timer, [1] counter 1 free-running counter
} CSRT_TABLE;

//
// --- SPCR (Serial Port Console Redirection)
//
typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER       Header;
  UINT8                             InterfaceType;
  UINT8                             Reserved[3];
  EFI_ACPI_6_3_GENERIC_ADDRESS_STRUCTURE  BaseAddress;
  UINT8                             InterruptType;
  UINT8                             Irq;
  UINT32                            Gsiv;
  UINT8                             BaudRate;
  UINT8                             Parity;
  UINT8                             StopBits;
  UINT8                             FlowControl;
  UINT8                             TerminalType;
  UINT8                             Language;
  UINT16                            PciDeviceId;
  UINT16                            PciVendorId;
  UINT8                             PciBus;
  UINT8                             PciDevice;
  UINT8                             PciFunction;
  UINT32                            PciFlags;
  UINT8                             PciSegment;
  UINT32                            Reserved2;
} SPCR_TABLE;
#pragma pack()

STATIC MADT_TABLE   mMadt;
STATIC CSRT_TABLE   mCsrt;
STATIC SPCR_TABLE   mSpcr;

STATIC
VOID
BuildTables (
  IN CONST PXA988_CPU1_PARKING_PROTOCOL  *Parking
  )
{
  UINTN  I;

  // FACS
  mFacs.Signature = EFI_ACPI_5_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_SIGNATURE;
  mFacs.Length    = sizeof (mFacs);
  mFacs.Version   = EFI_ACPI_5_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_VERSION;

  // FADT
  FILL_HEADER (mFadt.Header, EFI_ACPI_5_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE, sizeof (mFadt), EFI_ACPI_5_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION);
  mFadt.Header.Revision        = 5;
  mFadt.Flags                  = EFI_ACPI_5_0_HW_REDUCED_ACPI;   // BIT20
  mFadt.IaPcBootArch           = 0;                              // ARM_BOOT_ARCH: no PSCI
  mFadt.FirmwareCtrl           = 0;                              // set by AcpiTableDxe from FACS
  mFadt.PreferredPmProfile     = EFI_ACPI_5_0_PM_PROFILE_TABLET;
  // all legacy register blocks stay zero (hardware-reduced)

  // MADT
  FILL_HEADER (mMadt.Header.Header, EFI_ACPI_5_0_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE, sizeof (mMadt), EFI_ACPI_5_0_MULTIPLE_APIC_DESCRIPTION_TABLE_REVISION);
  mMadt.Header.LocalApicAddress = 0;
  mMadt.Header.Flags            = 0;
  for (I = 0; I < 2; I++) {
    mMadt.Gicc[I].Type                    = EFI_ACPI_5_0_GIC;
    mMadt.Gicc[I].Length                  = sizeof (EFI_ACPI_5_0_GIC_STRUCTURE);
    mMadt.Gicc[I].GicId                   = (UINT32)I;
    mMadt.Gicc[I].AcpiProcessorUid        = (UINT32)I;
    //
    // CPU1 is enabled only when Pxa988SmpDxe left it waiting in its ACPI
    // parking-protocol page ("Multi-processor Startup for ARM Platforms"):
    // the OS writes the entry point there and wakes it with an SGI.
    //
    mMadt.Gicc[I].Flags                   = ((I == 0) || (Parking->ParkedAddress[1] != 0)) ? EFI_ACPI_5_0_GIC_ENABLED : 0;
    mMadt.Gicc[I].ParkingProtocolVersion  = (Parking->ParkedAddress[I] != 0) ? PXA988_PARKING_PROTOCOL_VERSION : 0;
    mMadt.Gicc[I].PerformanceInterruptGsiv = 0;
    mMadt.Gicc[I].ParkedAddress           = Parking->ParkedAddress[I];
    mMadt.Gicc[I].PhysicalBaseAddress     = GIC_CPU_BASE;
  }

  mMadt.Gicd.Type                = EFI_ACPI_5_0_GICD;
  mMadt.Gicd.Length              = sizeof (EFI_ACPI_5_0_GIC_DISTRIBUTOR_STRUCTURE);
  mMadt.Gicd.GicId               = 0;
  mMadt.Gicd.PhysicalBaseAddress = GIC_DIST_BASE;
  mMadt.Gicd.SystemVectorBase    = 0;

  // CSRT
  FILL_HEADER (mCsrt.Header, SIGNATURE_32 ('C','S','R','T'), sizeof (mCsrt), 0);
  mCsrt.Group.Length           = sizeof (CSRT_GROUP) + 2 * sizeof (CSRT_TIMER_DESC);
  mCsrt.Group.VendorId         = SIGNATURE_32 ('M','R','V','L');
  mCsrt.Group.DeviceId         = 0x0001;
  mCsrt.Group.Revision         = 0;
  mCsrt.Group.SharedInfoLength = 0;
  for (I = 0; I < 2; I++) {
    mCsrt.Timer[I].Header.Length  = sizeof (CSRT_TIMER_DESC);
    // CSRT_RD_TYPE_TIMER = 2, CSRT_RD_SUBTYPE_TIMER = 0. The 8520 HAL dispatches descriptors on
    // (Subtype << 16 | Type) and only passes 0x00000002 to the timer code: with subtype 1 both timers
    // were dropped and the HAL stopped with 0x5C (0x110, problem 0x14 = no clock timer).
    mCsrt.Timer[I].Header.Type    = 2;
    mCsrt.Timer[I].Header.Subtype = 0;
    mCsrt.Timer[I].Header.Uid     = (UINT32)I;
    mCsrt.Timer[I].TimerType      = (UINT32)(I + 1);   // 1 = MMP event (counter 0), 2 = MMP counter (counter 1)
    mCsrt.Timer[I].ClockSource    = 3;       // FNCLKSEL 3 (3.25 MHz)
    mCsrt.Timer[I].Frequency      = TIMER0_FREQ;
    mCsrt.Timer[I].FrequencyScale = 1;
    mCsrt.Timer[I].BaseAddress    = PXA988_TIMER0_BASE;
    mCsrt.Timer[I].Interrupt      = (I == 0) ? TIMER0_IRQ : 0;
  }

  // SPCR (UART1)
  FILL_HEADER (mSpcr.Header, SIGNATURE_32 ('S','P','C','R'), sizeof (mSpcr), 2);
  mSpcr.InterfaceType                    = 0;         // 16550 compatible
  mSpcr.BaseAddress.AddressSpaceId       = EFI_ACPI_6_3_SYSTEM_MEMORY;
  mSpcr.BaseAddress.RegisterBitWidth     = 32;
  mSpcr.BaseAddress.RegisterBitOffset    = 0;
  mSpcr.BaseAddress.AccessSize           = EFI_ACPI_6_3_DWORD;
  mSpcr.BaseAddress.Address              = PXA988_UART1_BASE;
  mSpcr.InterruptType                    = BIT3;      // GIC
  mSpcr.Gsiv                             = 59;        // SPI 27 + 32
  mSpcr.BaudRate                         = 7;         // 115200
  mSpcr.Parity                           = 0;
  mSpcr.StopBits                         = 1;
  mSpcr.FlowControl                      = 0;
  mSpcr.TerminalType                     = 2;         // VT-UTF8
  mSpcr.PciDeviceId                      = 0xFFFF;
  mSpcr.PciVendorId                      = 0xFFFF;
}

STATIC
EFI_STATUS
Install (
  IN EFI_ACPI_TABLE_PROTOCOL  *Acpi,
  IN VOID                     *Table,
  IN UINTN                    Size,
  IN CONST CHAR8              *Name,
  IN BOOLEAN                  IsHeader
  )
{
  UINTN       Key;
  EFI_STATUS  Status;

  if (IsHeader) {
    Checksum ((EFI_ACPI_DESCRIPTION_HEADER *)Table);
  }

  Status = Acpi->InstallAcpiTable (Acpi, Table, Size, &Key);
  DEBUG ((DEBUG_INFO, "GoyaWifiAcpi: %a: %r\n", Name, Status));
  return Status;
}

EFI_STATUS
EFIAPI
GoyaWifiAcpiEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_ACPI_TABLE_PROTOCOL       *Acpi;
  EFI_STATUS                    Status;
  UINT8                         *Dsdt;
  PXA988_CPU1_PARKING_PROTOCOL  *Parking;
  PXA988_CPU1_PARKING_PROTOCOL  NoParking;
  UINTN                         I;
  UINTN                         Found;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gPxa988Cpu1ParkingProtocolGuid, NULL, (VOID **)&Parking);
  if (EFI_ERROR (Status)) {
    ZeroMem (&NoParking, sizeof (NoParking));
    Parking = &NoParking;
  }

  DEBUG ((DEBUG_INFO, "GoyaWifiAcpi: parking pages CPU0 0x%lx CPU1 0x%lx\n", Parking->ParkedAddress[0], Parking->ParkedAddress[1]));
  BuildTables (Parking);

  Dsdt = AllocateCopyPool (mDsdtAmlLen, mDsdtAml);
  if (Dsdt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // CPU1's _STA returns \_SB.CPU1.STAV (Name STAV ByteConst 0x0F): make it 0
  // when the MADT has CPU1 disabled, so the DSDT agrees with the MADT
  //
  Found = 0;
  for (I = 0; I + 7 <= mDsdtAmlLen; I++) {
    if ((Dsdt[I] == DSDT_AML_NAME_OP) && (CompareMem (&Dsdt[I + 1], "STAV", 4) == 0) && (Dsdt[I + 5] == DSDT_AML_BYTE_PREFIX)) {
      if (Parking->ParkedAddress[1] == 0) {
        Dsdt[I + 6] = 0;
      }

      Found++;
    }
  }

  ASSERT (Found == 1);
  ((EFI_ACPI_DESCRIPTION_HEADER *)Dsdt)->OemTableId = OEM_TABLE_ID;
  Checksum ((EFI_ACPI_DESCRIPTION_HEADER *)Dsdt);

  //
  // FACS and DSDT are linked into the FADT by AcpiTableDxe when installed
  // after it (FACS first, then DSDT, then the FADT).
  //
  Install (Acpi, &mFacs, sizeof (mFacs), "FACS", FALSE);
  Install (Acpi, Dsdt, mDsdtAmlLen, "DSDT", TRUE);
  Install (Acpi, &mFadt, sizeof (mFadt), "FADT", TRUE);
  Install (Acpi, &mMadt, sizeof (mMadt), "MADT", TRUE);
  Install (Acpi, &mCsrt, sizeof (mCsrt), "CSRT", TRUE);
  Install (Acpi, &mSpcr, sizeof (mSpcr), "SPCR", TRUE);
  return EFI_SUCCESS;
}
