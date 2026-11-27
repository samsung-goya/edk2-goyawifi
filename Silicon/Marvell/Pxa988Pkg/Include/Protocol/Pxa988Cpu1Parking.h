/** @file
  Pxa988SmpDxe -> GoyaWifiAcpiDxe: where the cores' ACPI parking-protocol
  pages are ("Multi-processor Startup for ARM Platforms"), for the MADT GICC
  ParkedAddress fields.

  Pxa988SmpDxe always installs it, also when CPU1 could not be parked (then
  ParkedAddress[1] is 0 and the MADT keeps CPU1 disabled), so the ACPI driver
  can wait for it in its depex.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef PXA988_CPU1_PARKING_H_
#define PXA988_CPU1_PARKING_H_

#define PXA988_CPU1_PARKING_PROTOCOL_GUID \
  { 0x2cb0485d, 0x6021, 0x4344, { 0x90, 0xd3, 0x8c, 0x3e, 0xa6, 0x8d, 0x07, 0x00 } }

#define PXA988_PARKING_PROTOCOL_VERSION  1

typedef struct {
  //
  // Physical address of each core's 4 KiB parking page (EfiReservedMemoryType),
  // 0 = none. CPU0's page is never used (the OS runs on it) but is set up like
  // CPU1's, as firmware does for every MADT GICC.
  //
  UINT64    ParkedAddress[2];
} PXA988_CPU1_PARKING_PROTOCOL;

extern EFI_GUID  gPxa988Cpu1ParkingProtocolGuid;

#endif
