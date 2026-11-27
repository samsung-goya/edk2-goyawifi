/* Minimal DSDT for Galaxy Tab 3 Lite (PXA986/988) - Windows RT bring-up */
DefinitionBlock ("Dsdt.aml", "DSDT", 5, "SAMSNG", "GOYAWIFI", 0x00000001)
{
  Scope (\_SB)
  {
    Device (CPU0) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0)
    }
    // CPU1 waits for the OS in its ACPI parking-protocol page (MADT GICC enabled). GoyaWifiAcpiDxe
    // patches STAV to 0 when the firmware could not park it (MADT GICC disabled), so the processor
    // object always matches the MADT.
    Device (CPU1) {
      Name (_HID, "ACPI0007")
      Name (_UID, 1)
      Name (STAV, 0x0F)
      Method (_STA) { Return (STAV) }
    }
    // Marvell MMP timer 0 HAL extension is matched by CSRT; this device lets the
    // INF install it (ACPI\MRVL0002)
    Device (TMR0) {
      Name (_HID, "MRVL0002")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4014000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 45 }
      })
    }
    // GPIO controller: 4 banks x 32 pins, one muxed interrupt (SPI 49 = GIC 81). GpioClx wants one
    // interrupt resource per bank, so the shared line is listed once per bank.
    Device (GPI0) {
      Name (_HID, "MRVL0003")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4019000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
      })
    }
    // TWSI1 (I2C bus 1): touchscreen (Zinitix BT532 @0x20) and display bridge. SpbCx controller driver
    // goyai2c; polled, so no interrupt. Second memory range = APBC_TWSI1 clock/reset register.
    // TWSI2 (PMIC) is deliberately not exposed: the firmware's runtime services use it.
    Device (I2C1) {
      Name (_HID, "MRVL0004")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4010800, 0x1000)
        Memory32Fixed (ReadWrite, 0xD4015060, 0x4)
      })
    }
    // TWSI2 (I2C bus 2): the 88PM822 PMIC (base page 0x30), the power I2C of the SoC. Hidden from
    // Windows (_STA 0): with goyai2c resetting and driving this unit Windows stopped intermittently with
    // KMODE_EXCEPTION_NOT_HANDLED outside any driver code, which fits disturbed PMIC/voltage traffic.
    // The firmware runtime services (reset/poweroff) keep using it.
    Device (I2C2) {
      Name (_HID, "MRVL0004")
      Name (_UID, 2)
      Method (_STA) { Return (0x0) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4037000, 0x1000)
        Memory32Fixed (ReadWrite, 0xD403B028, 0x4)
      })
    }
    // Zinitix BT532 touchscreen on I2C1 @0x20 (driver goyatouch polls it; no interrupt resource)
    Device (TCH0) {
      Name (_HID, "MRVL0005")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x20, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C1", 0, ResourceConsumer, , )
      })
    }
    // Hardware keys: volume up/down + home (GPIO 1/2/13, read directly). The power key (PMIC onkey on
    // I2C2) is not exposed while I2C2 is hidden.
    Device (KEY0) {
      Name (_HID, "MRVL0006")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
    }
    // Battery and charger (goyabatt, a battery miniclass driver): 88PM822 GPADC on TWSI2 (gauge and
    // thermistor), SMB328 charger and RT8973A MUIC on bit-banged GPIO buses. No resources on purpose: the
    // driver maps the blocks itself (TWSI2 stays hidden as I2C2, GPIO belongs to GPI0).
    Device (BAT0) {
      Name (_HID, "MRVL000A")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
    }
    // microSD host (SDH1) = Windows boot disk. HID MRVL0007 binds goyasdhc (Storport miniport, boot
    // start, polled PIO). No _CID PNP0D40 on purpose: the inbox sdbus.sys must never take this
    // controller (it cannot handle the PXAv3 quirks). The driver polls; the interrupt below is only there
    // for Storport (see the _CRS comment). Second range = APMU SDH0/SDH1 clock registers.
    // The firmware has already powered the slot (88PM822 LDO4/LDO5) and enabled the clocks.
    Device (SDH1) {
      Name (_HID, "MRVL0007")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      // SPI 39 = GIC IRQ 71, shared by SDH1/SDH2/SDH3. goyasdhc polls and keeps every SDH signal
      // enable at 0; the interrupt is listed only because Windows 8.0 (build 8520) Storport fails
      // the adapter start with STATUS_NOT_FOUND when there is no interrupt resource.
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4280000, 0x800)      // 0x800: SDH2 (Wi-Fi) starts at 0xD4280800
        Memory32Fixed (ReadWrite, 0xD4282854, 0x8)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 71 }
      })
    }
    // SDH2: SDIO controller of the Marvell 88W8777 Wi-Fi/Bluetooth module (Linux sdh2 0xD4280800,
    // clock APMU SDH1 0xD4282858). Driven by goyasdio.sys, an SDIO bus driver that enumerates the module's
    // functions (SD\VID_02DF&PID_9131 WLAN, PID_9132 Bluetooth). No _CID PNP0D40 on purpose (inbox
    // sdbus.sys must not take it). Polled like SDH1: the shared interrupt is listed but never signalled.
    // The firmware has powered the module (LDO10 3.3 V, WLAN_PD GPIO51 high) and set the MMC2 pins.
    Device (SDH2) {
      Name (_HID, "MRVL0009")
      Name (_UID, 2)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4280800, 0x800)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 71 }
      })
    }
    // Pin multiplexer (MFPR block), driven by goyapinmux
    Device (PMX0) {
      Name (_HID, "MRVL0008")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD401E000, 0x1000)
      })
    }
    // UART1 (16550-compatible PXA UART), SPI 27 = GIC IRQ 59
    Device (UAR1) {
      Name (_HID, "MRVL0001")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4017000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 59 }
      })
    }
  }
}
