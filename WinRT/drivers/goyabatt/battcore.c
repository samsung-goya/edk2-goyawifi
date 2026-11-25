/* Battery gauge and charger control for the Galaxy Tab 3 Lite (SM-T110), shared by goyabatt.sys and the
   host simulator tools/battsim (like goyasdhc's sdcore.c, this file is #included by both).

   Hardware (pxa986-samsung-goyawifi.dts, checked against register dumps taken in TWRP):
     88PM822 PMIC on TWSI2 (0xD4037000), GPADC page 0x32: VBAT (0x42 now, 0xA0 averaged, 12 bit,
       5.6 V full scale) and GPADC2 (0x58, 1.4 V full scale) = battery thermistor, biased by a current
       source of (code * 5 + 1) uA, code in GPADC register 0x0D bits 3:0.
       There is no coulomb counter: the charge level is estimated from the voltage.
     SMB328 charger, address 0x34, on a bit-banged bus (GPIO16 = SCL, GPIO17 = SDA); CHG_EN = GPIO98,
       active low. Its watchdog stops charging when nobody talks to it, so it is polled every 2 s.
     RT8973A MUIC, address 0x14, on a second bit-banged bus (GPIO49 = SCL, GPIO50 = SDA): DEV1 says what
       is plugged in (bit 2 USB host port, bit 5 charging host port, bit 6 dedicated charger).

   The register sequences for the charger follow the firmware's GoyaWifiChargerDxe (500 mA input and
   fast charge, 4.32 V float, 200 mA termination); the TWSI sequences are the firmware's Pm822Lib ones.
   The voltage-to-charge curve is a coarse 5 % sampling of the vendor's 88PM822 open-circuit curve; the
   thermistor table comes from a beta fit of the vendor's NTC curve (22 kOhm at 25 C).

   The including file defines, for a BC_HOST *c:
     BC_GPIO_RD(c, off) BC_GPIO_WR(c, off, v)   GPIO block (0xD4019000), offsets < 0x200
     BC_MFPR_RD(c, off) BC_MFPR_WR(c, off, v)   pin mux block (0xD401E000), offsets < 0x300
     BC_TW_RD(c, off)   BC_TW_WR(c, off, v)     TWSI2 registers
     BC_APBC_RD(c)      BC_APBC_WR(c, v)        APBCP_TWSI2 clock/reset register (0xD403B028)
     BC_UDELAY(us) BC_MSLEEP(c, ms) BC_NOW_MS(c) BC_LOG(...)
     BC_LOCK(c) BC_UNLOCK(c)                    keep one bus transaction from being preempted
   and the BC_HOST fields named BC_HOST_OS_FIELDS. */

#define BC_ERR_TIMEOUT   (-1)
#define BC_ERR_NAK       (-2)
#define BC_ERR_VERIFY    (-3)
#define BC_ERR_RANGE     (-4)

/* ---- GPIO ---- */
#define GPLR 0x00
#define GPDR 0x0C
#define GPSR 0x18
#define GPCR 0x24
#define GSDR 0x54                     /* direction set (MMP layout) */
#define GCDR 0x60                     /* direction clear */

#define PIN_CHG_SCL   16
#define PIN_CHG_SDA   17
#define PIN_CHG_EN    98
#define PIN_MUIC_SCL  49
#define PIN_MUIC_SDA  50

#define BC_HALF_US    5               /* 100 kHz, as the firmware and the Linux i2c-gpio node */

/* ---- devices ---- */
#define SMB_ADDR      0x34
#define SMB_CHARGE_CURRENT  0x00
#define SMB_INPUT_LIMIT     0x01
#define SMB_FLOAT_VOLTAGE   0x02
#define SMB_FUNC_CTRL_A     0x03
#define SMB_FUNC_CTRL_B     0x04
#define SMB_FUNC_CTRL_C     0x05
#define SMB_OTG_LDO_CTRL    0x06
#define SMB_VARIOUS_FUNCS   0x07
#define SMB_IRQ_CLEAR       0x30
#define SMB_COMMAND         0x31
#define SMB_STATUS_C        0x36
#define SMB_IRQ_STATUS_C    0x37
#define CMD_ALLOW_WRITE     0x80
#define CMD_CHARGE_DISABLE  0x10
#define CMD_AC_MODE         0x08
#define CMD_USB_MODE        0x04
#define STATC_ENABLED       0x01
#define STATC_CHARGING      0x06      /* 01 = pre-charge (trickle), 10/11 = fast */
#define STATC_ERROR         0x30
#define STATC_TERM          0xC0
#define IRQC_WATCHDOG       0x80
#define IRQC_FAULT          0x06
#define SMB_FLOAT_MV        4320

#define MUIC_ADDR     0x14
#define MUIC_ADC      0x07
#define MUIC_DEV1     0x0A
#define DEV1_SDP      0x04
#define DEV1_CDP      0x20
#define DEV1_DCP      0x40

#define PM_BASE_PAGE  0x30
#define PM_GPADC_PAGE 0x32
#define GP_MEAS_EN1   0x01            /* bit 2 VBAT */
#define GP_MEAS_EN2   0x02            /* bit 4 GPADC2 */
#define GP_MISC2      0x06            /* bits 0-1 GPADC state machine on */
#define GP_BIAS       0x0D            /* bits 3:0 thermistor bias code */
#define GP_BIAS_ENA1  0x14            /* 0x44: GPADC2 bias on */
#define GP_VBAT       0x42
#define GP_VBAT_AVG   0xA0
#define GP_GPADC2     0x58

/* ---- TWSI ---- */
#define TW_IDBR  0x08
#define TW_ICR   0x10
#define TW_ISR   0x18
#define ICR_START   0x0001
#define ICR_STOP    0x0002
#define ICR_ACKNAK  0x0004
#define ICR_TB      0x0008
#define ICR_SCLE    0x0020
#define ICR_IUE     0x0040
#define ISR_ACKNAK  0x0002
#define ISR_ITE     0x0040
#define ISR_IRF     0x0080
#define APBC_APBCLK 0x1
#define APBC_FNCLK  0x2
#define APBC_RST    0x4

/* ---- policy ---- */
#define BC_POLL_MS          2000
#define BC_TEMP_EVERY_MS    10000
#define BC_SAFETY_MS        (6UL * 60 * 60 * 1000)   /* one charging session at most 6 h */
#define BC_HOT_DECI         450
#define BC_HOT_RECOV_DECI   420
#define BC_COLD_DECI        0
#define BC_COLD_RECOV_DECI  30
#define BC_OVP_MV           4400
#define BC_DESIGN_MWH       13680                    /* 3600 mAh * 3.8 V (EB-BT111ABE) */
#define BC_TEMP_UNKNOWN     (-10000)

enum { BC_CABLE_NONE, BC_CABLE_SDP, BC_CABLE_CDP, BC_CABLE_DCP, BC_CABLE_UNKNOWN };
static const char *const gBcCableName[] = { "none", "USB port", "charging port", "charger", "unknown" };

typedef struct _BC_BUS { UCHAR Scl, Sda; const char *Name; } BC_BUS;
static const BC_BUS gChgBus  = { PIN_CHG_SCL,  PIN_CHG_SDA,  "charger" };
static const BC_BUS gMuicBus = { PIN_MUIC_SCL, PIN_MUIC_SDA, "MUIC" };

typedef struct _BC_HOST {
    BC_HOST_OS_FIELDS
    BOOLEAN HasGsdr;              /* GPIO direction set/clear registers work */
    /* measurements */
    BOOLEAN VbatOk;
    ULONG   VbatMv;
    BOOLEAN TempOk;
    LONG    TempDeci;             /* 0.1 C */
    UCHAR   Bias;                 /* thermistor bias code in use */
    ULONG   LastTempMs;
    /* cable and charger */
    BOOLEAN MuicOk;
    ULONG   Cable;
    UCHAR   Dev1, Adc;
    BOOLEAN ChgOk;                /* last charger transaction worked */
    BOOLEAN Configured;
    BOOLEAN ChargeOn;             /* we enabled charging */
    BOOLEAN Hot, Cold, Expired;
    BOOLEAN Online, Charging, Full;
    UCHAR   StatC, IrqC;
    ULONG   SessionStartMs;
    BOOLEAN WasCable;
    /* gauge, 0.01 % units */
    BOOLEAN Seeded;
    LONG    Soc;
    LONG    RawFilt;
    ULONG   LastGaugeMs;
    /* counters */
    ULONG   Polls, BusErrors, Watchdogs, Configs, Recoveries;
    ULONG   ErrLogged;
} BC_HOST;

static VOID BcErr(BC_HOST *c, const char *what, int rc)
{
    c->BusErrors++;
    if (c->ErrLogged < 20) {
        c->ErrLogged++;
        BC_LOG("goyabatt: %s failed (%d), %u bus errors so far\n", what, rc, c->BusErrors);
    }
}

/* ================= bit-banged I2C (open drain: output level preset to 0, the direction does the work) ================= */

static ULONG BcBank(ULONG pin)
{
    ULONG b = pin >> 5;
    return b < 3 ? b * 4 : 0x100 + (b - 3) * 4;
}
#define BCBIT(pin) (1UL << ((pin) & 31))

static VOID BcDir(BC_HOST *c, ULONG pin, BOOLEAN out)
{
    ULONG bank = BcBank(pin), bit = BCBIT(pin), d;
    if (c->HasGsdr) {
        BC_GPIO_WR(c, bank + (out ? GSDR : GCDR), bit);
    } else {
        d = BC_GPIO_RD(c, bank + GPDR);
        BC_GPIO_WR(c, bank + GPDR, out ? (d | bit) : (d & ~bit));
    }
}
static BOOLEAN BcLevel(BC_HOST *c, ULONG pin) { return (BC_GPIO_RD(c, BcBank(pin) + GPLR) & BCBIT(pin)) != 0; }
#define BcLow(c, pin)      BcDir(c, pin, TRUE)
#define BcRelease(c, pin)  BcDir(c, pin, FALSE)

/* release SCL and wait for it to go high (a slave may stretch the clock), up to 1 ms */
static BOOLEAN BcSclHigh(BC_HOST *c, const BC_BUS *b)
{
    ULONG i;
    BcRelease(c, b->Scl);
    for (i = 0; i < 200; i++) {
        if (BcLevel(c, b->Scl)) return TRUE;
        BC_UDELAY(BC_HALF_US);
    }
    return FALSE;
}

static VOID BcStart(BC_HOST *c, const BC_BUS *b)
{
    BcRelease(c, b->Sda);
    BcSclHigh(c, b);
    BC_UDELAY(BC_HALF_US);
    BcLow(c, b->Sda);
    BC_UDELAY(BC_HALF_US);
    BcLow(c, b->Scl);
    BC_UDELAY(BC_HALF_US);
}

static VOID BcStop(BC_HOST *c, const BC_BUS *b)
{
    BcLow(c, b->Sda);
    BC_UDELAY(BC_HALF_US);
    BcSclHigh(c, b);
    BC_UDELAY(BC_HALF_US);
    BcRelease(c, b->Sda);
    BC_UDELAY(BC_HALF_US);
}

static int BcWriteByte(BC_HOST *c, const BC_BUS *b, UCHAR v)
{
    ULONG i;
    BOOLEAN ack;
    for (i = 0; i < 8; i++) {
        if (v & 0x80) BcRelease(c, b->Sda); else BcLow(c, b->Sda);
        v = (UCHAR)(v << 1);
        BC_UDELAY(BC_HALF_US);
        if (!BcSclHigh(c, b)) return BC_ERR_TIMEOUT;
        BC_UDELAY(BC_HALF_US);
        BcLow(c, b->Scl);
    }
    BcRelease(c, b->Sda);
    BC_UDELAY(BC_HALF_US);
    if (!BcSclHigh(c, b)) return BC_ERR_TIMEOUT;
    BC_UDELAY(BC_HALF_US);
    ack = !BcLevel(c, b->Sda);
    BcLow(c, b->Scl);
    return ack ? 0 : BC_ERR_NAK;
}

static int BcReadByte(BC_HOST *c, const BC_BUS *b, BOOLEAN nak, UCHAR *v)
{
    ULONG i;
    UCHAR x = 0;
    BcRelease(c, b->Sda);
    for (i = 0; i < 8; i++) {
        BC_UDELAY(BC_HALF_US);
        if (!BcSclHigh(c, b)) return BC_ERR_TIMEOUT;
        BC_UDELAY(BC_HALF_US);
        x = (UCHAR)((x << 1) | (BcLevel(c, b->Sda) ? 1 : 0));
        BcLow(c, b->Scl);
    }
    if (nak) BcRelease(c, b->Sda); else BcLow(c, b->Sda);
    BC_UDELAY(BC_HALF_US);
    if (!BcSclHigh(c, b)) return BC_ERR_TIMEOUT;
    BC_UDELAY(BC_HALF_US);
    BcLow(c, b->Scl);
    BcRelease(c, b->Sda);
    *v = x;
    return 0;
}

/* a slave stuck in the middle of a byte holds SDA low: clock it out, then STOP */
static VOID BcRecover(BC_HOST *c, const BC_BUS *b)
{
    ULONG i;
    c->Recoveries++;
    BcRelease(c, b->Sda);
    for (i = 0; i < 9 && !BcLevel(c, b->Sda); i++) {
        BcLow(c, b->Scl);
        BC_UDELAY(BC_HALF_US);
        BcSclHigh(c, b);
        BC_UDELAY(BC_HALF_US);
    }
    BcLow(c, b->Scl);
    BC_UDELAY(BC_HALF_US);
    BcStop(c, b);
}

static int BcSmbReadOnce(BC_HOST *c, const BC_BUS *b, UCHAR addr, UCHAR reg, UCHAR *v)
{
    int rc;
    BC_LOCK(c);
    BcStart(c, b);
    rc = BcWriteByte(c, b, (UCHAR)(addr << 1));
    if (rc == 0) rc = BcWriteByte(c, b, reg);
    if (rc == 0) {
        BcStart(c, b);                                     /* repeated start */
        rc = BcWriteByte(c, b, (UCHAR)((addr << 1) | 1));
        if (rc == 0) rc = BcReadByte(c, b, TRUE, v);
    }
    BcStop(c, b);
    BC_UNLOCK(c);
    return rc;
}

static int BcSmbWriteOnce(BC_HOST *c, const BC_BUS *b, UCHAR addr, UCHAR reg, UCHAR v)
{
    int rc;
    BC_LOCK(c);
    BcStart(c, b);
    rc = BcWriteByte(c, b, (UCHAR)(addr << 1));
    if (rc == 0) rc = BcWriteByte(c, b, reg);
    if (rc == 0) rc = BcWriteByte(c, b, v);
    BcStop(c, b);
    BC_UNLOCK(c);
    return rc;
}

/* one retry after a bus recovery */
static int BcSmbRead(BC_HOST *c, const BC_BUS *b, UCHAR addr, UCHAR reg, UCHAR *v)
{
    int rc = BcSmbReadOnce(c, b, addr, reg, v);
    if (rc != 0) {
        BcRecover(c, b);
        rc = BcSmbReadOnce(c, b, addr, reg, v);
    }
    return rc;
}

static int BcSmbWriteRaw(BC_HOST *c, const BC_BUS *b, UCHAR addr, UCHAR reg, UCHAR v)
{
    int rc = BcSmbWriteOnce(c, b, addr, reg, v);
    if (rc != 0) {
        BcRecover(c, b);
        rc = BcSmbWriteOnce(c, b, addr, reg, v);
    }
    return rc;
}

/* write and read back, like smb328_write() */
static int BcSmbWrite(BC_HOST *c, const BC_BUS *b, UCHAR addr, UCHAR reg, UCHAR v)
{
    UCHAR rb = 0;
    int rc = BcSmbWriteRaw(c, b, addr, reg, v);
    if (rc == 0) rc = BcSmbRead(c, b, addr, reg, &rb);
    if (rc == 0 && rb != v) rc = BC_ERR_VERIFY;
    return rc;
}

/* ================= TWSI2 (PMIC), the firmware's Pm822Lib sequences, polled ================= */

static VOID BcTwClock(BC_HOST *c)
{
    ULONG v = BC_APBC_RD(c);
    if ((v & (APBC_APBCLK | APBC_FNCLK | APBC_RST)) != (APBC_APBCLK | APBC_FNCLK)) {
        BC_LOG("goyabatt: TWSI2 clock was %08x, enabling\n", v);
        BC_APBC_WR(c, (v & ~APBC_RST) | APBC_APBCLK | APBC_FNCLK);
        BC_UDELAY(10);
    }
}

static int BcTwWait(BC_HOST *c, ULONG bit)
{
    ULONG i;
    for (i = 0; i < 1000; i++) {
        if (BC_TW_RD(c, TW_ISR) & bit) {
            BC_TW_WR(c, TW_ISR, bit);
            return 0;
        }
        BC_UDELAY(10);
    }
    return BC_ERR_TIMEOUT;
}

static int BcTwXfer(BC_HOST *c, UCHAR data, ULONG flags)
{
    BC_TW_WR(c, TW_IDBR, data);
    BC_TW_WR(c, TW_ICR, ICR_IUE | ICR_SCLE | ICR_TB | flags);
    if (BcTwWait(c, ISR_ITE) != 0) return BC_ERR_TIMEOUT;
    if (BC_TW_RD(c, TW_ISR) & ISR_ACKNAK) return BC_ERR_NAK;
    return 0;
}

static int BcPmRead(BC_HOST *c, UCHAR page, UCHAR reg, UCHAR *buf, ULONG n)
{
    int rc;
    ULONG i;
    BC_LOCK(c);
    BcTwClock(c);
    rc = BcTwXfer(c, (UCHAR)(page << 1), ICR_START);
    if (rc == 0) rc = BcTwXfer(c, reg, 0);
    if (rc == 0) rc = BcTwXfer(c, (UCHAR)((page << 1) | 1), ICR_START);
    for (i = 0; rc == 0 && i < n; i++) {
        BC_TW_WR(c, TW_ICR, ICR_IUE | ICR_SCLE | ICR_TB | (i == n - 1 ? (ICR_STOP | ICR_ACKNAK) : 0));
        rc = BcTwWait(c, ISR_IRF);
        buf[i] = (UCHAR)BC_TW_RD(c, TW_IDBR);
    }
    if (rc != 0) BC_TW_WR(c, TW_ICR, ICR_IUE | ICR_SCLE | ICR_STOP);
    BC_UNLOCK(c);
    return rc;
}

static int BcPmWrite(BC_HOST *c, UCHAR page, UCHAR reg, UCHAR v)
{
    int rc;
    BC_LOCK(c);
    BcTwClock(c);
    rc = BcTwXfer(c, (UCHAR)(page << 1), ICR_START);
    if (rc == 0) rc = BcTwXfer(c, reg, 0);
    if (rc == 0) rc = BcTwXfer(c, v, ICR_STOP);
    if (rc != 0) BC_TW_WR(c, TW_ICR, ICR_IUE | ICR_SCLE | ICR_STOP);
    BC_UNLOCK(c);
    return rc;
}

/* set the bits of mask to val if they differ; *old gets the value before */
static int BcPmUpdate(BC_HOST *c, UCHAR page, UCHAR reg, UCHAR mask, UCHAR val, UCHAR *old)
{
    UCHAR v = 0;
    int rc = BcPmRead(c, page, reg, &v, 1);
    if (old) *old = v;
    if (rc == 0 && (v & mask) != val) rc = BcPmWrite(c, page, reg, (UCHAR)((v & ~mask) | val));
    return rc;
}

/* ================= measurements ================= */

static int BcAdc12(BC_HOST *c, UCHAR reg, ULONG *raw)
{
    UCHAR b[2] = { 0, 0 };
    int rc = BcPmRead(c, PM_GPADC_PAGE, reg, b, 2);
    if (rc != 0) return rc;
    *raw = ((ULONG)b[0] << 4) | (b[1] & 0x0F);
    return (*raw == 0 || *raw == 0xFFF) ? BC_ERR_RANGE : 0;
}

static int BcReadVbat(BC_HOST *c, UCHAR reg, ULONG *mv)
{
    ULONG raw = 0;
    int rc = BcAdc12(c, reg, &raw);
    if (rc == 0) *mv = (raw * 5600 + 2048) >> 12;
    return rc;
}

/* thermistor resistance (ohm) every 5 C from -25 C to +75 C */
static const ULONG gBcNtc[] = {
    199440, 153835, 119858, 94275, 74819, 59883, 48314, 39277, 32160, 26513, 22000,
    18249, 15230, 12784, 10790, 9155, 7806, 6689, 5757, 4977, 4321
};
#define BC_NTC_FIRST_C (-25)

/* ohm -> 0.1 C, linear between the table points; FALSE outside -25..75 C */
static BOOLEAN BcNtcToDeci(ULONG ohm, LONG *deci)
{
    ULONG i, n = sizeof(gBcNtc) / sizeof(gBcNtc[0]);
    if (ohm > gBcNtc[0] || ohm < gBcNtc[n - 1]) return FALSE;
    for (i = 1; i < n - 1 && ohm < gBcNtc[i]; i++)
        ;
    /* between point i-1 (higher ohm, colder) and point i */
    *deci = (BC_NTC_FIRST_C + 5 * (LONG)(i - 1)) * 10 +
            (LONG)((gBcNtc[i - 1] - ohm) * 50 / (gBcNtc[i - 1] - gBcNtc[i]));
    return TRUE;
}

/* GPADC2 with the bias current stepped like the vendor driver until the voltage is in range */
static int BcReadTemp(BC_HOST *c, LONG *deci)
{
    static const UCHAR biases[] = { 6, 12, 3, 2, 1 };
    LONG i;
    ULONG raw, mv, ohm;
    UCHAR old, bias;
    int rc;

    /* the code that worked last time first (i = -1), then the vendor order */
    for (i = -1; i < (LONG)sizeof(biases); i++) {
        if (i < 0 && c->Bias == 0) continue;
        bias = i < 0 ? c->Bias : biases[i];
        if (i >= 0 && bias == c->Bias) continue;
        rc = BcPmUpdate(c, PM_GPADC_PAGE, GP_BIAS, 0x0F, bias, &old);
        if (rc != 0) return rc;
        if ((old & 0x0F) != bias) BC_MSLEEP(c, 50);
        raw = 0;
        rc = BcAdc12(c, GP_GPADC2, &raw);
        if (rc == BC_ERR_RANGE && bias == 1 && raw == 0xFFF) { c->Bias = bias; *deci = -250; return 0; }
        if (rc == BC_ERR_RANGE) continue;
        if (rc != 0) return rc;
        mv = (raw * 700) >> 11;
        /* out of range even with the largest / smallest bias: hotter than ~70 C / colder than -25 C */
        if (bias == 12 && mv <= 300) { c->Bias = bias; *deci = 700; return 0; }
        if (bias == 1 && mv >= 1250) { c->Bias = bias; *deci = -250; return 0; }
        if (mv <= 300 || mv >= 1250) continue;
        ohm = mv * 1000 / (bias * 5UL + 1);
        if (BcNtcToDeci(ohm, deci)) {
            c->Bias = bias;
            return 0;
        }
    }
    return BC_ERR_RANGE;
}

/* open-circuit voltage (mV) at 0, 2, 5, 10 ... 100 % */
static const USHORT gBcOcvPct[] = { 0, 2, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100 };
static const USHORT gBcOcvMv[]  = { 3373, 3545, 3670, 3694, 3719, 3742, 3759, 3778, 3794, 3809, 3828,
                                    3850, 3877, 3916, 3967, 4010, 4056, 4104, 4156, 4210, 4267, 4320 };

/* mV -> 0.01 % */
static LONG BcOcvToSoc(LONG mv)
{
    ULONG i, n = sizeof(gBcOcvMv) / sizeof(gBcOcvMv[0]);
    if (mv <= gBcOcvMv[0]) return 0;
    if (mv >= gBcOcvMv[n - 1]) return 10000;
    for (i = 1; i < n - 1 && mv >= gBcOcvMv[i]; i++)
        ;
    return gBcOcvPct[i - 1] * 100 +
           (mv - gBcOcvMv[i - 1]) * (gBcOcvPct[i] - gBcOcvPct[i - 1]) * 100 / (gBcOcvMv[i] - gBcOcvMv[i - 1]);
}

/* ================= cable ================= */

static VOID BcCable(BC_HOST *c)
{
    UCHAR dev1 = 0, adc = 0;
    int rc = BcSmbRead(c, &gMuicBus, MUIC_ADDR, MUIC_DEV1, &dev1);
    if (rc == 0) rc = BcSmbRead(c, &gMuicBus, MUIC_ADDR, MUIC_ADC, &adc);
    if (rc != 0) {
        if (c->MuicOk) BcErr(c, "MUIC read", rc);
        c->MuicOk = FALSE;
        c->Cable = BC_CABLE_UNKNOWN;
        return;
    }
    c->MuicOk = TRUE;
    c->Dev1 = dev1;
    c->Adc = (UCHAR)(adc & 0x1F);
    if (dev1 & DEV1_DCP)      c->Cable = BC_CABLE_DCP;
    else if (dev1 & DEV1_CDP) c->Cable = BC_CABLE_CDP;
    else if (dev1 & DEV1_SDP) c->Cable = BC_CABLE_SDP;
    else                      c->Cable = BC_CABLE_NONE;
}

/* ================= charger ================= */

static int BcChgConfigure(BC_HOST *c)
{
    const BC_BUS *b = &gChgBus;
    UCHAR cmd = 0, v = 0, fl = (UCHAR)((SMB_FLOAT_MV - 3460) / 20);
    int rc, rc2;

    rc = BcSmbRead(c, b, SMB_ADDR, SMB_COMMAND, &cmd);
    if (rc != 0) return rc;
    cmd = (UCHAR)((cmd | CMD_ALLOW_WRITE | CMD_CHARGE_DISABLE) & ~(CMD_AC_MODE | CMD_USB_MODE));
    rc = BcSmbWrite(c, b, SMB_ADDR, SMB_COMMAND, cmd);
    if (rc != 0) return rc;

    rc = BcSmbWrite(c, b, SMB_ADDR, SMB_FUNC_CTRL_C, 0x00);
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_FUNC_CTRL_B, 0x6E);
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_FUNC_CTRL_A, 0xDA);
    if (rc == 0) rc = BcSmbRead(c, b, SMB_ADDR, SMB_FLOAT_VOLTAGE, &v);
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_FLOAT_VOLTAGE, (UCHAR)((v & ~0x7E) | (fl << 1)));
    if (rc == 0) rc = BcSmbRead(c, b, SMB_ADDR, SMB_CHARGE_CURRENT, &v);                   /* 500 mA, 200 mA term. */
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_CHARGE_CURRENT, (UCHAR)((v & 0x18) | 0x07));
    if (rc == 0) rc = BcSmbRead(c, b, SMB_ADDR, SMB_INPUT_LIMIT, &v);                      /* 500 mA input */
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_INPUT_LIMIT, (UCHAR)(v & 0x18));
    if (rc == 0) rc = BcSmbRead(c, b, SMB_ADDR, SMB_VARIOUS_FUNCS, &v);
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_VARIOUS_FUNCS, (UCHAR)((v & 0x1F) | 0xE0));
    if (rc == 0) rc = BcSmbRead(c, b, SMB_ADDR, SMB_OTG_LDO_CTRL, &v);                     /* never source VBUS */
    if (rc == 0) rc = BcSmbWrite(c, b, SMB_ADDR, SMB_OTG_LDO_CTRL, (UCHAR)(v & ~0x20));
    if (rc == 0) cmd |= CMD_AC_MODE;

    /* close the write gate, charging still off */
    rc2 = BcSmbWrite(c, b, SMB_ADDR, SMB_COMMAND, (UCHAR)(cmd & ~CMD_ALLOW_WRITE));
    if (rc == 0) rc = rc2;
    c->ChargeOn = FALSE;
    if (rc == 0) {
        c->Configured = TRUE;
        c->Configs++;
    }
    return rc;
}

static int BcChgEnable(BC_HOST *c, BOOLEAN on)
{
    UCHAR cmd = 0, want;
    int rc = BcSmbRead(c, &gChgBus, SMB_ADDR, SMB_COMMAND, &cmd);
    if (rc != 0) return rc;
    want = (UCHAR)(cmd & ~CMD_ALLOW_WRITE);
    want = on ? (UCHAR)(want & ~CMD_CHARGE_DISABLE) : (UCHAR)(want | CMD_CHARGE_DISABLE);
    if (want != cmd) rc = BcSmbWrite(c, &gChgBus, SMB_ADDR, SMB_COMMAND, want);
    if (rc == 0) c->ChargeOn = on;
    return rc;
}

/* Charging is enabled whether or not a cable is seen, as the firmware does: without input power the
   charger does nothing, and a cable the MUIC has not identified (it can miss one plugged in before a
   cold boot) still charges. "Plugged in" = the MUIC sees a cable or the charger is working. */
static VOID BcCharger(BC_HOST *c, ULONG now)
{
    BOOLEAN cable = c->Cable == BC_CABLE_SDP || c->Cable == BC_CABLE_CDP || c->Cable == BC_CABLE_DCP;
    BOOLEAN on, wd = FALSE;
    UCHAR irq = 0, stat = 0;
    int rc = 0;

    if (cable && !c->WasCable) {
        /* plugged in: start a clean session (clears latched faults and the safety timer) */
        c->SessionStartMs = now;
        c->Expired = FALSE;
        c->Configured = FALSE;
        rc = BcSmbWriteRaw(c, &gChgBus, SMB_ADDR, SMB_IRQ_CLEAR, 0xFF);
    }
    if (!cable && !c->Online) {
        c->Expired = FALSE;
        c->SessionStartMs = now;
    }
    c->WasCable = cable;

    if (rc == 0 && !c->Configured) rc = BcChgConfigure(c);
    if (rc == 0) rc = BcSmbRead(c, &gChgBus, SMB_ADDR, SMB_IRQ_STATUS_C, &irq);
    if (rc == 0 && (irq & IRQC_WATCHDOG)) {
        /* nobody talked to it for too long (also after the firmware hands over): program it again */
        wd = TRUE;
        c->Watchdogs++;
        rc = BcSmbWriteRaw(c, &gChgBus, SMB_ADDR, SMB_IRQ_CLEAR, 0xFF);
        if (rc == 0) rc = BcChgConfigure(c);
        if (rc == 0) rc = BcSmbRead(c, &gChgBus, SMB_ADDR, SMB_IRQ_STATUS_C, &irq);
    }

    if (c->TempOk) {
        if (c->TempDeci >= BC_HOT_DECI) c->Hot = TRUE;
        else if (c->TempDeci <= BC_HOT_RECOV_DECI) c->Hot = FALSE;
        if (c->TempDeci <= BC_COLD_DECI) c->Cold = TRUE;
        else if (c->TempDeci >= BC_COLD_RECOV_DECI) c->Cold = FALSE;
    }
    if (c->Online && now - c->SessionStartMs > BC_SAFETY_MS) c->Expired = TRUE;

    on = !c->Hot && !c->Cold && !c->Expired && !(irq & IRQC_FAULT) && !(c->VbatOk && c->VbatMv >= BC_OVP_MV);
    if (rc == 0) rc = BcChgEnable(c, on);
    if (rc == 0) rc = BcSmbRead(c, &gChgBus, SMB_ADDR, SMB_STATUS_C, &stat);

    if (rc != 0) {
        BcErr(c, "charger", rc);
        c->ChgOk = FALSE;
        c->Configured = FALSE;
        c->Charging = c->Full = FALSE;
        c->Online = cable;
        return;
    }
    if (!c->ChgOk || wd) BC_LOG("goyabatt: charger %s (IRQ_C %02x STATUS_C %02x)\n", wd ? "watchdog had expired, programmed again" : "programmed", irq, stat);
    c->ChgOk = TRUE;
    c->IrqC = irq;
    c->StatC = stat;
    c->Full = on && (stat & STATC_TERM) != 0;
    c->Charging = on && !c->Full && (stat & STATC_ENABLED) && !(stat & STATC_ERROR) && (stat & STATC_CHARGING);
    c->Online = cable || c->Charging || c->Full;
}

/* ================= gauge ================= */

static VOID BcGauge(BC_HOST *c, ULONG now)
{
    LONG ocv, raw, diff, step;
    ULONG dt;

    /* the first polls reprogram the charger: wait for the voltage to settle before seeding */
    if (!c->VbatOk || (!c->Seeded && c->Polls < 3)) return;
    /* rough correction for the voltage drop over the internal resistance: the charger's 500 mA minus
       what the tablet uses (TWRP: 4.046 V while charging at 71 % = about 25 mV above the curve) */
    if (c->Charging) ocv = (LONG)c->VbatMv - (((c->StatC & STATC_CHARGING) == 0x02) ? 10 : 25);
    else if (c->Online) ocv = (LONG)c->VbatMv;
    else ocv = (LONG)c->VbatMv + 25;
    raw = BcOcvToSoc(ocv);

    if (!c->Seeded) {
        c->Seeded = TRUE;
        c->Soc = c->RawFilt = raw;
        c->LastGaugeMs = now;
        return;
    }
    c->RawFilt += (raw - c->RawFilt) / 8;                       /* ~16 s low-pass */
    dt = now - c->LastGaugeMs;
    c->LastGaugeMs = now;
    if (dt > 10000) dt = 10000;
    step = (LONG)(dt / 600);                                    /* 1 % per minute */
    if (c->Full) step *= 5;
    if (!c->Online && c->VbatMv < 3450) step *= 10;             /* nearly empty: follow the voltage */

    diff = (c->Full ? 10000 : c->RawFilt) - c->Soc;
    if (!c->Full && diff > -50 && diff < 50) diff = 0;          /* dead band against noise */
    if ((c->Charging || c->Full) && diff < 0) diff = 0;         /* never down while charging */
    if (!c->Online && diff > 0) diff = 0;                       /* never up on battery */
    if (c->Charging && !c->Full && diff > 0 && c->Soc + diff > 9900) diff = c->Soc < 9900 ? 9900 - c->Soc : 0;   /* 100 % only when full */
    if (diff > step) diff = step;
    if (diff < -step) diff = -step;
    c->Soc += diff;
    if (c->Soc < 0) c->Soc = 0;
    if (c->Soc > 10000) c->Soc = 10000;
}

static ULONG BcPercent(const BC_HOST *c) { return (ULONG)((c->Soc + 50) / 100); }

/* what the battery class reports (batclass.h BATTERY_* power state flags) */
static ULONG BcPowerState(const BC_HOST *c)
{
    ULONG f = 0;
    if (c->Online) f |= BATTERY_POWER_ON_LINE;
    if (c->Charging) f |= BATTERY_CHARGING;
    else if (!c->Online) f |= BATTERY_DISCHARGING;
    if (!c->Online && BcPercent(c) <= 3) f |= BATTERY_CRITICAL;
    return f;
}

static ULONG BcCapacityMwh(const BC_HOST *c) { return (ULONG)c->Soc * BC_DESIGN_MWH / 10000; }

static VOID BcLogState(BC_HOST *c, const char *why)
{
    LONG t = c->TempOk ? c->TempDeci : 0, a = t < 0 ? -t : t;
    BC_LOG("goyabatt: %s: %u%% (%u.%02u), %u mV, %s%s%d.%d C, cable %s (DEV1 %02x), %s, STATUS_C %02x IRQ_C %02x, "
           "%u polls, %u watchdog, %u configured, %u bus errors, %u recoveries\n",
           why, BcPercent(c), (ULONG)c->Soc / 100, (ULONG)c->Soc % 100, c->VbatMv,
           c->TempOk ? "" : "no temperature ", t < 0 ? "-" : "", a / 10, a % 10, gBcCableName[c->Cable], c->Dev1,
           c->Full ? "full" : c->Charging ? "charging" : c->Online ? (c->Hot ? "too hot to charge" : c->Cold ? "too cold to charge" : c->Expired ? "charge time limit" : "not charging") : "on battery",
           c->StatC, c->IrqC, c->Polls, c->Watchdogs, c->Configs, c->BusErrors, c->Recoveries);
}

/* ================= entry points ================= */

/* pins, GPADC set-up; returns 0 if at least the PMIC answers */
static int BcInit(BC_HOST *c)
{
    static const struct { UCHAR Pin; USHORT Mfpr; } pins[] = {
        { PIN_CHG_SCL, 0x011C }, { PIN_CHG_SDA, 0x0120 }, { PIN_CHG_EN, 0x0234 },
        { PIN_MUIC_SCL, 0x01A0 }, { PIN_MUIC_SDA, 0x01A4 } };
    ULONG i, m[5], d1, d2, bank = BcBank(PIN_CHG_SCL), bit = BCBIT(PIN_CHG_SCL);
    UCHAR g[5] = { 0, 0, 0, 0, 0 };
    int rc;

    /* pins as GPIO (function 0) */
    for (i = 0; i < 5; i++) {
        m[i] = BC_MFPR_RD(c, pins[i].Mfpr);
        if (m[i] & 7) BC_MFPR_WR(c, pins[i].Mfpr, m[i] & ~7UL);
    }

    /* do the direction set/clear registers work? (an SCL pulse with SDA high is not a bus condition) */
    c->HasGsdr = FALSE;
    BcRelease(c, PIN_CHG_SCL);
    BC_GPIO_WR(c, bank + GPCR, bit);
    BC_GPIO_WR(c, bank + GSDR, bit);
    d1 = BC_GPIO_RD(c, bank + GPDR);
    BC_GPIO_WR(c, bank + GCDR, bit);
    d2 = BC_GPIO_RD(c, bank + GPDR);
    c->HasGsdr = (d1 & bit) && !(d2 & bit);
    BcRelease(c, PIN_CHG_SCL);

    /* the four bus lines: released, output level 0 for when they are driven */
    BcRelease(c, PIN_CHG_SCL);  BC_GPIO_WR(c, BcBank(PIN_CHG_SCL) + GPCR, BCBIT(PIN_CHG_SCL));
    BcRelease(c, PIN_CHG_SDA);  BC_GPIO_WR(c, BcBank(PIN_CHG_SDA) + GPCR, BCBIT(PIN_CHG_SDA));
    BcRelease(c, PIN_MUIC_SCL); BC_GPIO_WR(c, BcBank(PIN_MUIC_SCL) + GPCR, BCBIT(PIN_MUIC_SCL));
    BcRelease(c, PIN_MUIC_SDA); BC_GPIO_WR(c, BcBank(PIN_MUIC_SDA) + GPCR, BCBIT(PIN_MUIC_SDA));
    /* CHG_EN is active low: drive it low */
    BC_GPIO_WR(c, BcBank(PIN_CHG_EN) + GPCR, BCBIT(PIN_CHG_EN));
    BcLow(c, PIN_CHG_EN);

    BC_LOG("goyabatt: GPIO direction set/clear registers %s; MFPR 16 %04x 17 %04x 98 %04x 49 %04x 50 %04x; "
           "SDA levels %u %u\n", c->HasGsdr ? "work" : "missing, read-modify-write", m[0], m[1], m[2], m[3], m[4],
           BcLevel(c, PIN_CHG_SDA), BcLevel(c, PIN_MUIC_SDA));
    if (!BcLevel(c, PIN_CHG_SDA)) BcRecover(c, &gChgBus);
    if (!BcLevel(c, PIN_MUIC_SDA)) BcRecover(c, &gMuicBus);

    /* GPADC: state machine, VBAT and GPADC2 measurement, thermistor bias (normally already set) */
    rc = BcPmUpdate(c, PM_GPADC_PAGE, GP_MISC2, 0x03, 0x03, &g[0]);
    if (rc == 0) rc = BcPmUpdate(c, PM_GPADC_PAGE, GP_MEAS_EN1, 0x04, 0x04, &g[1]);
    if (rc == 0) rc = BcPmUpdate(c, PM_GPADC_PAGE, GP_MEAS_EN2, 0x10, 0x10, &g[2]);
    if (rc == 0) rc = BcPmUpdate(c, PM_GPADC_PAGE, GP_BIAS_ENA1, 0x44, 0x44, &g[3]);
    if (rc == 0) rc = BcPmRead(c, PM_GPADC_PAGE, GP_BIAS, &g[4], 1);
    BC_LOG("goyabatt: GPADC before: MISC2 %02x MEAS_EN1 %02x MEAS_EN2 %02x BIAS_ENA1 %02x BIAS %02x (%d)\n",
           g[0], g[1], g[2], g[3], g[4], rc);
    if (rc == 0) {
        c->Bias = (UCHAR)(g[4] & 0x0F);
        if ((g[1] & 0x04) == 0 || (g[0] & 3) != 3) BC_MSLEEP(c, 50);   /* first conversion */
    }
    c->Cable = BC_CABLE_UNKNOWN;
    return rc;
}

/* one poll (every BC_POLL_MS); returns TRUE when what Windows sees changed */
static BOOLEAN BcPoll(BC_HOST *c)
{
    ULONG now = BC_NOW_MS(c), mv = 0, oldPct = BcPercent(c);
    BOOLEAN oldOnline = c->Online, oldCharging = c->Charging, oldFull = c->Full, oldSeeded = c->Seeded;
    BOOLEAN oldHot = c->Hot, oldCold = c->Cold, oldMuic = c->MuicOk, oldTemp = c->TempOk;
    LONG deci = 0;
    int rc;

    c->Polls++;
    rc = BcReadVbat(c, GP_VBAT_AVG, &mv);
    if (rc == 0) {
        c->VbatOk = TRUE;
        c->VbatMv = mv;
    } else {
        if (c->VbatOk || c->Polls == 1) BcErr(c, "VBAT read", rc);
        c->VbatOk = FALSE;
    }
    if (!c->TempOk || now - c->LastTempMs >= BC_TEMP_EVERY_MS || c->Polls == 1) {
        rc = BcReadTemp(c, &deci);
        c->LastTempMs = now;
        if (rc == 0) {
            c->TempOk = TRUE;
            c->TempDeci = deci;
        } else {
            if (c->TempOk || c->Polls == 1) BcErr(c, "temperature read", rc);
            c->TempOk = FALSE;
        }
    }
    BcCable(c);
    BcCharger(c, now);
    BcGauge(c, now);

    if (c->Online != oldOnline || c->Charging != oldCharging || c->Full != oldFull || c->Hot != oldHot ||
        c->Cold != oldCold || c->MuicOk != oldMuic || c->TempOk != oldTemp || (!oldSeeded && c->Seeded))
        BcLogState(c, c->Polls == 1 ? "first reading" : "changed");
    else if (c->Polls % 150 == 0)
        BcLogState(c, "every 5 min");

    return c->Online != oldOnline || c->Charging != oldCharging || c->Full != oldFull ||
           BcPercent(c) != oldPct || (!oldSeeded && c->Seeded);
}
