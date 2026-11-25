# Windows RT on the Galaxy Tab 3 Lite — bring-up log

What it took to get Windows 8 build 8520 (Windows RT, ARM32) running on the
Samsung Galaxy Tab 3 Lite 7.0 Wi-Fi (SM-T110, `goyawifi`, Marvell PXA986,
Cortex-A9) with this EDK2 firmware: from the first stop screens to WinPE with
touch, keys and the microSD card, and a full Windows RT install on microSD.

Current status of every part (works / doesn't / untested): see
[README.md → Hardware status](README.md#hardware-status).

---

## Where it stands (2026-11-25)

* **WinPE** boots from microSD, runs for long periods without stopping, and has
  working **touch** (mouse pointer), **hardware keys** and the **microSD card as
  a disk with a drive letter**.
* **Full Windows RT** has been installed to a 16 GB microSD card and its first
  boot runs, with the card at full speed (26 MHz high-speed, 4-bit) and
  thousands of reads and writes without a single failure. Setup now finishes
  and Windows RT reaches the **desktop** (account "Goya", automatic logon), with
  touch and the SD card using DMA (section 12).
* Firmware to flash for Windows: `goyawifi-uefi-winrt-release.img.tar.md5`
  (DVFS off, RTC 2012-08-01, kernel fix-ups, DSDT with the SDH interrupt).

---

## 1. Early stop screens (kernel, HAL, ACPI)

| Stop | Cause | Fix |
|---|---|---|
| `0xC4 / 0x91` while the kernel starts | The 8520 kernel initialises the CPU's CP14 debug registers on every processor; they are fused off on this Cortex-A9 (undefined instruction) | **WinKernelFixupDxe**: at ExitBootServices, find the two calls to the per-processor debug-register setup in the loaded `ntoskrnl.exe` and replace them with NOPs (byte patterns from 6.2.8520.0) |
| `0x5C HAL_INITIALIZATION_FAILED` | CSRT timer descriptor used the wrong subtype | CSRT timer descriptors use Type 2 / **Subtype 0** |
| `KMODE_EXCEPTION_NOT_HANDLED` when drivers loaded | The compiler inlined NEON `memcpy`/`memset`; the 8.0 kernel does not allow NEON in drivers | `common/goyamem.c` (own NEON-free memcpy/memmove/memset) linked into every driver; CI fails on any NEON/VFP instruction |
| `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED` | goyagpio under the pre-release 8520 GPIO class extension (GpioClx v1.0) | goyagpio left out of cards and installs (nothing needs it) |

Also done along the way: Windows 8.0 GpioClx/SpbCx packet versions, Storport
buffer mapping through `StorPortGetSystemAddress`, test signing in CI with a
`signed-drivers` branch, `goya-diag` offline diagnostics for cards.

## 2. Seeing crashes: goyabug and the on-screen traces

The 8520 stop screen shows no parameters and WinPE writes no dump, so crashes
were invisible. Built:

* **goyabug** — boot-start driver using `KeRegisterBugCheckCallback` and the
  exported `KiBugCheckData`. At a stop it draws a box with the stop name, code,
  P1–P4, which driver each parameter points into, and a heuristic call chain.
  Later versions add: the instruction bytes at the exception address, the
  owning image found by walking back to its MZ header and reading its PDB name
  (works for images missing from the module list), drivers that have unloaded
  (`~name`), the caller from the trap frame, uptime, current process and the
  last loaded driver. Vol Up + Vol Down for 3 s forces a test stop (0xE2).
* **goyatrace** — every driver writes "`<id> <stage>`" lines and values at the
  top right of the screen; a caught exception becomes a red line with the
  driver, site, code and image offset. Ids: 1 goyasdhc, 2 goyapinmux,
  3 goyagpio, 4 goyai2c, 5 goyatouch, 6 goyakeys, 7 goyabug.
* **HalExtMmpTimer** draws progress squares; **WinKernelFixupDxe** draws a
  marker in the bottom-right corner (green = all kernel sites patched).
* CI rejects SDIV/UDIV too: the PXA988's Cortex-A9 has no hardware divide.

## 3. Random early KMODE stops

Windows sometimes stopped with KMODE_EXCEPTION_NOT_HANDLED before any driver
loaded.

* **HalExtMmpTimer counter read** — the 3-read sequence could return a wrong
  value while the counter was being latched. Now: CVWR latch request, at least
  12 reads, then read until two values agree, never go backwards. Several clean
  boots after this.
* **Asynchronous aborts** — a RELEASE firmware runs with aborts masked, so a bus
  error from firmware code would only be delivered once Windows unmasks them.
  WinKernelFixupDxe checks ISR.A at ExitBootServices and takes a pending abort
  with its own handler (marker cyan / blue if one was pending).

## 4. Touch (goyatouch)

The longest bring-up. In order:

1. **Driver model.** A KMDF driver under mshidkmdf, then a WDM HID minidriver
   (modelled on j5touch) never produced a pointer; the HID minidriver never even
   loaded because the touch device (TCH0) has no driver on the I2C1 bus.
   goyatouch became a **WDM mouse port driver** (like i8042prt) bound to the
   **I2C1 controller** (`ACPI\MRVL0004`) under mouclass: absolute mouse,
   first finger = pointer + left button. It drives TWSI1 itself (polled).
2. **Power.** The chip answered with NAK/bus errors (C0000185): its supply,
   **88PM822 LDO1**, was off (Samsung's kernel switches it on). The firmware now
   sets LDO1 to 2.8 V and enables it. Chip answers: code F400.
3. **Interrupt clear.** The Zinitix BT532 holds its interrupt line (GPIO94) low
   until it gets `CLEAR_INT`, which Samsung's driver sends after every read,
   even when the status is 0. goyatouch only sent it after a real touch, so the
   chip stopped reporting. Fixed, plus the GPIO94 pull-up, the vendor X/Y
   resolution (599/1023) and the 4 touch keys. **Touch works.**
4. **Freezes.** Touch sometimes froze until a key was pressed. Samsung's
   bt532 driver has an **ESD watchdog**: the chip sends a heartbeat interrupt
   every 100 scans (register 0x00F1) and is restarted after 3 silent seconds.
   goyatouch now does the same and releases a held click first (`5 7` counts
   restarts).

## 5. Hardware keys (goyakeys)

KMDF lower filter under mshidkmdf: Vol Up / Vol Down / Home from the GPIO
level register. Works. The power key has no driver yet (the PMIC's I2C2 bus is
hidden from Windows).

## 6. microSD card (goyasdhc)

goyasdhc is a Storport miniport with its own SD stack (polled PIO). It had been
taken off the card because the random stops were blamed on it; those also
happened without it. Getting it working, step by step (the Storport findings
come from disassembling build 8520's own `storport.sys`):

1. **Diagnostics** — on-screen stage, card info, request counters, last error,
   stage history, guarded callbacks; card init no longer retried on every
   request at DISPATCH_LEVEL (a dead card could have frozen Windows).
2. **Register range** — `StorPortGetDeviceBase` only maps an address whose
   resource entry has the same interface type as passed in; goyasdhc now tries
   the likely types, with its own mapping as a fallback.
3. **TaggedQueuing** — after FindAdapter, Storport fails the start
   (C0000182) unless NeedPhysicalAddresses, TaggedQueuing, ScatterGather and
   Master are all TRUE. TaggedQueuing was FALSE, so HwInitialize never ran.
4. **Interrupt** — the start also fails (C0000225) without an interrupt
   resource. The DSDT now lists the SD interrupt (GIC 71, shared by all three SD
   controllers); goyasdhc keeps every SD controller's signal enable at 0.
5. **DMA check (stop 0xE6 / 0x1F)** — as a DMA adapter, Storport runs DMA cache
   maintenance on every buffer and the kernel stops on buffers that aren't
   cache-line aligned. goyasdhc now reports **VirtualDevice**, which makes
   Storport skip DMA setup, the interrupt and those flushes. **The card works
   in WinPE with a drive letter**, and full Windows RT boots from it.

## 7. Crashes while idle (PatchGuard)

* A KMODE C000001D (illegal instruction) at an address in no module appeared
  once; the **WinRT firmware build now leaves DVFS off** (Windows has no CPU
  frequency or thermal control here, and 1066 MHz ran at the lowest allowed
  voltage on a supply that also feeds the RAM).
* WinPE then stopped after a few idle minutes with the same error. goyabug
  showed the instruction: `MRC p14, 0, r1, c0, c2, 2` (a debug register read).
  It is **PatchGuard**: its check routines run every few minutes from encrypted
  copies in memory and touch the debug registers when a "debug hardware
  present" flag is set. WinKernelFixupDxe gained a **third patch site** that
  keeps that flag clear when PatchGuard sets itself up at boot (before it takes
  its baseline). Checked in the emulator against the real kernel; WinPE has run
  idle without stopping since.

## 8. Full Windows RT install

* `install-winrt-sd.cmd` (on the PC, elevated): applies `install.wim` to the
  card, adds the Goya drivers and the test certificate, registers the HAL
  extension and goyabug, makes goyasdhc boot start, sets testsigning /
  nointegritychecks / onecpu, and adds an answer file that skips the setup
  screens (local account "Goya", no password).
* Fixed: goyapinmux is no longer added by default; the card is partitioned as
  **MBR** (FAT32 system + NTFS), because Windows refuses EFI/MSR partitions on
  removable card readers and diskpart didn't report the failure.
* First boot runs on a 16 GB card at full speed with no failed requests.

## 9. Firmware changes in this period

* WinKernelFixupDxe: three kernel patch sites (two CP14 debug-register setup
  calls, one PatchGuard flag) and the asynchronous-abort check.
* Pxa988SdhciDxe: LDO1 (touch supply) at 2.8 V.
* GoyaWifiAcpiDxe: CSRT subtype 0; SDH1 interrupt (GIC 71) in the DSDT.
* `tools/build-winrt.sh`: DVFS off for the Windows build (`GOYA_DVFS=1` keeps
  it); RTC starts at 2012-08-01 for the build 8520 timebomb.
* `tools/emu/fixup_test.py`: loads the whole kernel image and checks all three
  patch sites.

## 10. README

The README now has a full **Hardware status** section (firmware and Windows,
works / doesn't work / untested / not implemented) and an updated feature
table.

---

## 11. goyasdhc write fixes and goyalog (merged from jakaco/edk2-goyawifi, branch goyalog)

17 commits on top of 35e5bdc, each with its hardware run in the commit message:

* RequestTimerCall callbacks never came in WinPE (requests sat until Storport's
  timeout and a LUN reset): waits are done in place again (`USE_TIMER` keeps the
  timer path).
* About 1 in 10 multi-block writes never raised transfer complete although every
  block went out (BLKCNT 0, card still in the receive state: the controller left
  out the automatic CMD12). Tried and dropped because they lost data: finishing
  by idle data lines (run 6), sending our own CMD12 (run 9). ACMD22's block count
  is garbage on this controller unless the data line is reset first (run 10).
  Fix: CMD23 before every multi-block write, no auto CMD12; a stall with the card
  back in the transfer state is a complete write (run 12: 0 stalls, 0 retries,
  identical copy, clean FAT).
* 41.6 MHz: the APMU_SDH0 divider goes from 8 to 4 (83.2 MHz base); errors drop
  back to 26 MHz first.
* goyalog (see README), PMIC reboot reason for recovery, goyacmp.exe (byte
  compare for WinPE, which has no fc or script engine), WinPE flags in
  `\goyape`: bench.flag (speed test), twrp.flag, install.flag (copies
  `\goyape\install\goyasdhc.*` into the Windows RT install).
  `make-goya-sd.cmd` now puts goyacmp.exe and the goyasdhc package there.

## 12. Setup to the desktop, touch fix, SD card DMA

* **"Windows could not update the computer's boot configuration"**: Setup's
  boot-entry step (SpBcd, 0x3EE) failed because the BCD store on the card was
  missing `Description\System` and `Description\TreatAsSystem` (both DWORD 1).
  Without them the BCD library rejects the system store with 0xC0000098
  ("volume externally altered"); bootmgr doesn't check them, so the card boots
  anyway. `bcdboot /s` onto a removable card leaves them out.
  `install-winrt-sd.cmd` now adds them; WinPE's `fixbcd.flag` repairs an
  installed card.
* Setup recovery after a failed run (WinPE `resetsetup.flag`): move
  `Panther\SetupInfo*` away (the blackboard can't be reopened, GLE 8), reset
  ChildCompletion setup.exe=0, GeneralizationState=7 and UnattendPasses
  specialize=0.
* Touch: upstream 682359e (no ESD reset unless the chip really stopped; a
  reset recalibrated the panel with a finger on it). No resets were logged
  during the desktop session.
* WinPE `install.flag` installs every `NAME.sys` (with .inf/.cat) in
  `\goyape\install`.
* **SDMA in goyasdhc**: a 512 KiB uncached (write-combined) contiguous bounce
  buffer inside one 512 KiB SDMA boundary; data commands set XFER bit 0 and
  the DMA interrupt rewrites the address. A failed DMA request is repeated by
  PIO, and three failures in a row turn DMA off. `tools/sdsim` models SDMA
  (boundary pause, error injection) and runs the whole suite in PIO and SDMA.
  WinPE speed test, 158.6 MB (run 13 PIO → run 14 SDMA): FAT32 read 14.9 →
  11.8 s, uncached read 15.0 → 12.1 s (about 13 MB/s), NTFS write 9.1 → 9.7 s
  (card-limited); copies identical, 1729 SDMA transfers, 0 failed. Windows RT
  desktop session: 873 of 890 requests by SDMA, 0 failed.

---

## 13. Wi-Fi: the Surface RT Marvell driver on our own SDIO bus

The Surface RT's Wi-Fi was a Marvell 88W8797 on SDIO with `mwls97w8arm.sys` (14.62.17215.91 ARM, from
DriversStore/mwls97); our module is the 88W8777 from the same family. Findings from the binary:
- It is Marvell's MLAN driver (the same code base as the vendor Linux driver `sd8xxx`/`mlan` for the
  8777, which treats the 8777 as an 8787 with another firmware file). No NEON/VFP (its memcpy is integer
  only), imports only ntoskrnl/HAL/NDIS: it fits the 8520 kernel.
- Firmware: `NdisOpenFile("mv14sxx97.bin")`, the built-in 8797 image only as fallback. The SM-T110
  `sd8777_uapsta.bin` (w8777-A0 FP66 14.66.33.p46, goyawifi vendor blobs) installed under that name.
- SD bus contract of its statically linked sdbus.lib: interface GUID, sizes, IOCTL 0x43073, the 'SDIC'
  context signature, the properties and commands it uses (goyasdio README).

New:
- **goyasdio.sys** (WDM SDIO bus driver, ACPI\MRVL0009 on SDH2): card init, CMD52/CMD53 in PIO, child PDOs
  `SD\VID_02DF&PID_9131&FN_1` / `PID_9132&FN_2`, the SD bus interface, card interrupt polled at 1 ms.
  `sdiocore.c` tested by `tools/sdiosim` (CI).
- **goyawifi** package built in CI by `tools/make-goyawifi-pkg.py` from pinned, hash-checked inputs.
- DSDT `Device (SDH2)`; SDH1's register range trimmed to 0x800 (it overlapped SDH2).
- Firmware: MMC2 pins, LDO10 3.3 V, WLAN_PD (GPIO51) released after 10 ms (Linux wifi-pwrseq).

Not tested on hardware yet. Open risks: the FP62 MLAN with the FP66 8777 firmware (multi-port aggregation
gave Linux trouble on this chip), no calibration data download (the stock Android driver did not use one
either), and the 1 ms polling instead of an interrupt.

---

## 14. Battery and charging (goyabatt)

* **goyabatt**: a KMDF battery miniclass driver under battc.sys for the DSDT
  device `BAT0` (`ACPI\MRVL000A`, no resources: the driver maps GPIO, MFPR,
  TWSI2 and its APBC clock itself, so I2C2 stays hidden). Every 2 s it:
  * reads the battery voltage from the 88PM822 GPADC (VBAT on page 0x32) and
    the thermistor (GPADC2 with bias-current stepping);
  * talks to the SMB328 charger (0x34, bit-banged on GPIO16/17, CHG_EN GPIO98).
    This keeps its watchdog fed, so the tablet charges under Windows;
  * reads the cable type from the RT8973A MUIC (0x14, GPIO49/50).
* Charging stops when the battery is too hot (45 °C) or too cold (0 °C), after
  a 6-hour session, on a charger fault, or above 4.4 V.
* There is no coulomb counter. The percentage comes from an OCV table, with
  voltage corrections for charge current. It is filtered and rate-limited, never
  rises on battery, never falls while charging, and stops at 99 % until the
  charger reports full.
* `tools/battsim` runs the shared core (`battcore.c`) against pin-level models
  of the SMB328 and MUIC and a register model of the PMIC/GPADC. It covers
  plug/unplug, watchdog, temperature, full, empty, a missing MUIC, a stuck SDA
  line and PMIC loss, and runs on CI.
* **Installing it into an installed card**: WinPE's DISM can't service the
  offline image. `dismhost.exe` fails with 0x80004002 before the driver is even
  looked at. `adddrv.flag` therefore tries DISM first, then falls back to
  `:regdrv` using `NAME.regdrv` (HWID, class, class GUID, KMDF version from the
  INF). The fallback:
  * copies the .sys and backs up the SYSTEM hive to the card;
  * creates the service key;
  * points every existing device instance of the HWID at the service, with the
    class and `ConfigFlags` 0, which clears the "failed install" flag 0x40.
  This WinPE has `find.exe` but no `findstr.exe`.
* Result: Windows RT shows the battery with its charge level, and the charging
  status follows plugging and unplugging the cable (tested with the HID
  MRVL0009, before SDH2 took it; renamed to MRVL000A when rebasing onto the
  Wi-Fi work). A card that got goyabatt that way keeps a stale
  `ACPI\MRVL0009\0` entry (SDH2 is `\2`, so goyabatt never attaches to it);
  with the MRVL000A firmware the battery is a new device and needs
  `adddrv.flag` once more.

---

## Next

* **goyasdhc**: CMD23 writes, 41.6 MHz and SDMA done (sections 11, 12).
  Later: the real interrupt instead of waiting in place; ADMA2 to skip the
  bounce copy.
* **GPU (goyagpu, Vivante GC1000)**: steps 1–2 done — shared KMD/UMD ABI and the hardware
  layer (power, reset, identification, MMUv1, front-end ring, submission), checked against the
  Linux etnaviv sequences by `tools/gpusim/ringtest.c`. Next: the WDDM 1.2 KMD; see
  `WinRT/drivers/goyagpu/README.md` for verified facts and open questions.
* **goyabatt** (section 14): check the percentage against a full
  charge/discharge cycle and tune the OCV table; confirm a full charge reaches
  100 %.
* **Touch died during a Windows session** (cause unknown; the next WinPE boot
  overwrote the goyabug log).
* Power key and screen brightness; the second CPU core (firmware support for
  Windows' core start-up); Bluetooth (SDIO function 2 is enumerated).
* Turn off hibernation / Fast Startup on the Windows RT card (goyasdhc's
  hibernate path is untested).
* Still to confirm: goyabug's test stop, the on-screen keyboard (goyaosk) with
  touch, goyapinmux with a trimmed mask, persistent UEFI variables.
