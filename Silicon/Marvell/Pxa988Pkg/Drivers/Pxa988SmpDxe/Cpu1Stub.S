//
// Cpu1Stub.S - what the second Cortex-A9 core runs when released from reset.
//
// The BootROM starts the core at the physical address in CIU_WARM_RESET_VECTOR
// with MMU, caches and branch prediction off, in ARM state, SVC mode.
//
// Pxa988SmpDxe copies [Pxa988Cpu1StubStart, Pxa988Cpu1StubEnd) to offset 0x800
// of CPU1's ACPI parking-protocol page ("Multi-processor Startup for ARM
// Platforms"), the firmware half of the page, and points the vector there:
//   page + 0x000  ProcessorId (UINT32, written by the OS; 0xFFFFFFFF = none)
//   page + 0x008  JumpAddress (UINT64, written by the OS; cleared before the jump)
//   page + 0x010  ... 0x7FF reserved for the OS
//   page + 0x800  this stub; page + 0xF00 the debug mailbox below
// The page is EfiReservedMemoryType. Everything the stub touches lives in it,
// so it never needs a cache or an MMU (all its accesses are strongly ordered).
//
// Debug mailbox (page + 0xF00, offsets match Pxa988SmpDxe.c). Words written by
// CPU1 and words written by CPU0 sit in different 32-byte cache lines, so CPU0
// cleaning its line can never overwrite something CPU1 stored:
//   CPU1 -> CPU0:  +00 alive magic  +04 MIDR  +08 MPIDR  +0C SCTLR
//                  +10 heartbeat    +14 ACTLR +18 ack    +20 result
//                  +24 state (1 run, 2 parked in WFI, 3 parked for the OS, 4 jumped to the OS)
//                  +28 wake-ups while parked for the OS
//   CPU0 -> CPU1:  +40 command (1 park, 3 park for the OS, other: ping)
//                  +44 argument     +48 GICC_CTLR value for the OS parking loop
//                  +4C diagnostic register value for the OS entry
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#define MB_ALIVE      0x00
#define MB_MIDR       0x04
#define MB_MPIDR      0x08
#define MB_SCTLR      0x0C
#define MB_HEARTBEAT  0x10
#define MB_ACTLR      0x14
#define MB_ACK        0x18
#define MB_RESULT     0x20
#define MB_STATE      0x24
#define MB_WAKES      0x28
#define MB_CMD        0x40
#define MB_ARG        0x44
#define MB_GICCCTLR   0x48
#define MB_DIAG       0x4C          // CPU0's diagnostic register, for the OS entry
#define SCU_INVALIDATE 0x0C         // SCU Invalidate All in Secure State

#define PP_CPU_ID     0x000         // relative to the page
#define PP_JUMP       0x008
#define STUB_IN_PAGE  0x800         // where Pxa988SmpDxe copies this stub
#define MB_IN_PAGE    0xF00         // mailbox offset in the page (stub + 0x700)

#define GICC_CTLR     0x00          // relative to the GIC CPU interface 0xD1DFE100
#define GICC_PMR      0x04
#define GICC_IAR      0x0C
#define GICC_EOIR     0x10
#define GICD_ISENABLER0 0x100       // relative to the GIC distributor 0xD1DFF000
#define GICD_ISPENDR    0x200
#define GICD_IPRIORITYR 0x400

  .arm
  .text
  .align 5

  .globl  Pxa988Cpu1StubStart
Pxa988Cpu1StubStart:
  cpsid   aif, #0x13                // SVC, IRQ/FIQ/abort masked

  mrc     p15, 0, r0, c1, c0, 0     // SCTLR: MMU, D-cache, I-cache, BP off
  bic     r0, r0, #0x1800
  bic     r0, r0, #0x5
  mcr     p15, 0, r0, c1, c0, 0
  isb

  //
  // A Cortex-A9 comes out of reset with random L1 data-cache contents: the
  // OS enables the cache later, so invalidate it here (by set/way, geometry
  // from CCSIDR), together with the I-cache, the branch predictor and the TLBs.
  //
  mov     r0, #0
  mcr     p15, 0, r0, c7, c5, 0     // ICIALLU
  mcr     p15, 0, r0, c7, c5, 6     // BPIALL
  mcr     p15, 0, r0, c8, c7, 0     // TLBIALL
  mcr     p15, 2, r0, c0, c0, 0     // CSSELR = L1 data cache
  isb
  mrc     p15, 1, r0, c0, c0, 0     // CCSIDR
  and     r1, r0, #7
  add     r1, r1, #4                // log2(line size): set shift
  ubfx    r2, r0, #3, #10           // ways - 1
  ubfx    r3, r0, #13, #15          // sets - 1
  clz     r5, r2                    // way shift
1:
  mov     r6, r3
2:
  lsl     r8, r2, r5
  orr     r8, r8, r6, lsl r1
  mcr     p15, 0, r8, c7, c6, 2     // DCISW
  subs    r6, r6, #1
  bge     2b
  subs    r2, r2, #1
  bge     1b
  dsb
  isb

  //
  // Take part in SMP coherency (ACTLR.SMP) and broadcast cache/TLB
  // maintenance (ACTLR.FW), as Pxa988PlatformLib does on CPU0
  //
  mrc     p15, 0, r0, c1, c0, 1
  orr     r0, r0, #0x41
  mcr     p15, 0, r0, c1, c0, 1
  isb

  adr     r7, Pxa988Cpu1StubStart
  sub     r7, r7, #STUB_IN_PAGE     // r7 = parking-protocol page
  add     r4, r7, #MB_IN_PAGE       // r4 = debug mailbox (Pxa988Cpu1Mailbox)

  mrc     p15, 0, r0, c0, c0, 0
  str     r0, [r4, #MB_MIDR]
  mrc     p15, 0, r0, c0, c0, 5
  str     r0, [r4, #MB_MPIDR]
  mrc     p15, 0, r0, c1, c0, 0
  str     r0, [r4, #MB_SCTLR]
  mrc     p15, 0, r0, c1, c0, 1
  str     r0, [r4, #MB_ACTLR]
  mov     r0, #1
  str     r0, [r4, #MB_STATE]
  movw    r0, #0xC0DE
  movt    r0, #0xA9A9
  dsb
  str     r0, [r4, #MB_ALIVE]       // last: tells CPU0 we are up
  dsb
  sev

  mov     r5, #0                    // heartbeat
  mov     r6, #0                    // last command handled
Pxa988Cpu1Loop:
  add     r5, r5, #1
  str     r5, [r4, #MB_HEARTBEAT]
  dsb
  ldr     r0, [r4, #MB_CMD]
  cmp     r0, #1
  beq     Pxa988Cpu1Park            // command 1: park in WFI (CPU0 then resets us)
  cmp     r0, #3
  beq     Pxa988Cpu1OsPark          // command 3: wait for the OS (parking protocol)
  cmp     r0, #0
  cmpne   r0, r6
  beq     Pxa988Cpu1Wait            // nothing new
  mov     r6, r0                    // any other value: ping (ack with ~arg)
  ldr     r1, [r4, #MB_ARG]
  mvn     r1, r1
  str     r1, [r4, #MB_RESULT]
  dsb
  str     r0, [r4, #MB_ACK]
  dsb
  sev
Pxa988Cpu1Wait:
  wfe
  b       Pxa988Cpu1Loop

Pxa988Cpu1Park:
  mov     r0, #2
  str     r0, [r4, #MB_STATE]
  dsb
  sev
1:
  wfi
  b       1b

  //
  // ACPI parking protocol: the OS writes JumpAddress, then ProcessorId, and
  // sends this core an SGI. The GIC CPU interface is enabled (priority mask
  // open) so that the SGI ends WFI even with interrupts masked; each interrupt
  // is acknowledged and ended here so it cannot keep WFI from sleeping.
  //
Pxa988Cpu1OsPark:
  movw    r0, #0xE100
  movt    r0, #0xD1DF               // r0 = GIC CPU interface
  movw    r11, #0xF000
  movt    r11, #0xD1DF              // r11 = GIC distributor
  movw    r1, #0xFFFF
  str     r1, [r11, #GICD_ISENABLER0] // SGIs (banked for this core; fixed on on the A9)
  //
  // Only SGIs may reach this core while it waits: give this core's (banked)
  // SGI priorities the top priority 0 and open the mask just for that.
  // Windows's own interrupts (lower priority) stay pending for CPU0.
  //
  mov     r1, #0
  str     r1, [r11, #GICD_IPRIORITYR]
  str     r1, [r11, #GICD_IPRIORITYR + 4]
  str     r1, [r11, #GICD_IPRIORITYR + 8]
  str     r1, [r11, #GICD_IPRIORITYR + 12]
  mov     r1, #0x08
  str     r1, [r0, #GICC_PMR]
  ldr     r1, [r4, #MB_GICCCTLR]
  orr     r1, r1, #1
  str     r1, [r0, #GICC_CTLR]

  mvn     r1, #0
  str     r1, [r7, #PP_CPU_ID]      // no processor selected yet
  mov     r1, #0
  str     r1, [r7, #PP_JUMP]
  str     r1, [r7, #PP_JUMP + 4]
  str     r1, [r4, #MB_WAKES]
  dsb
  mov     r1, #3
  str     r1, [r4, #MB_STATE]
  dsb
  sev
  mov     r9, #0                    // wake-up count
  movw    r10, #1023                // spurious interrupt ID

Pxa988Cpu1OsLoop:
  ldr     r1, [r7, #PP_CPU_ID]
  cmn     r1, #1                    // still 0xFFFFFFFF?
  beq     Pxa988Cpu1OsWait
  ldr     r2, [r7, #PP_JUMP]        // 32-bit core: the low word is the address
  cmp     r2, #0
  beq     Pxa988Cpu1OsWait
  mov     r1, #0
  str     r1, [r7, #PP_JUMP]        // acknowledge: the OS sees JumpAddress cleared
  str     r1, [r7, #PP_JUMP + 4]
  //
  // Hand over the GIC CPU interface the way it came out of reset: disabled,
  // everything masked. Windows keeps sending this core SGIs while it waits,
  // and one left pending here would fire as soon as the OS unmasks IRQs,
  // before its interrupt controller code has set this core up.
  //
  mov     r1, #0
  str     r1, [r0, #GICC_CTLR]
  str     r1, [r0, #GICC_PMR]
  dsb
  //
  // Cortex-A9 errata workarounds live in the diagnostic register, which S-Boot
  // set on CPU0 only: CPU0's value (from the mailbox, read by Pxa988SmpDxe)
  //
  ldr     r1, [r4, #MB_DIAG]
  mcr     p15, 0, r1, c15, c0, 1
  isb
  //
  // The SCU keeps a copy of each core's L1 data-cache tags. This core's L1 is
  // invalid and its caches are off: invalidate its SCU tags too (all four
  // ways, bits [7:4] for CPU1), before the OS turns its cache on.
  //
  movw    r1, #0xE000
  movt    r1, #0xD1DF               // SCU
  mov     r3, #0xF0
  str     r3, [r1, #SCU_INVALIDATE]
  dsb
  mov     r1, #4
  str     r1, [r4, #MB_STATE]
  dsb
  isb
  //
  // r0 = the parking page (physical): Windows's entry code (hal.dll, 8520)
  // finds the processor state it saved at page + 0x18 through r0
  //
  mov     r0, r7
  bx      r2                        // bit 0 set = Thumb entry

Pxa988Cpu1OsWait:
  wfi
  add     r9, r9, #1
  str     r9, [r4, #MB_WAKES]
  ldr     r1, [r0, #GICC_IAR]
  ubfx    r3, r1, #0, #10
  cmp     r3, r10
  beq     3f                        // spurious: nothing acknowledged
  str     r1, [r0, #GICC_EOIR]
  cmp     r3, #32
  blo     3f                        // an SGI: ours, done
  // Not ours (should not pass the mask): make it pending again for CPU0
  lsr     r1, r3, #5
  and     r3, r3, #31
  mov     r8, #1
  lsl     r8, r8, r3
  add     r1, r11, r1, lsl #2
  str     r8, [r1, #GICD_ISPENDR]
3:
  dsb
  b       Pxa988Cpu1OsLoop

  .org    Pxa988Cpu1StubStart + 0x700
  .globl  Pxa988Cpu1Mailbox
Pxa988Cpu1Mailbox:
  .space  0x80
  .globl  Pxa988Cpu1StubEnd
Pxa988Cpu1StubEnd:
