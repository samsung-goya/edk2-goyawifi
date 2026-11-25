/* Host-side test of WinRT/drivers/goyabatt/battcore.c against simulated hardware:
   PXA GPIO block with two bit-banged I2C buses (pin-level slaves: SMB328 charger, RT8973A MUIC),
   the TWSI2 controller with the 88PM822 (base and GPADC pages) and a battery model.
   cc -O1 -Wall -o battsim sim.c && ./battsim                                                         */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint32_t ULONG; typedef int32_t LONG; typedef uint16_t USHORT; typedef uint8_t UCHAR; typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define VOID void
#define BATTERY_POWER_ON_LINE 0x1
#define BATTERY_DISCHARGING   0x2
#define BATTERY_CHARGING      0x4
#define BATTERY_CRITICAL      0x8

static uint64_t now_us;
static int opt_gsdr = 1, opt_log = 0, locked;

/* ================= GPIO block and the two bit-banged buses ================= */
static uint32_t g_out[4], g_dir[4];          /* per bank: output level, direction (1 = out) */
static uint32_t mfpr[0x300 / 4];

typedef struct Slave {
    const char *name;
    int scl_pin, sda_pin, addr, present;
    uint8_t (*rd)(int reg); void (*wr)(int reg, uint8_t v);
    int st, phase, bits, byte, rw, reg, regset, drive, sbyte, macked, scl, sda;
    unsigned accesses;
} Slave;
enum { S_IDLE, S_RX, S_RX_ACK, S_TX, S_TX_ACK, S_IGNORE };
enum { PH_ADDR, PH_DATA };

static Slave smb, muic;

static int bank_of(int pin) { return pin >> 5; }
static int master_low(int pin) { int b = bank_of(pin), m = 1 << (pin & 31); return (g_dir[b] & m) && !(g_out[b] & m); }
static int master_high(int pin) { int b = bank_of(pin), m = 1 << (pin & 31); return (g_dir[b] & m) && (g_out[b] & m); }
static int wire(int pin)
{
    if (master_high(pin)) return 1;
    if (master_low(pin)) return 0;
    if (pin == smb.sda_pin && smb.drive) return 0;
    if (pin == muic.sda_pin && muic.drive) return 0;
    return 1;                                  /* pull-up */
}

static void slave_drive_bit(Slave *s) { s->drive = !((s->sbyte >> (7 - s->bits)) & 1); }

static void slave_edge(Slave *s)
{
    int scl = wire(s->scl_pin), sda = wire(s->sda_pin);
    if (scl == s->scl && sda == s->sda) return;
    if (scl && s->scl && sda != s->sda) {                         /* SDA changed while SCL high */
        if (!sda) { s->st = S_RX; s->phase = PH_ADDR; s->bits = 0; s->byte = 0; s->regset = 0; s->drive = 0; }
        else { s->st = S_IDLE; s->drive = 0; }
    } else if (scl && !s->scl) {                                  /* rising */
        if (s->st == S_RX) { s->byte = (s->byte << 1) | sda; s->bits++; }
        else if (s->st == S_TX) s->bits++;
        else if (s->st == S_TX_ACK) s->macked = !sda;
    } else if (!scl && s->scl) {                                  /* falling */
        switch (s->st) {
        case S_RX:
            if (s->bits < 8) break;
            if (s->phase == PH_ADDR) {
                if ((s->byte >> 1) != s->addr || !s->present) { s->st = S_IGNORE; break; }
                s->rw = s->byte & 1; s->accesses++;
            } else if (!s->regset) { s->reg = s->byte; s->regset = 1; }
            else s->wr(s->reg++, (uint8_t)s->byte);
            s->drive = 1; s->st = S_RX_ACK;
            break;
        case S_RX_ACK:
            s->drive = 0;
            if (s->phase == PH_ADDR && s->rw) { s->sbyte = s->rd(s->reg++); s->bits = 0; s->st = S_TX; slave_drive_bit(s); }
            else { s->phase = PH_DATA; s->st = S_RX; s->bits = 0; s->byte = 0; }
            break;
        case S_TX:
            if (s->bits >= 8) { s->drive = 0; s->st = S_TX_ACK; } else slave_drive_bit(s);
            break;
        case S_TX_ACK:
            if (s->macked) { s->sbyte = s->rd(s->reg++); s->bits = 0; s->st = S_TX; slave_drive_bit(s); }
            else { s->drive = 0; s->st = S_IGNORE; }
            break;
        }
    }
    s->scl = scl; s->sda = wire(s->sda_pin);
}
static void buses(void) { slave_edge(&smb); slave_edge(&muic); slave_edge(&smb); slave_edge(&muic); }

static uint32_t gpio_rd(ULONG off)
{
    int b, reg;
    /* register offsets are multiples of 12 (0x00 0x0C 0x18 0x24 0x54 0x60): banks 0-2 at +0/4/8, bank 3 at +0x100 */
    if (off >= 0x100) { b = 3; reg = off - 0x100; } else { b = (off % 12) / 4; reg = off - b * 4; }
    switch (reg) {
    case 0x00: { uint32_t v = 0; for (int i = 0; i < 32; i++) v |= (uint32_t)wire(b * 32 + i) << i; return v; }
    case 0x0C: return g_dir[b];
    default: return 0;                           /* set/clear registers read as 0 */
    }
}
static void gpio_wr(ULONG off, uint32_t v)
{
    int b, reg;
    if (off >= 0x100) { b = 3; reg = off - 0x100; } else { b = (off % 12) / 4; reg = off - b * 4; }
    switch (reg) {
    case 0x0C: g_dir[b] = v; break;
    case 0x18: g_out[b] |= v; break;
    case 0x24: g_out[b] &= ~v; break;
    case 0x54: if (opt_gsdr) g_dir[b] |= v; break;
    case 0x60: if (opt_gsdr) g_dir[b] &= ~v; break;
    default: fprintf(stderr, "SIM: GPIO write %x\n", off); exit(2);
    }
    buses();
}

/* ================= battery and thermistor ================= */
static double soc = 0.5;                         /* true state of charge */
static double temp_c = 25.0;
static double load_a = 0.3;                      /* system load */
static double ibat;                              /* + = into the battery */
static const double R_INT = 0.15, CAP_AH = 3.6;
static double ocv(double s)
{
    static const double p[] = { 0, 2, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100 };
    static const double v[] = { 3373, 3545, 3670, 3694, 3719, 3742, 3759, 3778, 3794, 3809, 3828,
                                3850, 3877, 3916, 3967, 4010, 4056, 4104, 4156, 4210, 4267, 4320 };
    double x = s * 100; int i;
    if (x <= 0) return v[0] - 300;
    if (x >= 100) return v[21];
    for (i = 1; i < 21 && x >= p[i]; i++) ;
    return v[i - 1] + (x - p[i - 1]) * (v[i] - v[i - 1]) / (p[i] - p[i - 1]);
}
static double vbat_mv(void) { return ocv(soc) + ibat * R_INT * 1000; }
static double ntc_ohm(double t) { double B = t < 25 ? 3262 : 3379; return 22000 * exp(B * (1 / (t + 273.15) - 1 / 298.15)); }

/* ================= SMB328 ================= */
enum { CAB_NONE, CAB_SDP, CAB_CDP, CAB_DCP };
static int cable = CAB_SDP, opt_muic_latched = 1;
static uint8_t smb_r[0x40];
static int smb_wd, smb_wd_after_s = 10, smb_term;          /* watchdog: no access for this long */
static uint64_t smb_last_us;
static const uint8_t smb_defaults[8] = { 0x1F, 0x00, 0x56, 0xDA, 0x6E, 0x00, 0x5C, 0xF6 };
static int input_ok(void) { return cable != CAB_NONE && master_low(98); }
static int smb_charging(void)
{
    if (!input_ok() || smb_wd || (smb_r[0x31] & 0x10) || !(smb_r[0x31] & 0x08)) return 0;
    return 1;
}
static uint8_t smb_status_c(void)
{
    if (!smb_charging()) return 0;
    if (soc >= 0.995) smb_term = 1;              /* terminated; charges again below 97 % */
    if (soc < 0.97) smb_term = 0;
    if (smb_term) return 0xC1;
    return soc < 0.02 ? 0x03 : 0x05;
}
static void smb_touch(void) { smb_last_us = now_us; }
static uint8_t smb_rd(int reg)
{
    smb_touch(); reg &= 0x3F;
    if (reg == 0x36) return smb_status_c();
    if (reg == 0x37) return smb_r[0x37] | (smb_wd ? 0x80 : 0);
    return smb_r[reg];
}
static void smb_wr(int reg, uint8_t v)
{
    smb_touch(); reg &= 0x3F;
    if (reg <= 7) { if (smb_r[0x31] & 0x80) smb_r[reg] = v; return; }
    if (reg == 0x30) { smb_r[0x37] = 0; smb_wd = 0; return; }
    if (reg == 0x31) { smb_r[0x31] = v; return; }
}
static void smb_tick(void)
{
    if (!smb_wd && now_us - smb_last_us > (uint64_t)smb_wd_after_s * 1000000) {
        smb_wd = 1;
        memcpy(smb_r, smb_defaults, 8);
        smb_r[0x31] |= 0x10;
    }
}

/* ================= RT8973A ================= */
static uint8_t muic_rd(int reg)
{
    switch (reg) {
    case 0x01: return 0x0A;
    case 0x02: return 0xE4;
    case 0x07: return 0x1F;
    case 0x0A:
        if (!opt_muic_latched) return 0;
        return cable == CAB_SDP ? 0x04 : cable == CAB_CDP ? 0x20 : cable == CAB_DCP ? 0x40 : 0;
    default: return 0;
    }
}
static void muic_wr(int reg, uint8_t v) { (void)reg; (void)v; }

/* ================= TWSI2 + 88PM822 ================= */
static uint32_t tw_isr, tw_idbr, apbc = 0x3;
static int tw_dev = -1, tw_rw, tw_reg, tw_regset, opt_pmic_absent;
static uint8_t pm_base[256], pm_gp[256];
static unsigned pm_writes;

static uint8_t pm_rd(int dev, int reg)
{
    if (dev == 0x30) return pm_base[reg];
    if (dev != 0x32) return 0xFF;
    if (reg == 0x42 || reg == 0x43 || reg == 0xA0 || reg == 0xA1) {
        unsigned raw = (pm_gp[0x01] & 0x04) && (pm_gp[0x06] & 3) == 3 ? (unsigned)(vbat_mv() * 4096 / 5600) : 0;
        if (raw > 4095) raw = 4095;
        return (reg & 1) ? (raw & 0xF) : (uint8_t)(raw >> 4);
    }
    if (reg == 0x58 || reg == 0x59) {
        double ua = (pm_gp[0x0D] & 0xF) * 5 + 1, mv = ntc_ohm(temp_c) * ua / 1000;
        unsigned raw = (pm_gp[0x02] & 0x10) && (pm_gp[0x14] & 0x44) == 0x44 ? (unsigned)(mv * 4096 / 1400) : 0;
        if (raw > 4095) raw = 4095;
        return (reg & 1) ? (raw & 0xF) : (uint8_t)(raw >> 4);
    }
    return pm_gp[reg];
}
static void pm_wr(int dev, int reg, uint8_t v)
{
    pm_writes++;
    if (dev == 0x30) pm_base[reg] = v; else if (dev == 0x32) pm_gp[reg] = v;
    if (dev == 0x32 && reg != 0x0D && reg != 0x01 && reg != 0x02 && reg != 0x06 && reg != 0x14) {
        fprintf(stderr, "SIM: unexpected GPADC write %02x=%02x\n", reg, v); exit(2);
    }
    if (dev != 0x32) { fprintf(stderr, "SIM: unexpected PMIC write page %02x reg %02x\n", dev, reg); exit(2); }
}
static uint32_t tw_rd(ULONG off) { return off == 0x18 ? tw_isr : off == 0x08 ? tw_idbr : 0; }
static void tw_wr(ULONG off, uint32_t v)
{
    if (off == 0x18) { tw_isr &= ~v; return; }
    if (off == 0x08) { tw_idbr = v & 0xFF; return; }
    if (off != 0x10) { fprintf(stderr, "SIM: TWSI write %x\n", off); exit(2); }
    if (!(apbc & 3) || (apbc & 4)) return;       /* no clock: nothing happens (timeouts) */
    if (!(v & 0x8)) { if (v & 2) tw_dev = -1; return; }
    if (v & 1) {                                 /* START + address */
        int d = (int)(tw_idbr >> 1);
        tw_rw = tw_idbr & 1;
        if (opt_pmic_absent || (d != 0x30 && d != 0x31 && d != 0x32)) { tw_isr |= 0x42; tw_dev = -1; }
        else { tw_dev = d; tw_isr = (tw_isr & ~2u) | 0x40; if (!tw_rw) tw_regset = 0; }
    } else if (tw_dev < 0) {
        tw_isr |= 0x42;
    } else if (!tw_rw) {
        if (!tw_regset) { tw_reg = (int)tw_idbr; tw_regset = 1; } else pm_wr(tw_dev, tw_reg++, (uint8_t)tw_idbr);
        tw_isr = (tw_isr & ~2u) | 0x40;
    } else {
        tw_idbr = pm_rd(tw_dev, tw_reg++ & 0xFF);
        tw_isr |= 0x80;
    }
    if (v & 2) tw_dev = -1;
}

/* ================= glue ================= */
#define BC_HOST_OS_FIELDS int Dummy;
#define BC_GPIO_RD(c, o)      gpio_rd(o)
#define BC_GPIO_WR(c, o, v)   gpio_wr(o, v)
#define BC_MFPR_RD(c, o)      mfpr[(o) / 4]
#define BC_MFPR_WR(c, o, v)   (mfpr[(o) / 4] = (v))
#define BC_TW_RD(c, o)        tw_rd(o)
#define BC_TW_WR(c, o, v)     tw_wr(o, v)
#define BC_APBC_RD(c)         apbc
#define BC_APBC_WR(c, v)      (apbc = (v))
#define BC_UDELAY(us)         (now_us += (us))
#define BC_MSLEEP(c, ms)      (now_us += (ms) * 1000ULL)
#define BC_NOW_MS(c)          ((ULONG)(now_us / 1000))
#define BC_LOG(...)           do { if (opt_log) printf("    " __VA_ARGS__); } while (0)
#define BC_LOCK(c)            (locked++)
#define BC_UNLOCK(c)          (locked--)
#include "../../WinRT/drivers/goyabatt/battcore.c"

static BC_HOST h;

static void physics(double dt)
{
    double ichg = 0;
    if (smb_charging() && !(smb_status_c() & 0xC0)) {
        ichg = 0.5;
        if (ocv(soc) > 4.30 * 1000) ichg = 0.15;     /* constant-voltage phase, roughly */
        if (smb_status_c() == 0x03) ichg = 0.1;
    }
    ibat = ichg - load_a;
    soc += ibat * dt / 3600 / CAP_AH;
    if (soc < 0) soc = 0;
    if (soc > 1) soc = 1;
}

/* advance by one poll period with the battery and the charger watchdog running */
static void run(int polls, int skip_polls)
{
    for (int i = 0; i < polls; i++) {
        for (int k = 0; k < 20; k++) { physics(0.1); now_us += 100000; smb_tick(); }
        if (!skip_polls) {
            BOOLEAN ch = BcPoll(&h);
            if (locked) { printf("FAIL lock imbalance\n"); exit(1); }
            (void)ch;
        }
    }
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void reset_all(double s, int cab)
{
    memset(&h, 0, sizeof(h));
    memset(g_out, 0, sizeof(g_out)); memset(g_dir, 0, sizeof(g_dir));
    memset(&smb, 0, sizeof(smb)); memset(&muic, 0, sizeof(muic));
    smb.name = "smb328"; smb.scl_pin = 16; smb.sda_pin = 17; smb.addr = 0x34; smb.present = 1; smb.rd = smb_rd; smb.wr = smb_wr; smb.scl = smb.sda = 1;
    muic.name = "rt8973"; muic.scl_pin = 49; muic.sda_pin = 50; muic.addr = 0x14; muic.present = 1; muic.rd = muic_rd; muic.wr = muic_wr; muic.scl = muic.sda = 1;
    memcpy(smb_r, smb_defaults, 8); smb_r[0x31] = 0x08; smb_r[0x37] = 0; smb_wd = 0; smb_last_us = now_us;
    memset(pm_gp, 0, sizeof(pm_gp));
    pm_gp[0x01] = 0x06; pm_gp[0x02] = 0x10; pm_gp[0x06] = 0x33; pm_gp[0x0D] = 0x06; pm_gp[0x14] = 0x44;
    tw_isr = 0; tw_dev = -1; apbc = 3; opt_pmic_absent = 0; pm_writes = 0;
    for (unsigned i = 0; i < 0x300 / 4; i++) mfpr[i] = 0xC000;
    soc = s; cable = cab; temp_c = 25; load_a = 0.3; smb_term = 0; opt_muic_latched = 1; smb_wd_after_s = 10;
    /* the firmware leaves CHG_EN driven low */
    g_dir[3] |= 1 << (98 & 31);
}

static int start(const char *name)
{
    printf("--- %s\n", name);
    int rc = BcInit(&h);
    CHECK(rc == 0, "BcInit %d", rc);
    CHECK(h.HasGsdr == opt_gsdr, "direction registers detected %d, sim %d", h.HasGsdr, opt_gsdr);
    return rc;
}

static void show(void)
{
    printf("  soc %.1f%% shown %u%% (%.2f) %u mV temp %d flags %x online %d charging %d full %d wd %u cfg %u err %u\n",
           soc * 100, BcPercent(&h), h.Soc / 100.0, h.VbatMv, h.TempDeci, BcPowerState(&h), h.Online, h.Charging, h.Full,
           h.Watchdogs, h.Configs, h.BusErrors);
}

static void t_usb_then_unplug(void)
{
    reset_all(0.5, CAB_SDP);
    start("USB cable at 50 %, then unplugged for 30 min");
    run(30, 0); show();
    CHECK(h.Online && h.Charging, "online %d charging %d", h.Online, h.Charging);
    CHECK(BcPowerState(&h) == (BATTERY_POWER_ON_LINE | BATTERY_CHARGING), "flags %x", BcPowerState(&h));
    CHECK(abs((int)BcPercent(&h) - (int)(soc * 100 + 0.5)) <= 6, "shown %u true %.1f", BcPercent(&h), soc * 100);
    CHECK(h.Watchdogs == 0 && h.BusErrors == 0, "watchdogs %u errors %u", h.Watchdogs, h.BusErrors);
    CHECK(!(smb_r[0x31] & 0x10) && (smb_r[0x31] & 0x08) && smb_r[0x00] == 0x1F && (smb_r[0x02] & 0x7E) == (((4320 - 3460) / 20) << 1),
          "charger registers %02x %02x %02x", smb_r[0x31], smb_r[0x00], smb_r[0x02]);
    cable = CAB_NONE;
    unsigned last = BcPercent(&h), up = 0;
    for (int i = 0; i < 900; i++) { run(1, 0); if (BcPercent(&h) > last) up++; last = BcPercent(&h); }
    show();
    CHECK(!h.Online && !h.Charging && BcPowerState(&h) == BATTERY_DISCHARGING, "after unplug flags %x", BcPowerState(&h));
    CHECK(up == 0, "percentage went up %u times on battery", up);
    CHECK(abs((int)BcPercent(&h) - (int)(soc * 100 + 0.5)) <= 6, "shown %u true %.1f", BcPercent(&h), soc * 100);
}

static void t_watchdog(void)
{
    reset_all(0.4, CAB_DCP);
    smb_wd = 1; memcpy(smb_r, smb_defaults, 8); smb_r[0x31] = 0x18;      /* expired while Windows was starting */
    start("charger watchdog expired at hand-off, then a 20 s gap in polling");
    run(3, 0); show();
    CHECK(h.Charging && h.Configs >= 1, "charging %d configured %u", h.Charging, h.Configs);
    unsigned w = h.Watchdogs;
    run(10, 1);                                                           /* driver stalled for 20 s */
    CHECK(!smb_charging(), "sim watchdog did not fire");
    run(2, 0); show();
    CHECK(h.Charging && h.Watchdogs == w + 1, "after gap: charging %d watchdogs %u", h.Charging, h.Watchdogs);
}

static void t_temperature(void)
{
    reset_all(0.6, CAB_SDP);
    start("hot and cold battery");
    run(3, 0);
    temp_c = 47; run(6, 0); show();
    CHECK(h.Online && !h.Charging && h.Hot && (smb_r[0x31] & 0x10), "hot: charging %d hot %d cmd %02x", h.Charging, h.Hot, smb_r[0x31]);
    CHECK(BcPowerState(&h) == BATTERY_POWER_ON_LINE, "hot flags %x", BcPowerState(&h));
    temp_c = 43.5; run(6, 0);
    CHECK(!h.Charging, "charging again inside the hysteresis");
    temp_c = 40; run(6, 0);
    CHECK(h.Charging && !h.Hot, "cooled: charging %d", h.Charging);
    temp_c = -3; run(6, 0); show();
    CHECK(!h.Charging && h.Cold, "cold: charging %d cold %d", h.Charging, h.Cold);
    temp_c = 5; run(6, 0);
    CHECK(h.Charging && !h.Cold, "warmed: charging %d", h.Charging);
}

static void t_temp_accuracy(void)
{
    reset_all(0.6, CAB_NONE);
    start("thermistor -30..85 C with bias switching (clamped outside -25..70)");
    double worst = 0;
    for (double t = -30; t <= 85; t += 2.5) {
        LONG d = 0; temp_c = t;
        int rc = BcReadTemp(&h, &d);
        double want = t > 70 ? 70 : t < -25 ? -25 : t, tol = t > 69 || t < -24 ? 1.5 : 0;
        CHECK(rc == 0, "no reading at %.1f C (%d)", t, rc);
        if (rc == 0 && fabs(d / 10.0 - want) - tol > worst) worst = fabs(d / 10.0 - want) - tol;
    }
    printf("  worst error %.2f C\n", worst);
    CHECK(worst <= 1.0, "worst error %.2f C", worst);
}

static void t_full(void)
{
    reset_all(0.97, CAB_DCP);
    start("charging to full");
    load_a = 0.1;
    int n = 0;
    while (!h.Full && n < 3000) { run(1, 0); n++; }
    run(60, 0); show();
    CHECK(h.Full && BcPercent(&h) == 100, "full %d shown %u after %d polls", h.Full, BcPercent(&h), n);
    CHECK(BcPowerState(&h) == BATTERY_POWER_ON_LINE, "full flags %x", BcPowerState(&h));
}

static void t_empty(void)
{
    reset_all(0.06, CAB_NONE);
    start("running down to empty");
    load_a = 1.0;
    int n = 0;
    while (soc > 0.001 && n < 2000) { run(1, 0); n++; }
    run(30, 0); show();
    CHECK(BcPercent(&h) <= 3 && (BcPowerState(&h) & BATTERY_CRITICAL), "shown %u flags %x", BcPercent(&h), BcPowerState(&h));
}

static void t_muic(void)
{
    reset_all(0.5, CAB_SDP);
    muic.present = 0;
    start("MUIC not answering");
    run(5, 0); show();
    CHECK(!h.MuicOk && h.Charging && h.Online, "muic %d charging %d online %d", h.MuicOk, h.Charging, h.Online);
    cable = CAB_NONE; run(3, 0);
    CHECK(!h.Online && !h.Charging && !(smb_r[0x31] & 0x10), "unplugged without MUIC: online %d cmd %02x", h.Online, smb_r[0x31]);

    reset_all(0.5, CAB_SDP);
    opt_muic_latched = 0;
    start("cable the MUIC has not latched (cold boot)");
    run(5, 0); show();
    CHECK(h.MuicOk && h.Cable == BC_CABLE_NONE && h.Online && h.Charging, "muic %d cable %u online %d", h.MuicOk, h.Cable, h.Online);
}

static void t_stuck(void)
{
    reset_all(0.5, CAB_SDP);
    /* the MUIC was cut off in the middle of a read and holds SDA low */
    muic.st = S_TX; muic.bits = 1; muic.sbyte = 0x00; muic.drive = 1; muic.sda = 0;
    start("MUIC holding SDA low at start-up");
    run(3, 0); show();
    CHECK(h.MuicOk && h.Recoveries >= 1 && h.Online, "muic %d recoveries %u", h.MuicOk, h.Recoveries);
}

static void t_pmic(void)
{
    reset_all(0.5, CAB_SDP);
    apbc = 0x7;                                  /* TWSI2 held in reset: driver enables it */
    start("TWSI2 clock off at start, later the PMIC stops answering");
    CHECK(apbc == 3, "APBC %x", apbc);
    run(3, 0);
    opt_pmic_absent = 1; run(5, 0); show();
    CHECK(!h.VbatOk && h.Charging && h.Seeded, "vbat %d charging %d", h.VbatOk, h.Charging);
    opt_pmic_absent = 0; run(2, 0);
    CHECK(h.VbatOk, "VBAT back");
    CHECK(pm_writes < 10, "%u PMIC writes", pm_writes);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-v")) opt_log = 1;
    for (opt_gsdr = 1; opt_gsdr >= 0; opt_gsdr--) {
        printf("===== GPIO direction set/clear registers %s =====\n", opt_gsdr ? "present" : "absent");
        t_usb_then_unplug();
        t_watchdog();
        t_temperature();
        t_temp_accuracy();
        t_full();
        t_empty();
        t_muic();
        t_stuck();
        t_pmic();
    }
    printf(fails ? "SOME TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", fails);
    return fails != 0;
}
