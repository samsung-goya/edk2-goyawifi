/** @file
  goyawifi PlatformPeiLib.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>

typedef struct {
  EFI_PHYSICAL_ADDRESS    Base;
  UINT64                  Size;
  CONST CHAR8             *Name;
} RESERVED_REGION;

STATIC CONST RESERVED_REGION  mReserved[] = {
  { 0x00000000, 0x00100000, "OBM / BootROM scratch"          },
  { 0x06000000, 0x02100000, "CP carveout (HW protected)"     },
  { 0x08100000, 0x00080000, "ramoops / last_kmsg"            },
  { 0x08180000, 0x00002000, "ACPI parking pages CPU0/CPU1"   },   // PXA988_PARKING_PAGES
};

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  UINTN   Index;
  UINT64  FbSize;

  BuildFvHob (FixedPcdGet64 (PcdFvBaseAddress), FixedPcdGet32 (PcdFvSize));

  for (Index = 0; Index < ARRAY_SIZE (mReserved); Index++) {
    DEBUG ((
      DEBUG_INFO,
      "Reserving 0x%08lx-0x%08lx: %a\n",
      mReserved[Index].Base,
      mReserved[Index].Base + mReserved[Index].Size - 1,
      mReserved[Index].Name
      ));
    BuildMemoryAllocationHob (
      mReserved[Index].Base,
      mReserved[Index].Size,
      EfiReservedMemoryType
      );
  }

  // Bootloader splash = GOP framebuffer, plus one page of log state
  FbSize = ALIGN_VALUE (
             (UINT64)FixedPcdGet32 (PcdFrameBufferStride) * 4 *
             FixedPcdGet32 (PcdFrameBufferHeight),
             EFI_PAGE_SIZE
             ) + EFI_PAGE_SIZE;
  BuildMemoryAllocationHob (
    FixedPcdGet32 (PcdFrameBufferBase),
    FbSize,
    EfiReservedMemoryType
    );

  return EFI_SUCCESS;
}
