"""Run the CPU1 stub (Cpu1Stub.S) alone in Unicorn and play CPU0's role:
wait for the alive magic, ping it, check the answer; then play the OS's role
in the ACPI parking protocol: park it for the OS, wake it with nothing to do,
then give it a (Thumb) entry point and check that it clears JumpAddress and
jumps there. A second run checks the old "park in WFI" command.

The stub runs from offset 0x800 of its 4 KiB parking page (as Pxa988SmpDxe
copies it); the debug mailbox is at page + 0xF00.

usage: cpu1_stub_test.py <Cpu1Stub.obj>
"""
import subprocess, struct, sys
from unicorn import *
from unicorn.arm_const import *

obj = sys.argv[1]
subprocess.check_call(['arm-linux-gnueabi-objcopy', '-O', 'binary', '--only-section=.text', obj, '/tmp/cpu1stub.bin'])
code = open('/tmp/cpu1stub.bin', 'rb').read()
assert len(code) == 0x780, hex(len(code))

PAGE = 0x10000000          # the parking page in "DRAM"
STUB = PAGE + 0x800
MB = PAGE + 0xF00
GICC = 0xD1DFE100
GICD = 0xD1DFF000
WFE, WFI = 0xe320f002, 0xe320f003


def boot():
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    mu.mem_map(PAGE, 0x1000)
    mu.mem_map(0xD1DFE000, 0x2000)                  # SCU/GICC page + GICD page
    mu.mem_write(STUB, code)
    mu.reg_write(UC_ARM_REG_CPSR, 0x1d3)            # SVC, as after reset
    mu.reg_write(UC_ARM_REG_PC, STUB)
    return mu


def rd(mu, a): return struct.unpack('<I', mu.mem_read(a, 4))[0]
def wr(mu, a, v): mu.mem_write(a, struct.pack('<I', v))


def run(mu, maxins=40000):
    """Run until the stub sleeps (WFE/WFI), or maxins instructions."""
    pc = mu.reg_read(UC_ARM_REG_PC)
    if mu.reg_read(UC_ARM_REG_CPSR) & 0x20:
        pc |= 1
    # Depending on the version, Unicorn either reports WFE/WFI as an invalid
    # instruction or just stops after it. The stub sleeps there until CPU0
    # sends an event or an interrupt arrives, so both mean "halted"; the next
    # run() resumes after it.
    err = None
    try:
        mu.emu_start(pc, 0xFFFFFFFF, count=maxins)
    except UcError as e:
        err = e
    pc = mu.reg_read(UC_ARM_REG_PC)
    if not mu.reg_read(UC_ARM_REG_CPSR) & 0x20:
        op = struct.unpack('<I', mu.mem_read(pc - 4, 4))[0]
        if op in (WFE, WFI):
            return op
    if err is not None:
        print('emu error', err, 'pc=%#x' % pc)
        raise err
    return None


mu = boot()
run(mu)
assert rd(mu, MB + 0x00) == 0xA9A9C0DE, 'no alive magic: %#x' % rd(mu, MB)
print('alive: MIDR=%#x MPIDR=%#x SCTLR=%#x ACTLR=%#x state=%u' % (
    rd(mu, MB + 4), rd(mu, MB + 8), rd(mu, MB + 0xc), rd(mu, MB + 0x14), rd(mu, MB + 0x24)))
assert rd(mu, MB + 0x24) == 1
if rd(mu, MB + 0x14) & 0x41 != 0x41:
    # Unicorn's (QEMU's) Cortex-A9 ACTLR ignores writes; on hardware Cpu1Status shows it
    print('note: ACTLR.SMP/FW not set (emulator ACTLR is read-only)')
assert rd(mu, MB + 0xc) & 0x1805 == 0, 'MMU/caches not off'

# ping
wr(mu, MB + 0x44, 0x12345678); wr(mu, MB + 0x40, 2)
run(mu)
assert rd(mu, MB + 0x18) == 2, 'no ack'
assert rd(mu, MB + 0x20) == (~0x12345678) & 0xffffffff, 'bad result %#x' % rd(mu, MB + 0x20)
print('ping ok: ack=%u result=%#x heartbeat=%u' % (rd(mu, MB + 0x18), rd(mu, MB + 0x20), rd(mu, MB + 0x10)))

# a second ping with a new sequence number (3 is "park for the OS", so 4)
wr(mu, MB + 0x44, 0xdeadbeef); wr(mu, MB + 0x40, 4)
run(mu)
assert rd(mu, MB + 0x18) == 4 and rd(mu, MB + 0x20) == (~0xdeadbeef) & 0xffffffff
print('second ping ok')

# park for the OS (CPU0's GICC_CTLR value passed in the mailbox)
wr(mu, PAGE + 0x0, 0x12345678)                  # garbage the stub must overwrite
wr(mu, PAGE + 0x8, 0x87654321)
wr(mu, MB + 0x48, 0x2)
wr(mu, MB + 0x40, 3)
assert run(mu) == WFI, 'not sleeping in WFI'
assert rd(mu, MB + 0x24) == 3, 'not parked for the OS: state %u' % rd(mu, MB + 0x24)
assert rd(mu, PAGE + 0x0) == 0xFFFFFFFF, 'ProcessorId %#x' % rd(mu, PAGE)
assert rd(mu, PAGE + 0x8) == 0 and rd(mu, PAGE + 0xc) == 0, 'JumpAddress not cleared'
assert rd(mu, GICC + 0x04) == 0x08, 'GICC_PMR %#x' % rd(mu, GICC + 4)
assert all(rd(mu, GICD + 0x400 + 4 * i) == 0 for i in range(4)), 'SGI priorities not 0'
assert rd(mu, GICC + 0x00) == 0x3, 'GICC_CTLR %#x' % rd(mu, GICC)
assert rd(mu, GICD + 0x100) == 0xFFFF, 'GICD_ISENABLER0 %#x' % rd(mu, GICD + 0x100)
print('parked for the OS: ProcessorId=%#x GICC_CTLR=%#x PMR=%#x' % (rd(mu, PAGE), rd(mu, GICC), rd(mu, GICC + 4)))

# a wake-up with nothing to do (SGI 0 from CPU0): acknowledged, ended, back to sleep
wr(mu, GICC + 0x0C, 0x00000400)                 # GICC_IAR: SGI 0 from CPU 1
assert run(mu) == WFI
assert rd(mu, GICC + 0x10) == 0x400, 'GICC_EOIR %#x' % rd(mu, GICC + 0x10)
assert rd(mu, MB + 0x28) == 1, 'wake count %u' % rd(mu, MB + 0x28)
assert rd(mu, MB + 0x24) == 3
print('empty wake-up ok: EOIR=%#x wakes=%u' % (rd(mu, GICC + 0x10), rd(mu, MB + 0x28)))

# a spurious wake-up (ID 1023): not ended
wr(mu, GICC + 0x10, 0)
wr(mu, GICC + 0x0C, 1023)
assert run(mu) == WFI
assert rd(mu, GICC + 0x10) == 0, 'spurious interrupt was ended'
print('spurious wake-up ok')

# a Windows interrupt (SPI 69) that got through anyway: ended and made pending again for CPU0
wr(mu, GICC + 0x0C, 69)
assert run(mu) == WFI
assert rd(mu, GICC + 0x10) == 69, 'SPI not ended'
assert rd(mu, GICD + 0x208) == 1 << 5, 'SPI not re-pended: ISPENDR2 %#x' % rd(mu, GICD + 0x208)
print('stray SPI handed back ok')

# the OS: JumpAddress first, then ProcessorId, then the SGI. Entry is Thumb:
#   mov r1, r0 ; movs r0, #0x5a ; b .
OS = PAGE + 0x100
mu.mem_write(OS, struct.pack("<HHH", 0x4601, 0x205a, 0xe7fe))
wr(mu, PAGE + 0x8, OS | 1); wr(mu, PAGE + 0xc, 0)
wr(mu, PAGE + 0x0, 1)
wr(mu, GICC + 0x0C, 0x00000400)
run(mu, maxins=200)
pc = mu.reg_read(UC_ARM_REG_PC)
assert mu.reg_read(UC_ARM_REG_CPSR) & 0x20, 'not in Thumb state'
assert pc == OS + 4, 'pc %#x' % pc
assert mu.reg_read(UC_ARM_REG_R0) == 0x5a
assert mu.reg_read(UC_ARM_REG_R1) == PAGE, 'r0 at the OS entry was %#x, not the parking page' % mu.reg_read(UC_ARM_REG_R1)
assert rd(mu, PAGE + 0x8) == 0 and rd(mu, PAGE + 0xc) == 0, 'JumpAddress not cleared before the jump'
assert rd(mu, MB + 0x24) == 4
assert rd(mu, GICC + 0x00) == 0 and rd(mu, GICC + 0x04) == 0, 'GIC CPU interface not handed over disabled'
assert rd(mu, 0xD1DFE00C) == 0xF0, "SCU tags of CPU1 not invalidated: %#x" % rd(mu, 0xD1DFE00C)
assert mu.reg_read(UC_ARM_REG_CPSR) & 0xC0 == 0xC0, 'interrupts not masked at the OS entry'
print('OS entry ok: pc=%#x (Thumb) r0=%#x JumpAddress cleared, state 4' % (pc, mu.reg_read(UC_ARM_REG_R0)))

# old behaviour: command 1 parks in WFI
mu = boot()
run(mu)
wr(mu, MB + 0x40, 1)
run(mu)
assert rd(mu, MB + 0x24) == 2, 'not parked'
print('parked in WFI ok')
print('PASS')
