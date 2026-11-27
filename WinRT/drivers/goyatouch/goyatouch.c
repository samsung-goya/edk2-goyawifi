/* Zinitix BT532 touchscreen (Samsung SM-T110) as an absolute mouse - WDM mouse port driver.

   Windows sees the touchscreen as a mouse: the first finger moves the pointer and holds the left button
   (tap = click, drag = drag). WinPE has mouse support but no touch input.

   Model: like a mouse port driver (i8042prt, sermouse). The device is in the Mouse class, mouclass.sys
   is the class upper filter; it connects with IOCTL_INTERNAL_MOUSE_CONNECT and this driver hands it
   MOUSE_INPUT_DATA (MOUSE_MOVE_ABSOLUTE, 0..65535) through the class service callback. Only
   ntoskrnl/hal are imported: no hidclass.sys, KMDF, mshidkmdf, SpbCx or goyai2c. (The HID minidriver
   version, which imports HIDCLASS.SYS, was never loaded by WinPE 8520 - most likely a dependency of
   hidclass.sys (WppRecorder.sys) is missing there; the KMDF/mshidkmdf version never got a mouse either.)

   I2C: TWSI1 (0xD4010800, PXA "i2c-pxa" register layout), polled, 400 kHz; APBC TWSI1 clock register
   0xD4015060; GPIO87/88 are routed to TWSI1 by this driver. The addresses are fixed for the PXA986, so
   no ACPI resources are used. The INF binds to the I2C1 controller device (ACPI\MRVL0004), not to the
   touch device TCH0 (MRVL0005): TCH0 declares an I2cSerialBus connection on I2C1, and Windows never
   started it while I2C1 had no driver - goyatouch was never loaded. goyatouch replaces goyai2c. Touch chip at 7-bit address 0x20, protocol
   from the 4.19 kernel drivers/input/touchscreen/zinitix.c (point mode 0, 5 fingers). A system thread
   polls the point-status register every 10 ms (no interrupt).

   Status on screen (top right, see common\goyatrace.h), redrawn every second by a thread started in
   DriverEntry so that the desktop cannot paint over it:
     "5 <stage>"  00 driver loaded, 02 device added, 10 started, 11 started and mouclass connected,
                  1F start failed
     "5 0 <x>"    result of the last chip initialisation (00000000 = OK, else NTSTATUS; FFFFFFFF = none yet)
     Once the chip runs: "5 0" = last non-zero point status (4 digits), read failures (2), sub_status of
     finger slot 0 (2); "5 1" = non-zero statuses seen (4), out-of-range fingers (1), events sent (3).
     "5 1 <x>"    after a failed initialisation: 3 = both I2C lines high (idle) in the top digit (bit 28 SDA,
                  bit 29 SCL), APBC TWSI1 clock register in digits 3-4, I2C status register (ISR) in the low
                  4 digits; after the chip answered: chip code (high 16 bits) + mouse events sent (low 16)
                  "5 0" C000000E = no ACK from the chip (absent / unpowered), C0000185 = bus error or
                  arbitration lost (lines stuck), C00000B5 = controller timeout
*/
#define GOYA_TRACE_ID 5
#include <ntddk.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include "..\common\goyatrace.h"

/* ---- hardware --------------------------------------------------------------------------------- */

#define TWSI1_PHYS      0xD4010800ULL
#define TWSI1_LEN       0x100
#define APBC_TWSI1_PHYS 0xD4015060ULL
#define MFPR_TWSI1_PHYS 0xD401E208ULL   /* MFPR GPIO87 (SCL), GPIO88 (SDA) */
#define MFPR_TWSI1_MASK 0x038F          /* from the vendor DT twsi1_pins (goyapinmux table) */
#define MFPR_TWSI1_VAL  0x028D          /* AF5 = TWSI1, drive/pull as in the DT */

#define I2C_IBMR   0x00             /* bus monitor: bit 0 SDA, bit 1 SCL (1 = high) */
#define I2C_IDBR   0x08
#define I2C_ICR    0x10
#define I2C_ISR    0x18
#define ICR_START  (1UL << 0)
#define ICR_STOP   (1UL << 1)
#define ICR_ACKNAK (1UL << 2)
#define ICR_TB     (1UL << 3)
#define ICR_MA     (1UL << 4)
#define ICR_SCLE   (1UL << 5)
#define ICR_IUE    (1UL << 6)
#define ICR_GCD    (1UL << 7)
#define ICR_UR     (1UL << 14)
#define ICR_FM     (1UL << 15)
#define ISR_ACKNAK (1UL << 1)
#define ISR_ALD    (1UL << 5)
#define ISR_ITE    (1UL << 6)
#define ISR_IRF    (1UL << 7)
#define ISR_BED    (1UL << 10)
#define APBC_APBCLK (1UL << 0)
#define APBC_FNCLK  (1UL << 1)
#define APBC_RST    (1UL << 2)
#define I2C_POLL_LOOPS 3000         /* x 10 us = 30 ms per byte */

#define Z_ADDR     0x20

/* panel / DT: touchscreen-size 600x1024, inverted-x, swapped-x-y => screen 1024 x 600 */
#define RAW_MAX_X 600
#define RAW_MAX_Y 1024
#define NFINGERS  5
#define POLL_MS   10

#define Z_SWRESET        0x0000
#define Z_WAKEUP         0x0001
#define Z_CLEAR_INT      0x0003
#define Z_CALIBRATE      0x0006
#define Z_TOUCH_MODE     0x0010
#define Z_CHIP_REVISION  0x0011
#define Z_FW_VERSION     0x0012
#define Z_DATA_VERSION   0x0013
#define Z_FINGER_NUM     0x0015
#define Z_INIT_MODE      0x0019
#define Z_POINT_STATUS   0x0080
#define Z_BUTTON_NUM     0x00b0
#define Z_X_RES          0x00c0
#define Z_Y_RES          0x00c1
#define Z_INT_ENABLE     0x00f0
#define Z_PERIODIC_INT   0x00f1     /* periodical interrupt interval in scans (Samsung bt532_ts.c ESD) */
#define Z_ICON_STATUS    0x00aa
#define Z_VENDOR_ENABLE  0xc000
#define Z_PROGRAM_START  0xc001
#define Z_NVM_INIT       0xc002
#define Z_INTN_CLEAR     0xc004
#define Z_CHIP_CODE      0xcc00

#define BIT_PT_CNT_CHANGE (1u << 0)
#define BIT_DOWN          (1u << 1)
#define BIT_MOVE          (1u << 2)
#define BIT_UP            (1u << 3)
#define BIT_PT_EXIST      (1u << 11)
#define BIT_MUST_ZERO     (1u << 13)
#define BIT_ICON_EVENT    (1u << 15)
#define Z_BUTTONS         4        /* DT linux,keycodes: 4 entries (menu/back touch keys) */

/* touch interrupt line TSP_INT = GPIO94 (bank 2 bit 30), active low; used as "data ready" when polling */
#define GPIO_PHYS        0xD4019000ULL
#define GPIO94_BANK      0x008
#define GPIO94_BIT       (1UL << 30)
#define GPIO_GPLR        0x00
#define GPIO_GPDR        0x0C
#define GPIO_GPSR        0x18
#define GPIO_GPCR        0x24
#define GPIO_GSDR        0x54
#define GPIO_GCDR        0x60
/* TWSI1 pins as GPIOs for the bus recovery: GPIO87 = SCL, GPIO88 = SDA (bank 2, bits 23/24) */
#define GPIO_TWSI1_BANK  0x008
#define GPIO87_BIT       (1UL << 23)
#define GPIO88_BIT       (1UL << 24)
#define MFPR_GPIO94_PHYS 0xD401E224ULL
#define SUB_BIT_EXIST     0x01
/* ESD watchdog as in Samsung's bt532_ts.c: the chip raises an interrupt every 100 scans (~1 s) even
   without a touch; if it stays silent for 3 s it has locked up and is initialised again. */
#define Z_SCAN_HZ         100
#define WD_TICKS          (3000 / POLL_MS)     /* 3 s without an interrupt: ask the chip (probe) */
#define WD_LOCK_TICKS     (10000 / POLL_MS)    /* 10 s without the heartbeat it used to send: locked up */
#define Z_INT_MASK        (BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP | BIT_ICON_EVENT)
#define Z_RETRIES 8

typedef struct _DEV_EXT {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Next;
    PUCHAR         Twsi;            /* mapped TWSI1 */
    PULONG         Clock;           /* mapped APBC TWSI1 clock register */
    KSPIN_LOCK     Lock;            /* protects Connect */
    CONNECT_DATA   Connect;         /* mouclass device + service callback */
    BOOLEAN        Connected;
    PKTHREAD       Thread;
    KEVENT         StopEvent;
    BOOLEAN        Initialized;
    BOOLEAN        Down;            /* last button state sent */
    BOOLEAN        Started;
    BOOLEAN        Removed;
    ULONG          InitTries;
    LONG           LastX, LastY;
    ULONG          Tick;            /* time since the poll thread started, in POLL_MS units (not a poll count:
                                       the wait lasts a whole system timer period, ~15.6 ms, or more) */
    ULONG          NextInit;        /* Tick at which the next init try is due */
    ULONG          LastAlive;       /* Tick of the last sign of life from the chip */
    ULONG          LastProbe;       /* Tick of the last register probe by the watchdog */
    BOOLEAN        HbSeen;          /* the chip sent its periodic (heartbeat) interrupt since the last init */
    ULONG          ReadFailRun;     /* status reads in a row that failed with INT low */
} DEV_EXT;

/* status shown by the heartbeat thread */
static volatile ULONG gStage, gInit = 0xFFFFFFFF, gInfo, gChip, gReports;
static volatile ULONG gLastStatus, gLastSub, gReadFails, gNonZero, gRejected;
static volatile ULONG gPolls, gRaw4, gReadback, gIntInfo, gIntLow, gWatchdog;
static PUCHAR gGpio;                 /* mapped GPIO block (read-only use + GCDR) */
/* TWSI1 clock register (APBC) and bus recovery, shown as "5 8" .. "5 A":
   gClkDiag  = value found when it was wrong (31:24) | value read back after our fix (23:16) |
               times it was found wrong (15:0)
   gRecDiag  = bus recoveries (31:16) | SCL pulses needed last time (15:8) | line levels before (7:4) and
               after (3:0) the last recovery (bit 1 SDA, bit 0 SCL; 3 = both high)
   gClkLost  = poll number at which a poll first found the clock wrong (0 = never) */
static volatile ULONG gClkDiag, gRecDiag, gClkLost;
static PULONG gClk;
/* Power-cycle requests to goyabatt (it owns the PMIC bus), through the named notification event
   \BaseNamedObjects\GoyaTouchPowerCycle: we set it after NAK_CYCLE failed inits in a row in which the
   chip did not even acknowledge its address, goyabatt switches LDO1 off and on and clears it.
   Shown as "5 B": requests (31:24) | answered (23:16) | failed inits in a row that were NAKs (15:0). */
#define NAK_CYCLE        4
#define CYCLE_GAP_TICKS  (30000 / POLL_MS)     /* at most one request per 30 s */
static PKEVENT gPwrEv;
static HANDLE gPwrEvH;
static volatile ULONG gPwr;
static ULONG gNakRun, gPwrTick, gNextEvOpen;
static BOOLEAN gPwrPending;

/* extra status lines "5 <n> <value>", n = 2.., from row 21 down, in a second column left of the
   drivers' column: goyasdio (id 8) uses rows 21-23 of that column, and 10 lines would not fit below it
   (32 rows of 18 px on the 600 px screen) */
static VOID TraceExtra(ULONG N, ULONG Value)
{
    PULONG fb = GtFb();
    ULONG y = 8 + (7 + 7 * 2 + (N - 2)) * 6 * GT_SCALE, x = GT_FB_W - 2 * 12 * 4 * GT_SCALE;
    if (fb == NULL) return;
    x = GtHex(fb, x, y, GOYA_TRACE_ID, 1, 0x00FFFFFF, 0x00202020);
    x = GtHex(fb, x, y, N, 1, 0x00808080, 0x00202020);
    GtHex(fb, x, y, Value, 8, 0x0000FFFF, 0x00202020);
}
/* state changes to the boot log (goyalog), stamped with the poll tick in seconds; capped so a dead chip
   cannot flood it */
static ULONG gLogLines, gLostTick, gLostRun;
#define TLOG(E, fmt, ...) do { if (gLogLines < 300) { gLogLines++; \
    DbgPrint("goyatouch: %u.%02u s " fmt "\n", (E)->Tick / (1000 / POLL_MS), (E)->Tick % (1000 / POLL_MS) * POLL_MS / 10, __VA_ARGS__); } } while (0)

static BOOLEAN gChipOk;
static KEVENT gHbStop;
static PKTHREAD gHbThread;

static VOID HeartbeatThread(PVOID Context)
{
    LARGE_INTEGER t;
    UNREFERENCED_PARAMETER(Context);
    t.QuadPart = -10000000LL;               /* 1 s */
    do {
        GoyaTraceStep(gStage);
        if (!gChipOk) {
            GoyaTraceValue(0, gInit);
            GoyaTraceValue(1, gInfo);
        } else {
            /* chip running: slot 0 = last non-zero point status (high 16) | read failures (bits 15:8) |
               sub_status of the first finger slot (bits 7:0); slot 1 = non-zero statuses seen (high 16) |
               fingers rejected as out of range (bits 15:12) | mouse events sent (bits 11:0) */
            GoyaTraceValue(0, (gInit != 0) ? gInit : ((gLastStatus << 16) | ((gReadFails & 0xFF) << 8) | (gLastSub & 0xFF)));
            GoyaTraceValue(1, ((gNonZero & 0xFFFF) << 16) | ((gRejected & 0xF) << 12) | (gReports & 0xFFF));
        }
        /* 5 2: polls done | 5 3: INT_ENABLE readback (high) TOUCH_MODE readback (low) |
           5 4: GPIO94 MFPR (high 16) | direction (bit 4, 1 = output) | level now (bit 0) | 5 5: first 4 raw bytes
           of the last point-status read | 5 6: polls that saw the interrupt line low |
           5 7: ESD watchdog re-inits (bits 31:24) | watchdog probes that found the chip fine (23:16) |
           heartbeat seen since the last init (bit 15) | read-failure re-inits (14:0) */
        TraceExtra(2, gPolls);
        TraceExtra(3, gReadback);
        if (gGpio != NULL) {
            ULONG lvl = READ_REGISTER_ULONG((PULONG)(gGpio + GPIO94_BANK + GPIO_GPLR));
            ULONG dir = READ_REGISTER_ULONG((PULONG)(gGpio + GPIO94_BANK + GPIO_GPDR));
            gIntInfo = (gIntInfo & 0xFFFF0000) | ((dir & GPIO94_BIT) ? 0x10 : 0) | ((lvl & GPIO94_BIT) ? 1 : 0);
        }
        TraceExtra(4, gIntInfo);
        TraceExtra(5, gRaw4);
        TraceExtra(6, gIntLow);
        TraceExtra(7, gWatchdog);
        TraceExtra(8, gClkDiag);
        TraceExtra(9, gRecDiag);
        TraceExtra(10, gClk != NULL ? ((READ_REGISTER_ULONG(gClk) & 0xFF) << 24) | (gClkLost & 0xFFFFFF) : 0xEEEEEEEE);
        TraceExtra(11, gPwrEv != NULL ? (gPwr & 0xFFFF0000) | (gNakRun & 0xFFFF) : 0xEEEEEEEE);
    } while (KeWaitForSingleObject(&gHbStop, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static DEV_EXT *GetExt(PDEVICE_OBJECT Dev) { return (DEV_EXT *)Dev->DeviceExtension; }

static ULONG Rd(DEV_EXT *E, ULONG Off) { return READ_REGISTER_ULONG((PULONG)(E->Twsi + Off)); }
static VOID  Wr(DEV_EXT *E, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((PULONG)(E->Twsi + Off), V); }

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ---- polled PXA I2C master (same sequences as goyai2c / Linux i2c-pxa) --------------------- */

static VOID I2cReset(DEV_EXT *E)
{
    if (E->Clock != NULL) {
        ULONG v = READ_REGISTER_ULONG(E->Clock);
        if ((v & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) != (APBC_APBCLK | APBC_FNCLK)) {
            WRITE_REGISTER_ULONG(E->Clock, (v & ~APBC_RST) | APBC_APBCLK | APBC_FNCLK);
            KeStallExecutionProcessor(10);
            gClkDiag = ((v & 0xFF) << 24) | ((READ_REGISTER_ULONG(E->Clock) & 0xFF) << 16) | ((gClkDiag + 1) & 0xFFFF);
        }
    }
    Wr(E, I2C_ICR, ICR_UR);
    Wr(E, I2C_ISR, 0x7FF);
    Wr(E, I2C_ICR, 0);
    Wr(E, I2C_ICR, ICR_GCD | ICR_SCLE | ICR_FM);
    Wr(E, I2C_ICR, Rd(E, I2C_ICR) | ICR_IUE);
    KeStallExecutionProcessor(100);
}

static volatile ULONG gLastIsr, gLastIbmr;

/* Errors: NAK (BED + ACKNAK: nobody answered) -> STATUS_NO_SUCH_DEVICE, arbitration lost / other bus
   error -> STATUS_IO_DEVICE_ERROR, no completion -> STATUS_IO_TIMEOUT. The ISR is kept for the screen. */
static NTSTATUS I2cWait(DEV_EXT *E, ULONG Bit)
{
    ULONG n;
    for (n = 0; n < I2C_POLL_LOOPS; n++) {
        ULONG isr = Rd(E, I2C_ISR);
        if (isr & (ISR_BED | ISR_ALD)) {
            gLastIsr = isr;
            Wr(E, I2C_ISR, ISR_BED | ISR_ALD);
            return ((isr & ISR_BED) && (isr & ISR_ACKNAK) && !(isr & ISR_ALD)) ? STATUS_NO_SUCH_DEVICE : STATUS_IO_DEVICE_ERROR;
        }
        if (isr & Bit) { Wr(E, I2C_ISR, Bit); return STATUS_SUCCESS; }
        KeStallExecutionProcessor(10);
    }
    gLastIsr = Rd(E, I2C_ISR);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS I2cTx(DEV_EXT *E, UCHAR Data, ULONG Flags)
{
    NTSTATUS st;
    Wr(E, I2C_IDBR, Data);
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | Flags | ICR_TB | ICR_IUE | ICR_SCLE);
    st = I2cWait(E, ISR_ITE);
    if (NT_SUCCESS(st) && (Rd(E, I2C_ISR) & ISR_ACKNAK)) st = STATUS_NO_SUCH_DEVICE;
    return st;
}

static NTSTATUS I2cRx(DEV_EXT *E, PUCHAR Data, BOOLEAN Last)
{
    NTSTATUS st;
    ULONG f = ICR_TB | ICR_IUE | ICR_SCLE | (Last ? (ICR_ACKNAK | ICR_STOP) : 0);
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_STOP | ICR_ACKNAK | ICR_TB)) | f);
    st = I2cWait(E, ISR_IRF);
    if (NT_SUCCESS(st)) *Data = (UCHAR)Rd(E, I2C_IDBR);
    return st;
}

/* Unit reset through the APBC register (RST with the clocks on, then released), as the vendor
   i2c-pxa does after a hang; the ICR_UR soft reset alone did not bring a timed-out unit back. */
static VOID I2cHardReset(DEV_EXT *E)
{
    if (E->Clock != NULL) {
        ULONG v = READ_REGISTER_ULONG(E->Clock) & ~(APBC_APBCLK | APBC_FNCLK | APBC_RST);
        WRITE_REGISTER_ULONG(E->Clock, v | APBC_APBCLK | APBC_FNCLK | APBC_RST);
        KeStallExecutionProcessor(20);
        WRITE_REGISTER_ULONG(E->Clock, v | APBC_APBCLK | APBC_FNCLK);
        KeStallExecutionProcessor(20);
    }
}

/* I2C bus recovery (I2C spec 3.1.16): a slave stopped in the middle of a byte holds SDA low until it
   gets the rest of its clocks. Switch GPIO87/88 to GPIO, release both (the pull-ups take them high),
   clock SCL up to 9 times until SDA is released, send a STOP, then give the pins back to TWSI1. */
static ULONG GpioLevels(VOID)
{
    ULONG l = READ_REGISTER_ULONG((PULONG)(gGpio + GPIO_TWSI1_BANK + GPIO_GPLR));
    return ((l & GPIO88_BIT) ? 2 : 0) | ((l & GPIO87_BIT) ? 1 : 0);
}

static VOID GpioLine(ULONG Bit, BOOLEAN High)
{
    PUCHAR b = gGpio + GPIO_TWSI1_BANK;
    if (High) {
        WRITE_REGISTER_ULONG((PULONG)(b + GPIO_GCDR), Bit);         /* input: released, pulled up */
    } else {
        WRITE_REGISTER_ULONG((PULONG)(b + GPIO_GPCR), Bit);         /* output low */
        WRITE_REGISTER_ULONG((PULONG)(b + GPIO_GSDR), Bit);
    }
    KeStallExecutionProcessor(5);
}

static VOID I2cBusRecover(VOID)
{
    PHYSICAL_ADDRESS pa;
    PULONG m;
    ULONG a, b, before, n = 0;

    if (gGpio == NULL) return;
    pa.QuadPart = MFPR_TWSI1_PHYS;
    m = (PULONG)MmMapIoSpace(pa, 8, MmNonCached);
    if (m == NULL) return;
    a = READ_REGISTER_ULONG(&m[0]);
    b = READ_REGISTER_ULONG(&m[1]);
    GpioLine(GPIO87_BIT, TRUE);
    GpioLine(GPIO88_BIT, TRUE);
    WRITE_REGISTER_ULONG(&m[0], a & ~7UL);                          /* AF0 = GPIO */
    WRITE_REGISTER_ULONG(&m[1], b & ~7UL);
    KeStallExecutionProcessor(10);
    before = GpioLevels();
    while (n < 9 && !(GpioLevels() & 2)) {
        GpioLine(GPIO87_BIT, FALSE);
        GpioLine(GPIO87_BIT, TRUE);
        n++;
    }
    GpioLine(GPIO88_BIT, FALSE);                                    /* STOP: SDA low -> high while SCL high */
    GpioLine(GPIO87_BIT, TRUE);
    GpioLine(GPIO88_BIT, TRUE);
    gRecDiag = ((gRecDiag + 0x10000) & 0xFFFF0000) | (n << 8) | (before << 4) | GpioLevels();
    WRITE_REGISTER_ULONG(&m[0], a);
    WRITE_REGISTER_ULONG(&m[1], b);
    MmUnmapIoSpace(m, 8);
}

static VOID I2cRecover(DEV_EXT *E)
{
    Wr(E, I2C_ICR, (Rd(E, I2C_ICR) & ~(ICR_START | ICR_TB | ICR_ACKNAK)) | ICR_STOP | ICR_MA);
    KeStallExecutionProcessor(50);
    I2cReset(E);
}

/* One complete write (START addr+W data... STOP) */
static NTSTATUS I2cWrite(DEV_EXT *E, const UCHAR *Buf, ULONG Len)
{
    NTSTATUS st;
    gLastIbmr = Rd(E, I2C_IBMR);                 /* line levels before START: 3 = both high (idle) */
    st = I2cTx(E, (UCHAR)(Z_ADDR << 1), ICR_START);
    ULONG i;
    for (i = 0; NT_SUCCESS(st) && i < Len; i++) {
        st = I2cTx(E, Buf[i], (i == Len - 1) ? ICR_STOP : 0);
    }
    if (!NT_SUCCESS(st)) I2cRecover(E);
    return st;
}

/* One complete read (START addr+R data... NAK STOP) */
static NTSTATUS I2cRead(DEV_EXT *E, PUCHAR Buf, ULONG Len)
{
    NTSTATUS st = I2cTx(E, (UCHAR)((Z_ADDR << 1) | 1), ICR_START);
    ULONG i;
    for (i = 0; NT_SUCCESS(st) && i < Len; i++) {
        st = I2cRx(E, &Buf[i], (BOOLEAN)(i == Len - 1));
    }
    if (!NT_SUCCESS(st)) I2cRecover(E);
    return st;
}

/* ---- Zinitix protocol --------------------------------------------------------------------- */

static NTSTATUS ZSend(DEV_EXT *E, const UCHAR *d, ULONG len)
{
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    int i;
    for (i = 0; i < Z_RETRIES; i++) {
        st = I2cWrite(E, d, len);
        if (NT_SUCCESS(st)) return st;
        KeStallExecutionProcessor(1000);
    }
    return st;
}

static NTSTATUS ZReadData(DEV_EXT *E, USHORT reg, PUCHAR out, ULONG len)
{
    UCHAR r[2];
    NTSTATUS st;
    r[0] = (UCHAR)reg; r[1] = (UCHAR)(reg >> 8);
    st = ZSend(E, r, 2);                     /* a combined transfer does not work with this chip */
    if (!NT_SUCCESS(st)) return st;
    KeStallExecutionProcessor(50);
    st = I2cRead(E, out, len);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZReadU16(DEV_EXT *E, USHORT reg, USHORT *v)
{
    UCHAR b[2];
    NTSTATUS st = ZReadData(E, reg, b, 2);
    if (NT_SUCCESS(st)) *v = (USHORT)(b[0] | (b[1] << 8));
    return st;
}

static NTSTATUS ZWriteU16(DEV_EXT *E, USHORT reg, USHORT val)
{
    UCHAR p[4];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8); p[2] = (UCHAR)val; p[3] = (UCHAR)(val >> 8);
    st = ZSend(E, p, 4);
    KeStallExecutionProcessor(15);
    return st;
}

static NTSTATUS ZCmd(DEV_EXT *E, USHORT reg)
{
    UCHAR p[2];
    NTSTATUS st;
    p[0] = (UCHAR)reg; p[1] = (UCHAR)(reg >> 8);
    st = ZSend(E, p, 2);
    KeStallExecutionProcessor(15);
    return st;
}

#define CHK(x) do { st = (x); if (!NT_SUCCESS(st)) return st; } while (0)

/* zinitix_start() + power-on sequence + zinitix_init_touch() for the BT532 (mode 0) */
static NTSTATUS ZInitChip(DEV_EXT *E)
{
    NTSTATUS st;
    USHORT chip_code = 0, rev = 0, fw = 0, dv = 0;
    int i;

    I2cReset(E);
    SleepMs(15);
    ZCmd(E, Z_WAKEUP);
    SleepMs(20);

    CHK(ZWriteU16(E, Z_VENDOR_ENABLE, 1));
    CHK(ZReadU16(E, Z_CHIP_CODE, &chip_code));
    CHK(ZCmd(E, Z_INTN_CLEAR));
    CHK(ZWriteU16(E, Z_NVM_INIT, 1));
    SleepMs(2);
    CHK(ZWriteU16(E, Z_PROGRAM_START, 1));
    SleepMs(20);

    CHK(ZCmd(E, Z_SWRESET));
    SleepMs(15);
    CHK(ZReadU16(E, Z_CHIP_REVISION, &rev));
    CHK(ZReadU16(E, Z_FW_VERSION, &fw));
    CHK(ZReadU16(E, Z_DATA_VERSION, &dv));
    gChip = chip_code;
    gChipOk = TRUE;

    CHK(ZWriteU16(E, Z_INT_ENABLE, 0));
    CHK(ZCmd(E, Z_SWRESET));
    SleepMs(15);
    /* exactly what the vendor driver writes: touchscreen-size 600 x 1024 -> max 599 / 1023, 4 keys */
    CHK(ZWriteU16(E, Z_X_RES, RAW_MAX_X - 1));
    CHK(ZWriteU16(E, Z_Y_RES, RAW_MAX_Y - 1));
    CHK(ZWriteU16(E, Z_BUTTON_NUM, Z_BUTTONS));
    CHK(ZWriteU16(E, Z_FINGER_NUM, NFINGERS));
    CHK(ZWriteU16(E, Z_INIT_MODE, 0));
    CHK(ZWriteU16(E, Z_TOUCH_MODE, 0));
    CHK(ZCmd(E, Z_CALIBRATE));
    CHK(ZWriteU16(E, Z_INT_ENABLE, Z_INT_MASK));
    for (i = 0; i < 10; i++) { ZCmd(E, Z_CLEAR_INT); KeStallExecutionProcessor(15); }
    CHK(ZWriteU16(E, Z_PERIODIC_INT, Z_SCAN_HZ));          /* heartbeat interrupt ~1 s, for the watchdog */
    {
        USHORT ie = 0, tm = 0;
        ZReadU16(E, Z_INT_ENABLE, &ie);
        ZReadU16(E, Z_TOUCH_MODE, &tm);
        gReadback = ((ULONG)ie << 16) | tm;
    }
    return STATUS_SUCCESS;
}

/* ---- mouse input to mouclass ------------------------------------------------------------------ */

static VOID SendMouse(DEV_EXT *E, LONG X, LONG Y, USHORT ButtonFlags)
{
    MOUSE_INPUT_DATA d;
    ULONG consumed = 0;
    KIRQL irql;

    RtlZeroMemory(&d, sizeof(d));
    d.UnitId = 0;
    d.Flags = MOUSE_MOVE_ABSOLUTE;
    d.ButtonFlags = ButtonFlags;
    d.LastX = X;
    d.LastY = Y;

    KeAcquireSpinLock(&E->Lock, &irql);             /* the class service runs at DISPATCH_LEVEL */
    if (E->Connected && E->Connect.ClassService != NULL) {
        ((PSERVICE_CALLBACK_ROUTINE)(ULONG_PTR)E->Connect.ClassService)(E->Connect.ClassDeviceObject, &d, &d + 1, &consumed);
        gReports++;
    }
    KeReleaseSpinLock(&E->Lock, irql);
}

/* ---- polling ---------------------------------------------------------------------------------- */

/* The chip is about to be initialised again: a finger that was down is reported as lifted, so Windows
   does not keep the left button held. */
static VOID ReleaseButton(DEV_EXT *E)
{
    if (E->Down) {
        SendMouse(E, E->LastX, E->LastY, MOUSE_LEFT_BUTTON_UP);
        E->Down = FALSE;
    }
}

static VOID PollOnce(DEV_EXT *E)
{
    UCHAR raw[4 + 6 * NFINGERS];
    USHORT status;
    BOOLEAN intLow;
    BOOLEAN down = FALSE;
    LONG x = E->LastX, y = E->LastY;
    int i;

    gPolls++;
    if (gClkLost == 0 && E->Clock != NULL &&
        (READ_REGISTER_ULONG(E->Clock) & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) != (APBC_APBCLK | APBC_FNCLK)) {
        gClkLost = gPolls;
    }
    intLow = (BOOLEAN)(gGpio != NULL && !(READ_REGISTER_ULONG((PULONG)(gGpio + GPIO94_BANK + GPIO_GPLR)) & GPIO94_BIT));
    if (intLow) gIntLow++;
    /* Talk to the chip only when it asks (INT low), like the vendor driver. Between scans the chip
       does not acknowledge its address; reading the status on every poll regardless made it look lost
       every 10-45 s, and each "recovery" (re-init, recalibration) was a visible touch lag. */
    if (gGpio != NULL && !intLow) return;
    {
    NTSTATUS rs = ZReadData(E, Z_POINT_STATUS, raw, sizeof(raw));
    if (!NT_SUCCESS(rs)) {
        /* INT stays low until CLEAR_INT, so the next poll simply tries again: re-init only when the
           chip keeps refusing */
        TLOG(E, "status read failed %08x (ISR %04x, lines %u), %u in a row, last interrupt %u ticks ago, finger %s%s",
             (ULONG)rs, gLastIsr & 0xFFFF, gLastIbmr & 3, E->ReadFailRun + 1, E->Tick - E->LastAlive,
             E->Down ? "down" : "up", E->ReadFailRun + 1 >= 3 ? " - re-init" : "");
        if (++E->ReadFailRun < 3) return;
        E->ReadFailRun = 0;
        gLostTick = E->Tick;
        gReadFails++;
        gWatchdog = (gWatchdog & 0xFFFF8000) | ((gWatchdog + 1) & 0x7FFF);
        ReleaseButton(E);
        E->Initialized = FALSE;                             /* chip lost: re-initialise */
        E->InitTries = 0;
        return;
    }
    }
    E->ReadFailRun = 0;
    status = (USHORT)(raw[0] | (raw[1] << 8));
    if (intLow || status != 0) E->LastAlive = E->Tick;      /* an interrupt (touch or heartbeat) */
    gRaw4 = ((ULONG)raw[0] << 24) | ((ULONG)raw[1] << 16) | ((ULONG)raw[2] << 8) | raw[3];
    if (status == 0) {
        /* INT low with an empty status is the periodic interrupt (vendor: "maybe periodical repeated
           int"): this chip does send the heartbeat, so the watchdog may rely on it. */
        if (intLow) { E->HbSeen = TRUE; gWatchdog |= 0x8000; }
        /* The vendor interrupt handler always ends with CLEAR_INT, also when the status is 0. The chip
           keeps INT asserted (and stops reporting) until it is cleared, so clear whenever INT is low. */
        if (intLow) ZCmd(E, Z_CLEAR_INT);
        return;
    }
    gNonZero++;
    gLastStatus = status;
    gLastSub = raw[4 + 5];
    if (status == 0xFFFF || (status & BIT_MUST_ZERO)) { ZCmd(E, Z_CLEAR_INT); return; }
    if (status & BIT_ICON_EVENT) {                          /* touch keys: read (and ignore) like the vendor driver */
        USHORT icon;
        ZReadU16(E, Z_ICON_STATUS, &icon);
    }

    if (status & BIT_PT_EXIST) {
        for (i = 0; i < NFINGERS; i++) {
            const UCHAR *p = raw + 4 + i * 6;               /* x(2) y(2) width sub_status */
            LONG rx = (LONG)(p[0] | (p[1] << 8)), ry = (LONG)(p[2] | (p[3] << 8));
            if (!(p[5] & SUB_BIT_EXIST)) continue;
            if (rx > RAW_MAX_X - 1 || ry > RAW_MAX_Y - 1) { gRejected++; continue; }
            /* DT inverted-x, swapped-x-y: screen x = raw y, screen y = max - raw x; 0..65535 */
            x = ry * 65535 / (RAW_MAX_Y - 1);
            y = (RAW_MAX_X - 1 - rx) * 65535 / (RAW_MAX_X - 1);
            down = TRUE;
            break;
        }
    }
    ZCmd(E, Z_CLEAR_INT);

    if (down && !E->Down) {
        SendMouse(E, x, y, 0);                              /* move first, then press there */
        SendMouse(E, x, y, MOUSE_LEFT_BUTTON_DOWN);
    } else if (!down && E->Down) {
        SendMouse(E, x, y, MOUSE_LEFT_BUTTON_UP);
    } else if (down && (x != E->LastX || y != E->LastY)) {
        SendMouse(E, x, y, 0);
    }
    E->Down = down;
    E->LastX = x;
    E->LastY = y;
}

/* \BaseNamedObjects only exists once the session manager has run; DriverEntry of a boot-start driver
   is too early, so the poll thread keeps trying */
static VOID PowerEventOpen(VOID)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, L"\\BaseNamedObjects\\GoyaTouchPowerCycle");
    gPwrEv = IoCreateNotificationEvent(&n, &gPwrEvH);
    if (gPwrEv != NULL) { KeClearEvent(gPwrEv); DbgPrint("goyatouch: GoyaTouchPowerCycle event open\n"); }
}

/* after a failed init: count NAKs in a row and ask goyabatt for a power cycle */
static VOID PowerCycleCheck(DEV_EXT *E, NTSTATUS st, ULONG Tick)
{
    if (gPwrPending && !KeReadStateEvent(gPwrEv)) {
        gPwrPending = FALSE;
        gPwr += 0x10000;                                            /* answered */
        TLOG(E, "goyabatt power-cycled the supply (%u)", (gPwr >> 16) & 0xFF);
        E->InitTries = 0;                                           /* back to fast retries */
        E->NextInit = Tick;
    }
    if (st != STATUS_NO_SUCH_DEVICE) { gNakRun = 0; return; }
    gNakRun++;
    if (gPwrEv != NULL && !gPwrPending && gNakRun >= NAK_CYCLE &&
        (gPwrTick == 0 || Tick - gPwrTick > CYCLE_GAP_TICKS)) {
        gPwrTick = Tick ? Tick : 1;
        gPwrPending = TRUE;
        gPwr += 0x1000000;
        TLOG(E, "%u NAKs in a row: asking goyabatt for a power cycle", gNakRun);
        KeSetEvent(gPwrEv, IO_NO_INCREMENT, FALSE);
    }
}

static VOID PollStep(DEV_EXT *E, ULONG Tick)
{
    E->Tick = Tick;
    if (gPwrEv == NULL && (LONG)(Tick - gNextEvOpen) >= 0) { gNextEvOpen = Tick + 5000 / POLL_MS; PowerEventOpen(); }
    if (gPwrPending && !KeReadStateEvent(gPwrEv)) PowerCycleCheck(E, STATUS_SUCCESS, Tick);
    if (!E->Initialized) {
        /* every 0.5 s for the first 20 tries, then every 5 s (a missing chip costs CPU: polled I2C) */
        if ((LONG)(Tick - E->NextInit) >= 0) {
            NTSTATUS st;
            E->NextInit = Tick + (E->InitTries < 20 ? 500 : 5000) / POLL_MS;
            E->InitTries++;
            st = ZInitChip(E);
            gInit = (ULONG)st;
            if (!NT_SUCCESS(st)) {
                if (gLostRun == 0 || gLostRun % 10 == 0)
                    TLOG(E, "init try %u failed %08x (ISR %04x, lines %u)", E->InitTries, (ULONG)st, gLastIsr & 0xFFFF, gLastIbmr & 3);
                gLostRun++;
                /* a stuck unit or bus (timeout / bus error, not a plain NAK): reset the unit through the
                   APBC register and clock the bus free before the next try */
                if (st == STATUS_IO_TIMEOUT || st == STATUS_IO_DEVICE_ERROR) {
                    I2cHardReset(E);
                    I2cBusRecover();
                    I2cReset(E);
                }
                /* slot 1 until the chip answers: bus lines (SDA bit 28, SCL bit 29), APBC TWSI1 clock
                   register (bits 23:16), I2C status register at the failure (bits 15:0) */
                gInfo = ((gLastIbmr & 3) << 28) | ((E->Clock ? (READ_REGISTER_ULONG(E->Clock) & 0xFF) : 0xEE) << 16) | (gLastIsr & 0xFFFF);
                PowerCycleCheck(E, st, Tick);
            } else {
                gNakRun = 0;
            }
            E->Initialized = (BOOLEAN)NT_SUCCESS(st);
            if (E->Initialized) {
                TLOG(E, "chip answering (init try %u, %u failed, %u ticks after it was lost)", E->InitTries, gLostRun, Tick - gLostTick);
                gLostRun = 0;
            }
            if (E->Initialized) { E->LastAlive = E->LastProbe = Tick; E->InitTries = 0; E->HbSeen = FALSE; gWatchdog &= ~0x8000UL; }
        }
        return;
    }
    /* ESD watchdog. The vendor driver power-cycles the chip after 3 s without an interrupt; we can only
       reset it over I2C, and a reset recalibrates the panel: a finger on the screen at that moment becomes
       part of the baseline and touches are ignored until the chip's drift compensation catches up (up to
       a minute). Some chips/firmware also never send the heartbeat, which used to mean a reset every 3 s.
       So after 3 s of silence first ask the chip: INT_ENABLE still holding our mask means it kept its
       settings (an ESD reset clears them). Re-initialise only if that read fails or differs, or if a chip
       that did send heartbeats has gone 10 s without one while no finger is down. */
    if (Tick - E->LastAlive > WD_TICKS && Tick - E->LastProbe > WD_TICKS) {
        USHORT ie = 0;
        BOOLEAN ok = (BOOLEAN)(NT_SUCCESS(ZReadU16(E, Z_INT_ENABLE, &ie)) && ie == Z_INT_MASK);
        E->LastProbe = Tick;
        if (ok && !(E->HbSeen && !E->Down && Tick - E->LastAlive > WD_LOCK_TICKS)) {
            gWatchdog = (gWatchdog & 0xFF00FFFF) | ((gWatchdog + 0x10000) & 0x00FF0000);   /* probe passed */
        } else {
            TLOG(E, "watchdog: INT_ENABLE %04x (%s), heartbeat %s, last interrupt %u ticks ago - re-init",
                 ie, ok ? "kept" : "lost/failed", E->HbSeen ? "seen" : "never", Tick - E->LastAlive);
            gLostTick = Tick;
            gWatchdog = (gWatchdog & 0x00FFFFFF) | ((gWatchdog + 0x1000000) & 0xFF000000); /* re-init */
            ReleaseButton(E);
            E->Initialized = FALSE;
            E->InitTries = 0;
            return;
        }
    }
    PollOnce(E);
}

static VOID PollThread(PVOID Context)
{
    DEV_EXT *E = (DEV_EXT *)Context;
    LARGE_INTEGER period;
    ULONG tick = 0;
    ULONGLONG start = KeQueryInterruptTime();

    E->NextInit = 0;                                    /* the tick restarts with every thread */
    period.QuadPart = -10000LL * POLL_MS;
    while (KeWaitForSingleObject(&E->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        __try { PollStep(E, tick); }
        __except (GoyaTraceFilter(GetExceptionInformation(), 0x01)) { E->Initialized = FALSE; }
        tick = (ULONG)((KeQueryInterruptTime() - start) / (10000ULL * POLL_MS));
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- PnP / power -------------------------------------------------------------------------------- */

static NTSTATUS SyncCompletion(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Dev);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardAndWait(DEV_EXT *E, PIRP Irp)
{
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncCompletion, &ev, TRUE, TRUE, TRUE);
    if (IoCallDriver(E->Next, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    }
    return Irp->IoStatus.Status;
}

static VOID StopDevice(DEV_EXT *E)
{
    if (E->Thread != NULL) {
        KeSetEvent(&E->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(E->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(E->Thread);
        E->Thread = NULL;
    }
    if (E->Twsi != NULL) { MmUnmapIoSpace(E->Twsi, TWSI1_LEN); E->Twsi = NULL; }
    gClk = NULL;
    if (E->Clock != NULL) { MmUnmapIoSpace(E->Clock, sizeof(ULONG)); E->Clock = NULL; }
    E->Initialized = FALSE;
    E->Started = FALSE;
}

/* Route GPIO87/88 to TWSI1 (normally goyapinmux does it; it is left out of WinPE cards). Only these two
   pins are touched. Slot 1 shows the previous MFPR values until the chip answers. */
static VOID SetTwsi1Pins(VOID)
{
    PHYSICAL_ADDRESS pa;
    PULONG m;
    ULONG a, b;
    pa.QuadPart = MFPR_TWSI1_PHYS;
    m = (PULONG)MmMapIoSpace(pa, 8, MmNonCached);
    if (m == NULL) return;
    a = READ_REGISTER_ULONG(&m[0]);
    b = READ_REGISTER_ULONG(&m[1]);
    gInfo = (a << 16) | (b & 0xFFFF);
    if ((a & MFPR_TWSI1_MASK) != MFPR_TWSI1_VAL) { WRITE_REGISTER_ULONG(&m[0], (a & ~MFPR_TWSI1_MASK) | MFPR_TWSI1_VAL); a = READ_REGISTER_ULONG(&m[0]); }
    if ((b & MFPR_TWSI1_MASK) != MFPR_TWSI1_VAL) { WRITE_REGISTER_ULONG(&m[1], (b & ~MFPR_TWSI1_MASK) | MFPR_TWSI1_VAL); b = READ_REGISTER_ULONG(&m[1]); }
    MmUnmapIoSpace(m, 8);
}

/* TSP_INT (GPIO94): GPIO function (AF0, as the DT touch_pins) and input, so the chip can drive it */
static VOID SetIntPin(VOID)
{
    PHYSICAL_ADDRESS pa;
    PULONG m;
    if (gGpio == NULL) {
        pa.QuadPart = GPIO_PHYS;
        gGpio = (PUCHAR)MmMapIoSpace(pa, 0x200, MmNonCached);
    }
    pa.QuadPart = MFPR_GPIO94_PHYS;
    m = (PULONG)MmMapIoSpace(pa, 4, MmNonCached);
    if (m != NULL) {
        ULONG v = READ_REGISTER_ULONG(m);
        gIntInfo = v << 16;                                  /* MFPR as found */
        /* AF0 (GPIO) + pull-up (PULL_SEL bit 15, PULLUP_EN bit 14, no pull-down bit 13): the line is
           active low; without a pull it can read low all the time */
        WRITE_REGISTER_ULONG(m, (v & ~(7UL | 0xE000UL)) | 0xC000UL);
        MmUnmapIoSpace(m, 4);
    }
    if (gGpio != NULL) WRITE_REGISTER_ULONG((PULONG)(gGpio + GPIO94_BANK + GPIO_GCDR), GPIO94_BIT);
}

static NTSTATUS StartDevice(DEV_EXT *E)
{
    PHYSICAL_ADDRESS pa;
    HANDLE h;
    NTSTATUS st;

    GT_GUARD(0x03, SetTwsi1Pins(), (VOID)0);
    GT_GUARD(0x04, SetIntPin(), (VOID)0);

    pa.QuadPart = TWSI1_PHYS;
    E->Twsi = (PUCHAR)MmMapIoSpace(pa, TWSI1_LEN, MmNonCached);
    pa.QuadPart = APBC_TWSI1_PHYS;
    E->Clock = (PULONG)MmMapIoSpace(pa, sizeof(ULONG), MmNonCached);
    gClk = E->Clock;
    if (E->Twsi == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&E->StopEvent, NotificationEvent, FALSE);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, E);
    if (!NT_SUCCESS(st)) return st;
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&E->Thread, NULL);
    ZwClose(h);
    E->Started = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    DEV_EXT *E = GetExt(Fdo);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT next = E->Next;
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = ForwardAndWait(E, Irp);
        if (NT_SUCCESS(st)) {
            st = StartDevice(E);
            if (!NT_SUCCESS(st)) StopDevice(E);
        }
        gStage = NT_SUCCESS(st) ? (E->Connected ? 0x11 : 0x10) : 0x1F;
        Irp->IoStatus.Status = st;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        StopDevice(E);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        StopDevice(E);
        E->Removed = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        st = IoCallDriver(next, Irp);
        IoDetachDevice(next);
        IoDeleteDevice(Fdo);
        return st;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(next, Irp);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(GetExt(Fdo)->Next, Irp);
}

static NTSTATUS DispatchPassDown(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(GetExt(Fdo)->Next, Irp);
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Fdo);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ---- requests from mouclass (the class driver above us) ------------------------------------------ */

static NTSTATUS DispatchInternalIoctl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
    DEV_EXT *E = GetExt(Fdo);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = STATUS_SUCCESS;
    KIRQL irql;

    Irp->IoStatus.Information = 0;
    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_INTERNAL_MOUSE_CONNECT: {
        PCONNECT_DATA cd = (PCONNECT_DATA)sp->Parameters.DeviceIoControl.Type3InputBuffer;
        if (cd == NULL || sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        KeAcquireSpinLock(&E->Lock, &irql);
        if (E->Connected) {
            st = STATUS_SHARING_VIOLATION;
        } else {
            E->Connect = *cd;
            E->Connected = TRUE;
        }
        KeReleaseSpinLock(&E->Lock, irql);
        if (NT_SUCCESS(st) && gStage == 0x10) gStage = 0x11;
        break;
    }
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        KeAcquireSpinLock(&E->Lock, &irql);
        E->Connected = FALSE;
        RtlZeroMemory(&E->Connect, sizeof(E->Connect));
        KeReleaseSpinLock(&E->Lock, irql);
        break;
    case IOCTL_INTERNAL_MOUSE_ENABLE:
    case IOCTL_INTERNAL_MOUSE_DISABLE:
        break;
    case IOCTL_MOUSE_QUERY_ATTRIBUTES: {
        PMOUSE_ATTRIBUTES a = (PMOUSE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
        if (a == NULL || sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_ATTRIBUTES)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(a, sizeof(*a));
        a->MouseIdentifier = MOUSE_HID_HARDWARE;
        a->NumberOfButtons = 1;
        a->SampleRate = 1000 / POLL_MS;
        a->InputDataQueueLength = 64;
        Irp->IoStatus.Information = sizeof(MOUSE_ATTRIBUTES);
        break;
    }
    default:
        IoSkipCurrentIrpStackLocation(Irp);              /* not ours: let the bus driver answer */
        return IoCallDriver(E->Next, Irp);
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    DEV_EXT *E;
    NTSTATUS st;

    st = IoCreateDevice(Driver, sizeof(DEV_EXT), NULL, FILE_DEVICE_MOUSE, FILE_DEVICE_SECURE_OPEN, FALSE, &fdo);
    if (!NT_SUCCESS(st)) { gStage = 0x0E; gInit = (ULONG)st; return st; }
    E = GetExt(fdo);
    RtlZeroMemory(E, sizeof(*E));
    E->Self = fdo;
    KeInitializeSpinLock(&E->Lock);
    E->Next = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (E->Next == NULL) { IoDeleteDevice(fdo); gStage = 0x0F; return STATUS_NO_SUCH_DEVICE; }
    fdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    gStage = 0x02;
    return STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    if (gHbThread != NULL) {
        KeSetEvent(&gHbStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(gHbThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(gHbThread);
        gHbThread = NULL;
    }
    if (gPwrEvH != NULL) { ZwClose(gPwrEvH); gPwrEvH = NULL; gPwrEv = NULL; }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    HANDLE h;
    ULONG i;
    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) Driver->MajorFunction[i] = DispatchPassDown;
    Driver->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternalIoctl;
    Driver->DriverExtension->AddDevice = AddDevice;
    Driver->DriverUnload = DriverUnload;

    gStage = 0x00;
    KeInitializeEvent(&gHbStop, NotificationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, HeartbeatThread, NULL))) {
        ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&gHbThread, NULL);
        ZwClose(h);
    }
    return STATUS_SUCCESS;
}
