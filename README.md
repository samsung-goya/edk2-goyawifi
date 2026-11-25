# edk2-goyawifi

UEFI firmware (TianoCore EDK2) for the **Samsung Galaxy Tab 3 Lite 7.0 Wi-Fi
(SM-T110, `goyawifi`)**, built on the **Marvell PXA986** (PXA988 family,
2x Cortex-A9, 1 GiB LPDDR2).

> **Status: boots on hardware.** UEFI (boot menu, setup, hardware keys,
> diagnostics app, eMMC and microSD) runs on the tablet, and Windows 8 build
> 8520 (Windows RT, ARM32) **WinPE boots from microSD with working touch and
> keys**. Full working / not working list: [Hardware status](#hardware-status).
> DEBUG builds print their log **on the screen**, so no UART is needed (see
> [Debugging](#debugging)).
>
> How Windows RT was brought up, step by step: [BRINGUP-LOG.md](BRINGUP-LOG.md).

## How it boots

The Samsung boot chain (BootROM → OBM → S-Boot) is signed and stays in place.
Like lk2nd or the U-Boot port, this firmware replaces the *Linux kernel*:

```
S-Boot ──loads KERNEL partition boot.img──> 0x10008000 (our FD)
   FD+0x0000  zImage-style header (Header/FdHeader.S), magic 0x016f2818 @ +0x24;
              if entered anywhere but 0x10008000, it copies the FD there first
   FD+0x1000  FVMAIN_COMPACT: zero vector patched by GenFv -> PeilessSec
              PeilessSec: L2 off, UART, MMU, HOBs, LZMA-decompress FvMain
              DXE: GIC, MMP timer, GOP on S-Boot's splash, SD/eMMC, BDS
```

S-Boot leaves DRAM, PLLs, the PMIC and the display (LCD + DSI + VX5B3D
bridge, 1024x600 a8r8g8b8 at `0x17000000`) initialised, so the firmware only
reuses them.

It can also be started from the goyawifi U-Boot port without flashing:
`fastboot boot goyawifi-uefi-debug-boot.img`. U-Boot runs uncompressed
Android images in place from its fastboot buffer, and the header stub
relocates the FD. The bootmenu entry "UEFI firmware" also works: it loads
`GOYAWIFI_EFI.fd` from microSD and starts it with `bootz`.

## Layout

```
Silicon/Marvell/Pxa988Pkg/            reusable SoC support
  Include/Pxa988.h                    register map
  Library/MmpTimerLib                 TimerLib on the MMP APB timer (A9 has no generic timer)
  Library/PxaUartSerialPortLib        PXA UART (16550 + UUE bit), sets up its clock
  Library/Pxa988PlatformLib           ArmPlatformLib: L2 hand-off, MPCore info, memory map
  Library/Pxa988ResetSystemLib        WDT reset, 88PM822 power-off, "recovery" reboot
  Drivers/MmpTimerDxe                 EFI_TIMER_ARCH_PROTOCOL (timer 0 / GIC SPI 13)
  Drivers/SimpleFbDxe                 GOP on the pre-initialised framebuffer
  Drivers/Pxa988SdhciDxe              SDHCI clocks/pinmux + SdMmcOverride (PXAv3 quirks)
Platform/Samsung/GoyaWifiPkg/
  GoyaWifi.dsc / GoyaWifi.fdf         platform description, FD at 0x10008000 (2 MiB)
  Library/GoyaWifiPlatformPeiLib      FV HOB + reserved regions (OBM, CP, ramoops, FB)
  Drivers/GoyaWifiKeypadDxe           Vol Up / Vol Down / Home as Up / Down / Enter
  Drivers/GoyaWifiAcpiDxe             ACPI 5.0 tables for Windows RT
  Drivers/WinKernelFixupDxe           Windows 8.0 kernel fixes at ExitBootServices
  Drivers/GoyaWifiChargerDxe          SMB328 charger
WinRT/                                Windows RT: HAL extension, drivers, WinPE/install scripts, goyaosk
  Dtb/pxa986-samsung-goyawifi.dtb     DT installed for the OS (from the 4.19 kernel tree)
tools/mkbootimg-goyawifi.py           boot.img (+ Odin .tar.md5) packer
build.sh                              fetch EDK2, build, pack
```

## EDK2 version

TianoCore removed 32-bit ARM after **`edk2-stable202508.01`**, which is the
tag `build.sh` checks out. Moving to a newer tag would mean carrying ARM
support out of tree.

## Building

```bash
sudo apt install gcc-arm-linux-gnueabi uuid-dev python3 build-essential
./build.sh            # DEBUG (serial logging)
./build.sh RELEASE
```

Outputs `out/goyawifi-uefi-boot.img` and `out/goyawifi-uefi-boot.img.tar.md5`.
Set `EDK2=/path/to/edk2` to use an existing checkout of the same tag.

## Flashing

Download mode: **Volume Down + Home + Power**, then

```bash
heimdall flash --KERNEL out/goyawifi-uefi-boot.img
# or Odin: AP/PDA = goyawifi-uefi-boot.img.tar.md5
```

`RECOVERY` is untouched (Volume Up + Home + Power). To get back to Android,
flash a stock/LineageOS `boot.img` to `KERNEL`.

## What is implemented

| Area | Implementation | Notes |
|---|---|---|
| CPU / MMU / caches | ArmPkg CpuDxe, short-descriptor MMU | L2 (PL310) cleaned and left **off** |
| Interrupts | A9GicDxe (ArmGicV2Dxe copy) | A9 MPCore GIC @ `0xD1DFF000` / `0xD1DFE100`; upstream driver can't map the unaligned A9 CPU interface |
| Timer | MmpTimerDxe + MmpTimerLib | 3.25 MHz APB timer, 10 ms tick |
| Serial | PxaUartSerialPortLib | UART1 `0xD4017000`, 115200 8N1; DEBUG log only, no serial console in ConIn/ConOut |
| Display | SimpleFbDxe | reuses S-Boot's splash buffer, no re-init |
| Storage | SdMmcPciHcDxe via NonDiscoverablePciDeviceDxe | eMMC (SDH3, 26 MHz, clk-delay 15) and microSD (SDH1, 50 MHz, GPIO 11 CD, LDO4/LDO5 enabled) |
| Input | GoyaWifiKeypadDxe | Vol Up / Vol Down = Up / Down, Home = Enter, Power = Esc; keys taken out of keypad-controller mode with pad pull-ups (as the vendor kernel does) |
| Interrupt routing | Pxa988PlatformLib | legacy ICU global IRQ/FIQ outputs masked (`0xD4282114/144`), as 4.19 `pxa988_dt_init_irq()` |
| PMIC | Pm822Lib | polled TWSI2; base page `0x30`, power page `0x31` |
| On-screen log | PxaUartSerialPortLib | DEBUG builds mirror the log to the framebuffer (8x19 font, 128x31) |
| CPU1 | Pxa988SmpDxe | **experimental self-test**: releases the second core with the vendor-kernel sequence (`CIU_WARM_RESET_VECTOR` + `APMU_CC2_AP` reset bits), runs a tiny MMU-off stub (`Cpu1Stub.S`) that reports MIDR/MPIDR, answers a ping and parks, then puts the core back into reset before ExitBootServices (the OS releases it itself). Not an MP-services implementation. `PcdSecondaryCpuBringup` turns it off |
| USB host | Pxa988UsbDxe + ChipIdeaEhciDxe + UsbBus/UsbKb/UsbMassStorage | **experimental**: APMU USB clock, UTMI PHY calibration (vendor values), controller reset, host mode, EHCI at `0xD4208100`. ChipIdeaEhciDxe is EhciDxe plus a `USBMODE` write after every controller reset. **No VBUS control** (needs a powered hub / OTG Y cable); the vendor kernels only ever use this port as a device. `PcdUsbHostEnable` turns it off |
| Diagnostics | GoyaWifiDiagApp | boot menu entry "GoyaWifi Diagnostics" (also startable from the Shell: `fs0:` is not needed, it lives in the firmware volume). Read-only pages: system (firmware, CPU, **timer interrupt test**, GIC, ICU, RAM, CPU1 result), board (88PM822, display, SDH1/SDH3 registers and card detect, disks), bring-up drivers (`Cpu1Status`, `UsbStatus`, live EHCI registers), a colour test and a live key test with raw GPIO/ONKEY levels next to the UEFI key events. Every register block is only read when its clock is on. Inspired by RossaDiag (notnoelchannel2/rossadiag) |
| Reset / power off | Pxa988ResetSystemLib | WDT; 88PM822 `SW_PDOWN` over TWSI2 |
| Variables | emulated (RAM) | not persistent across reboots |
| OS hand-off | GoyaWifiAcpiDxe + DtPlatformDxe | ACPI 5.0 (FADT HW_REDUCED, FACS, MADT, CSRT, SPCR, DSDT) for Windows; DT config table for Linux |
| Windows kernel fixes | WinKernelFixupDxe | at ExitBootServices: stops the 8520 kernel from touching the locked CP14 debug registers; takes a pending asynchronous abort (marker in the bottom-right corner) |
| Touch supply | Pxa988SdhciDxe | 88PM822 LDO1 (touch vdd) set to 2.8 V and switched on if S-Boot left it off |
| USB device | Pxa988UsbDxe (UsbGadget.c) | CDC-ACM serial console (VID 1209 / PID 0001) when the core refuses host mode |
| DVFS | Pxa988DvfsDxe | one-shot move to 1066 MHz (PLL2), `PcdDvfsEnable`; **off in the Windows RT build** (`tools/build-winrt.sh`, `GOYA_DVFS=1` keeps it): Windows has no frequency or thermal control here |
| Charging | GoyaWifiChargerDxe | SMB328 at 500 mA while UEFI runs |

Not implemented: EFI_MP_SERVICES (CPU1 is only self-tested), VBUS switching,
persistent variables.

## Hardware status

Everything said in this project's test rounds so far, as of 2026-11-21.
"Works" means seen working on the tablet; "untested" means built and booted but
the result was never checked or reported.

### Firmware (UEFI)

**Works**

* Boot from the KERNEL partition through S-Boot (Odin/heimdall flash), and from
  the U-Boot port with `fastboot boot` (self-relocating FD header).
* On-screen DEBUG log; display through S-Boot's framebuffer, colours correct
  (BGRA).
* GIC and MMP timer interrupts (timer interrupt test passes in the diag app).
* Boot menu, Device Manager / setup screens and the UEFI Shell.
* Hardware keys: Vol Up / Vol Down = Up / Down, Home = Enter, Power = Esc.
* Second core self-test: CPU1 comes up (MPIDR 0x80000001) and answers the ping.
* eMMC (7456 MB plus two 2 MB boot partitions).
* microSD (SDH1, LDO4/LDO5 power, GPIO11 card detect): Windows boots from it.
* GoyaWifi Diagnostics app (all pages, key test, colour test).
* ACPI tables: accepted by the Windows 8 build 8520 kernel and HAL (after the
  CSRT timer descriptor was changed to subtype 0).
* WinKernelFixupDxe CP14 fix (green marker; before it the 8520 kernel stopped
  early).
* Touch supply: firmware switches on LDO1 at 2.8 V; without it the touch chip
  never answers.
* Runtime services under Windows (ResetSystem, timer and PMIC bases converted
  at SetVirtualAddressMap).
* Windows RTC workaround: `tools/build-winrt.sh` images start the clock at
  2012-08-01 for the build 8520 timebomb.

**Doesn't work**

* USB host: the ChipIdea core stays in device mode (USBMODE reads 2 after
  writing 3) and the PHY PLL-ready bit never sets. There is also no VBUS
  switching, so a powered hub / OTG Y cable would be needed anyway.

**Untested / result not reported**

* USB device-mode CDC-ACM serial console (`dmpstore UsbStatus`).
* DVFS to 1066 MHz (`dmpstore DvfsStatus`).
* SMB328 charging in UEFI (`dmpstore ChargerStatus`); CHG_EN polarity
  (GPIO98) may need flipping. The SMB328 watchdog stops charging shortly after
  UEFI exits.
* Asynchronous-abort check at ExitBootServices: bottom-right marker colour
  (green = no pending abort, cyan = one was pending and taken, blue = pending
  and not cleared) not yet reported over several boots.
* `acpiview` on hardware.
* Booting Linux through the EFI stub (needs a kernel without
  `ARM_SAMSUNG_PXA_BOOTFIXUP`).

**Not implemented**

* Persistent UEFI variables (still emulated in RAM). The microSD card is ruled
  out (it holds only Windows RT); an eMMC area hasn't been chosen.
* EFI_MP_SERVICES; VBUS control.

### Windows 8 build 8520 (Windows RT, ARM32)

Drivers live in `WinRT/`; CI builds and test-signs them (`signed-drivers`
branch). WinPE cards: `WinRT/winpe/make-goya-sd.cmd`; full install:
`WinRT/install/install-winrt-sd.cmd`.

**Works**

* WinPE boots from microSD to the command prompt, reliably since the
  HalExtMmpTimer counter fix.
* **HalExtMmpTimer** (timer HAL extension, MMP timer, CSRT): latch-safe,
  monotonic counter read. Its older 3-read sequence caused random early
  KMODE_EXCEPTION_NOT_HANDLED stops before any driver loaded.
* **goyatouch** (WDM mouse port driver on the I2C1 controller, ACPI\MRVL0004):
  touch works as an absolute mouse pointer (first finger = pointer + left
  button) through mouclass. It drives TWSI1 itself and programs the Zinitix
  BT532 at 0x20 like the vendor driver; it clears the chip interrupt whenever
  INT (GPIO94, pull-up on) is low.
* **goyakeys** (KMDF lower filter under mshidkmdf): Vol Up, Vol Down and Home.
* **goyabatt** (KMDF battery miniclass under battc, ACPI `MRVL000A` = DSDT
  `BAT0`, needs this firmware): battery percentage, "plugged in / charging" and
  charging itself. The SMB328 is programmed like GoyaWifiChargerDxe and polled
  every 2 s so its watchdog never stops charging; charging stops outside
  0..45 C, above 4.4 V or after 6 h. No coulomb counter on this tablet: the
  percentage comes from the battery voltage (88PM822 GPADC), corrected for the
  charge/load current, filtered and rate-limited, so it is approximate in the
  flat middle of the curve. `tools/battsim` tests it against simulated
  hardware. Works on the tablet (battery level, charging status follows the
  cable). On an installed card add it with WinPE `adddrv.flag` (BRINGUP-LOG
  section 14).
* **goyasdhc** (Storport microSD miniport, polled PIO): the microSD card works
  in WinPE as a disk with a drive letter. On WinPE cards by default
  (`GOYA_SDHC=0` leaves it out). Build 8520's Storport needed:
  `TaggedQueuing` (with `NeedPhysicalAddresses`, `ScatterGather`, `Master`) or
  the start fails with C0000182; `VirtualDevice` so it skips the interrupt
  connection, DMA setup and `KeFlushIoBuffers` (otherwise stop 0xE6/0x1F on
  buffers that aren't cache-line aligned); `StorPortGetDeviceBase` with the
  interface type the resources carry. The DSDT also lists the SDH interrupt
  (GIC 71), now unused.
  Data goes through the controller's data port directly. Multi-block writes
  announce their length with CMD23: about 1 in 10 writes never raised
  transfer complete on this controller (each cost an abort and re-send); with
  CMD23 a test run had 0 stalls, 0 retries and an identical copy. 41.6 MHz high
  speed (base clock raised to 83.2 MHz), falling back to 26 MHz and the lower
  speed levels on errors. Card waits are done in place: in WinPE, requests
  deferred to Storport's RequestTimerCall were never called back. Full history
  in the commit messages of 2852194..97def4d (hardware runs 1-12).
* **goyalog**: goyabug keeps a text log (boot number, drivers and programs
  loaded, DbgPrint, stop screen) in the reserved RAM at 0x08100000; after a
  watchdog reset the firmware saves it as `\goya\logs\boot-NNNN-<state>.txt`
  on the card's FAT32 system partition and, unless Windows shut down cleanly,
  restarts into recovery (TWRP) so the log can be pulled with adb.
  `\goya\reboot-recovery` on that partition also restarts into recovery
  (PMIC reboot reason 6, which S-Boot honours; RTC_BR0 alone is ignored).
* **goyabug** (bugcheck callback): loads early ("7 01"); draws the stop code,
  parameters and the faulting driver on the stop screen.
* Display through the firmware framebuffer (1024x600).
* Test signing (testsigning + nointegritychecks in the BCD).
* Driver build: no NEON in drivers (own memcpy/memset in `common/goyamem.c`;
  the 8.0 kernel stops with KMODE_EXCEPTION_NOT_HANDLED otherwise).
* Fixed stops along the way: 0xC4, 0x5C (CSRT subtype), KMODE (NEON memcpy,
  timer read), SYSTEM_THREAD (goyagpio, left out).

**Doesn't work / left off the card**

* **goyagpio** (GpioClx): the 8520 GPIO class extension stops with
  SYSTEM_THREAD_EXCEPTION_NOT_HANDLED. Left out.
* **goyapinmux**: left off until its MFPR mask is trimmed.
* **goyai2c** (SpbCx): replaced by goyatouch on I2C1 (using both conflicts);
  I2C2 (PMIC) is hidden from Windows (`_STA` 0).
* Power key: no driver (KEY0 has no resource while I2C2 is hidden).
* Touch through the HID stack (hidclass / mshidkmdf minidriver): never gave a
  pointer; replaced by the mouse port driver.
* TCH0 (MRVL0005) in the DSDT has no driver.

**Untested / result not reported**

* goyabug's test stop (Vol Up + Vol Down held 3 s → stop 0xE2).
* goyaosk on-screen keyboard with touch.
* Full Windows RT install on microSD (`install-winrt-sd.cmd`): now possible to
  try, since goyasdhc works as the boot disk driver.
* goyasdhc at 41.6 MHz on a full Windows RT boot (runs 1-12 were WinPE with
  CMD23 at 26 MHz; the 41.6 MHz build was installed but its run not reported).
* Whether the random early stop is completely gone (one more since the timer
  fix, before the async-abort check).

**Under investigation**

* Random KMODE_EXCEPTION_NOT_HANDLED with C000001D after a few idle minutes:
  PatchGuard's check routines (ntoskrnl INITKDBG code, run from encrypted
  copies in pool, so "in no module") read DBGDSCRext through CP14, which is
  fused off on this Cortex-A9. WinKernelFixupDxe now clears the flag that
  makes them do it (third patch site). Fix not yet confirmed on hardware.

* **Wi-Fi (goyasdio + goyawifi), untested on hardware**: goyasdio is an SDIO
  bus driver for SDH2 (DSDT `MRVL0009`) that enumerates the Marvell 88W8777 and
  gives its WLAN function the Windows SD bus interface; goyawifi is the Surface
  RT Marvell driver (`mwls97w8arm.sys` 14.62.17215.91, same MLAN code base)
  with the SM-T110 `sd8777_uapsta.bin` installed as its firmware file. The
  firmware powers the module (LDO10, WLAN_PD GPIO51). Details:
  [WinRT/drivers/goyasdio/README.md](WinRT/drivers/goyasdio/README.md).

**Not available**

* Second core in Windows (CPU1 disabled in the MADT).
* Bluetooth, audio, sensors, camera, USB, GPU acceleration (no drivers).

## Second core and USB: what to look for

DEBUG builds print (on the screen log) lines starting with `Pxa988Smp:` and
`Pxa988Usb:`. A healthy CPU1 prints `CPU1 is up: MIDR=0x410fc09x MPIDR=0x80000001`
(core ID 1 in the low bits), `CPU1 ping OK` and a heartbeat that increases.
`CPU1 did not come up` means the release sequence does not work as in the
kernel (CC2 and the warm-reset vector are printed for debugging).
`tools/emu/cpu1_stub_test.py` runs the stub alone in Unicorn.

## Memory map

| Range | Use |
|---|---|
| `0x00000000-0x000FFFFF` | OBM / BootROM, reserved |
| `0x06000000-0x080FFFFF` | CP carveout (hardware protected by S-Boot), reserved |
| `0x08100000-0x0817FFFF` | ramoops / last_kmsg, reserved |
| `0x10008000-0x10207FFF` | firmware device (FD) |
| `0x17000000-0x17257FFF` | framebuffer (write-through), reserved |
| `0x3C000000-0x3FFFFFFF` | UEFI PI memory + stack |

## Booting Linux

32-bit ARM Linux boots through its EFI stub (`CONFIG_EFI=y`,
`CONFIG_EFI_STUB=y`) using the DT this firmware installs. The goyawifi 4.19
kernel uses `CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP` (it forces decompression to
physical `0x00108000` for S-Boot), which isn't EFI-stub aware. For EFI boot,
build that kernel with the fixup **off** and the regions above kept reserved
(they are, through the EFI memory map). That kernel-side change hasn't been
tested yet.

## Emulated smoke test

`tools/emu/boot_emu.py` runs a packed boot.img on an emulated Cortex-A9
(Unicorn), entering at the U-Boot fastboot buffer (`0x30000800`). Peripherals
are faked as RAM, with a UART that's always ready, a counting timer and a
plausible `GICD_TYPER`. It prints the UART log and saves the emulated screen.
It can't deliver interrupts, so it stops at the BDS countdown. It still
catches link, SEC, decompression and DXE dispatch failures before anything
touches the tablet.

```bash
pip install unicorn pillow
python3 tools/emu/reloc_test.py out/goyawifi-uefi-debug-boot.img <SEC entry from FVMAIN_COMPACT.Fv.map>
python3 tools/emu/boot_emu.py out/goyawifi-uefi-debug-boot.img 4000000000 screen.png
```

## Debugging

* **Screen (DEBUG builds):** from the very first SEC line, everything printed
  through `DEBUG()` also appears on the panel in white on black. The log
  wraps to the top when it reaches the bottom; the blank line marks where the
  newest output is. If the tablet hangs, a photo of the screen shows the last
  thing that ran. The log state lives in the page right after the framebuffer
  (`0x17258000`), so every module shares one cursor.
* **UART:** UART1 is routed to micro-USB by the RT8973A MUIC with a
  **619 kOhm JIG** (ID to GND), at 3.3 V levels, 115200 8N1. DEBUG builds log
  everything from SEC onwards.
* **Splash stays, nothing printed:** the image never reached SEC's
  `ArmPlatformInitialize` (the first thing it does is clear the screen).
* **Hangs before any output:** the most likely culprits are, in order:
  1. S-Boot rejecting or not jumping to the image (check the header at FD+0x24),
  2. S-Boot entering in a mode other than SVC/MMU off (PeilessSec expects the
     Linux boot protocol),
  3. the UART clock (`MPMU_UART_PLL`, `APBC_UART1`).
* **Hangs after "DXE":** check the GIC (secure vs non-secure entry) and the
  timer IRQ number (SPI 13 → ID 45).
* `ramoops` at `0x08100000` survives a warm reset, so it's possible to dump
  a log from Linux/TWRP if UART isn't available.

## Credits / sources

* Hardware description: the goyawifi Linux 4.19 port
  (`samsung-goya/android_kernel_samsung_goyawifi`), the Samsung 3.4 vendor
  kernel and the Marvell PXA988 BSP.
* Structure modelled on ArmVirtPkg and edk2-platforms.

License: BSD-2-Clause-Patent (same as EDK2).
