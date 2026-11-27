/** @file
  Marvell PXA988 / PXA986 register map (subset used by the firmware).

  Derived from the Marvell PXA988 vendor kernel (3.4) and the goyawifi
  Linux 4.19 port.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef PXA988_H_
#define PXA988_H_

//
// Cortex-A9 MPCore private region
//
#define PXA988_L2C_BASE             0xD1DFB000
#define PXA988_SCU_BASE             0xD1DFE000
#define PXA988_GICC_BASE            0xD1DFE100
#define PXA988_GICD_BASE            0xD1DFF000

// ACPI parking-protocol pages of CPU0 and CPU1 (Pxa988SmpDxe), reserved by GoyaWifiPlatformPeiLib.
// A fixed address in DRAM, which a warm restart keeps, so the next boot can report how the
// previous OS session used CPU1's page.
#define PXA988_PARKING_PAGES        0x08180000

#define PL310_CTRL                  0x100
#define PL310_CACHE_SYNC            0x730
#define PL310_CLEAN_INV_WAY         0x7FC

//
// Power management units
//
#define PXA988_MPMU_BASE            0xD4050000
#define MPMU_UART_PLL               (PXA988_MPMU_BASE + 0x0014)
#define MPMU_CPRR                   (PXA988_MPMU_BASE + 0x0020)
#define MPMU_WDTPCR                 (PXA988_MPMU_BASE + 0x0200)
#define MPMU_APRR                   (PXA988_MPMU_BASE + 0x1020)
#define   MPMU_APRR_CPR             BIT0
#define   MPMU_APRR_WDTR            BIT4

#define PXA988_APMU_BASE            0xD4282800
#define APMU_SDH0                   (PXA988_APMU_BASE + 0x0054)
#define APMU_SDH1                   (PXA988_APMU_BASE + 0x0058)
#define APMU_USB                    (PXA988_APMU_BASE + 0x005C)
#define APMU_SDH2                   (PXA988_APMU_BASE + 0x00E0)
// Debug/trace clocks of the Cortex-A9 CoreSight logic (Linux clock-pxa988.c "DBGCLK"/"TRACECLK").
// With DBGCLK off, the extended CP14 debug registers are UNDEFINED, and Windows' kernel touches
// them during start-up.
#define APMU_TRACE                  (PXA988_APMU_BASE + 0x0108)
#define APMU_TRACE_DBGCLK_EN        (1U << 3)
#define APMU_TRACE_TRACECLK_EN      (1U << 4)
// Memory-mapped debug registers of core 0 (DBGDRAR + DBGDSAR = 0xD4100000 + 0x10000)
#define PXA988_CORE0_DEBUG_BASE     0xD4110000
#define   SDH_CLK_RST_N             BIT0
#define   SDH_CLK_AXI_RST_N         BIT1
#define   SDH_CLK_ACLK_EN           BIT3
#define   SDH_CLK_FCLK_EN           BIT4
#define   SDH_CLK_DIV(x)            (((x) & 7) << 8)
#define   SDH_CLK_FC                BIT11
#define PXA988_SDH_BASE_CLOCK       52000000    // pll1_416 / 8

#define PXA988_APBC_BASE            0xD4015000
#define APBC_UART1                  (PXA988_APBC_BASE + 0x0000)
#define APBC_GPIO                   (PXA988_APBC_BASE + 0x0008)
#define APBC_TIMERS0                (PXA988_APBC_BASE + 0x0034)
#define PXA988_APBCP_BASE           0xD403B000
#define APBCP_TWSI2                 (PXA988_APBCP_BASE + 0x0028)
#define   APBC_APBCLK               BIT0
#define   APBC_FNCLK                BIT1
#define   APBC_RST                  BIT2
#define   APBC_FNCLKSEL(x)          (((x) & 7) << 4)

//
// APB peripherals
//
#define PXA988_TIMER0_BASE          0xD4014000
#define PXA988_TIMER_FREQ           3250000
#define PXA988_TIMER0_IRQ           (32 + 13)
#define TMR_CCR                     0x00
#define TMR_TN_MM(n, m)             (0x04 + ((n) << 3) + (((n) + (m)) << 2))
#define TMR_PLCR(n)                 (0x58 + ((n) << 2))
#define TMR_IER(n)                  (0x40 + ((n) << 2))
#define TMR_ICR(n)                  (0x74 + ((n) << 2))
#define TMR_SR(n)                   (0x34 + ((n) << 2))
#define TMR_CER                     0x84
#define TMR_CMR                     0x88
#define TMR_CVWR(n)                 (0xA4 + ((n) << 2))

#define PXA988_UART1_BASE           0xD4017000  // debug UART, "AP UART0"
#define PXA988_UART_CLOCK           14745600

#define PXA988_GPIO_BASE            0xD4019000
#define GPIO_BANK(n)                (PXA988_GPIO_BASE + (((n) / 32) < 3 ? (((n) / 32) << 2) : (0x100 + ((((n) / 32) - 3) << 2))))
#define GPIO_GPLR                   0x00
#define GPIO_GPDR                   0x0C
#define GPIO_GPSR                   0x18
#define GPIO_GPCR                   0x24
#define GPIO_GSDR                   0x54
#define GPIO_GCDR                   0x60

#define PXA988_MFPR_BASE            0xD401E000
#define   MFPR_AF_MASK              0x7
#define MFPR_MMC1_DAT3              0x0094
#define MFPR_MMC1_DAT2              0x0098
#define MFPR_MMC1_DAT1              0x009C
#define MFPR_MMC1_DAT0              0x00A0
#define MFPR_MMC1_CMD               0x00A4
#define MFPR_MMC1_CLK               0x00A8
#define MFPR_GPIO1                  0x00E0
#define MFPR_GPIO2                  0x00E4
#define MFPR_GPIO11                 0x0108
#define MFPR_GPIO13                 0x0110
#define MFPR_GPIO37                 0x0170      // GPIO37..42 = MMC2 DAT3..DAT0, CMD, CLK (AF1): SDIO Wi-Fi
#define MFPR_GPIO42                 0x0184
#define MFPR_GPIO51                 0x01A8      // WLAN_PD (AF0)
#define MFPR_GPIO84                 0x01FC      // WLAN_WU_HOST (AF0)
#define GOYAWIFI_WLAN_PD_GPIO       51          // high = module powered (Linux wifi-pwrseq reset-gpios, active low)
#define   MFPR_PULL_MASK            (BIT15 | BIT14 | BIT13)
#define   MFPR_PULL_UP              (BIT15 | BIT14)

#define PXA988_KPC_PC               0xD4012000  // keypad controller
#define APBC_KPC                    (PXA988_APBC_BASE + 0x0030)
#define APMU_WAKE_CLR               (PXA988_APMU_BASE + 0x007C)
#define   APMU_WAKE_CLR_KPC         BIT3
#define GOYAWIFI_SD_CD_GPIO         11          // active low

#define PXA988_TWSI2_BASE           0xD4037000
#define PXA988_RTC_BR0              0xD4010014
#define   RTC_BR0_RECOVERY          0x7265636F  // "reco"

#define PXA988_WDT_BASE             0xD4080000
#define WDT_WMER                    (PXA988_WDT_BASE + 0x64)
#define WDT_WMR                     (PXA988_WDT_BASE + 0x68)
#define WDT_WVR                     (PXA988_WDT_BASE + 0x6C)
#define WDT_WSR                     (PXA988_WDT_BASE + 0x70)
#define WDT_WFAR                    (PXA988_WDT_BASE + 0x9C)
#define WDT_WSAR                    (PXA988_WDT_BASE + 0xA0)

//
// AXI peripherals
//
#define PXA988_SDH1_BASE            0xD4280000  // microSD  (Linux sdh1)
#define PXA988_SDH2_BASE            0xD4280800  // SDIO WiFi (Linux sdh2)
#define PXA988_SDH3_BASE            0xD4281000  // eMMC     (Linux sdh3)
#define PXA988_SDH_SIZE             0x800

//
// 88PM822 PMIC on TWSI2 (base page 0x30, power/regulator page 0x31)
//
#define PM822_I2C_ADDR              0x30
#define PM822_POWER_I2C_ADDR        0x31
#define PM800_STATUS_1              0x01
#define   PM800_ONKEY_STS           BIT0
#define PM800_LDO_ENA1_1            0x51
#define   PM800_LDO1_EN             BIT0        // v_tsp_3v3 (touchscreen vdd/vcca)
#define PM800_LDO1_VOUT             0x08        // low nibble: 88PM822 LDO1/2 table, 4 = 2.8 V
#define   PM822_LDO1_2V8            4
#define   PM800_LDO4_EN             BIT3        // v_sd_3v3  (microSD vmmc)
#define   PM800_LDO5_EN             BIT4        // v_mmc_3v3 (microSD vqmmc)
#define PM800_LDO_ENA1_2            0x52
#define   PM800_LDO10_EN            BIT1        // v_wib_3v3 (Marvell 88W8777 Wi-Fi/BT module)
#define PM800_LDO10_VOUT            0x13        // low nibble: 88PM822 LDO3..17 table, 15 = 3.3 V
#define   PM822_LDO_3V3             15

//
// Legacy ICU: its global IRQ/FIQ outputs must be masked, peripheral IRQs
// reach the GIC SPIs directly (see 4.19 pxa988-dt.c)
//
#define PXA988_ICU_BASE             0xD4282000
#define ICU_A9C0_GBL_INT_MSK        (PXA988_ICU_BASE + 0x114)
#define ICU_A9C1_GBL_INT_MSK        (PXA988_ICU_BASE + 0x144)
#define   ICU_GBL_IRQ_FIQ_MSK       0x3
#define PM800_WAKEUP1               0x0D
#define   PM800_SW_PDOWN            BIT5
#define PM822_GENERAL_USE_REG       0xEF
#define   PM822_SHUTDOWN_BY_POWEROFF 0xA0
//
// Reboot reason for S-Boot in the low 5 bits of PM822_GENERAL_USE_REG (the vendor
// kernel's reboot_notifier_func keeps bits 7:5): "recovery" 6, "recovery_done" 5,
// "arm11_fota" 4, "alarm" 9, "download" 0x1E. Read from TWRP's kernel on the SM-T110.
//
#define   PM822_REBOOT_REASON_MASK  0x1F
#define   PM822_REBOOT_RECOVERY     0x06

//
// Secondary core (CPU1) release, as in the vendor kernels
// (platsmp-pxa988.c / reset-pxa988.c)
//
#define PXA988_CIU_BASE             0xD4282C00
#define CIU_WARM_RESET_VECTOR       (PXA988_CIU_BASE + 0x00D8)
#define APMU_CORE_STATUS            (PXA988_APMU_BASE + 0x0090)
#define APMU_CC2_AP                 (PXA988_APMU_BASE + 0x0100)
#define   CC2_CPU1_RST             BIT20
#define   CC2_CPU1_DBG_RST         BIT22
#define   CC2_CPU1_WDOG_RST        BIT23
#define APMU_CORE_WAKEUP_CTL        (PXA988_APMU_BASE + 0x012C)
#define   APMU_WAKEUP_CORE1        BIT1

//
// USB 2.0 OTG controller (ChipIdea-style core) and UTMI PHY
//
#define PXA988_USB_BASE             0xD4208000
#define PXA988_USB_EHCI_BASE        0xD4208100  // EHCI capability registers
#define PXA988_USB_EHCI_SIZE        0x100
#define PXA988_USB_PHY_BASE         0xD4207000
#define USB_EHCI_USBMODE            0xA8        // from EHCI base
#define   USBMODE_CM_HOST          0x3
#define APMU_USB_CLK_EN             0x9         // AXI reset released + clock on

#endif // PXA988_H_
