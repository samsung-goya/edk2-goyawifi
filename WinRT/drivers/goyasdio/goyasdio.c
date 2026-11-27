/*
 * goyasdio.sys - SDIO bus driver for the Marvell 88W8777 Wi-Fi/Bluetooth module on SDH2 (PXA988,
 * Galaxy Tab 3 Lite), Windows 8.0 RT build 8520. WDM.
 *
 * It replaces the Windows SD bus (sdbus.sys) for this slot: the inbox SD bus does not know the PXAv3
 * quirks (broken base-clock capability, no card-detect pin), and Windows 8.0 RT has no sdport.sys.
 *
 *   FDO on ACPI\MRVL0009 (SDH2 registers 0xD4280800). Initialises the SDIO card (sdiocore.c, polled,
 *   PIO) and creates one child PDO per SDIO function, with the hardware IDs the Windows SD bus uses:
 *       SD\VID_02DF&PID_9131&FN_1, SD\VID_02DF&PID_9131           (88W8777 WLAN)
 *       SD\VID_02DF&PID_9132&FN_2, SD\VID_02DF&PID_9132           (88W8777 Bluetooth)
 *   PDOs answer IRP_MN_QUERY_INTERFACE for GUID_SDBUS_INTERFACE_STANDARD and IOCTL_SD_SUBMIT_REQUEST,
 *   the contract between a function driver's statically linked sdbus.lib and the SD bus. The
 *   structure layouts below are the ones the Surface RT Marvell driver (mwls97w8arm.sys 14.62.17215.91)
 *   was built with, read from its code: SdBusOpenInterface sends IRP_MN_QUERY_INTERFACE (size 0x18,
 *   version 0x101); SdBusSubmitRequest sends IRP_MJ_INTERNAL_DEVICE_CONTROL 0x43073 with the 0x48-byte
 *   request packet in Type3InputBuffer to the device named in the interface context, which must
 *   start with the signature 'SDIC' (sdbus.lib checks it before every request).
 *
 *   Card interrupts are polled (1 ms): the SDH interrupt line is shared with the microSD controller,
 *   whose driver (goyasdhc) polls with every signal enable at 0. The callback runs at PASSIVE_LEVEL
 *   (or DISPATCH_LEVEL if the function driver asks) without the bus lock held: the Marvell callback
 *   reads its interrupt status with synchronous requests, then calls AcknowledgeInterrupt.
 *
 * The firmware (Pxa988SdhciDxe) powers the module: pins, LDO10 3.3 V, WLAN_PD (GPIO51) high. If the
 * card does not answer, goyasdio power-cycles it through GPIO51 and tries again.
 *
 * SPDX-License-Identifier: MIT
 */
#define GOYA_TRACE_ID 8
#include <ntddk.h>
#include <initguid.h>
#include "..\common\goyatrace.h"

/* ---- register access for sdiocore.c ---- */
#define SD_RD8(c, o)      READ_REGISTER_UCHAR((PUCHAR)((c)->Regs + (o)))
#define SD_RD16(c, o)     READ_REGISTER_USHORT((PUSHORT)((c)->Regs + (o)))
#define SD_RD32(c, o)     READ_REGISTER_ULONG((PULONG)((c)->Regs + (o)))
#define SD_WR8(c, o, v)   WRITE_REGISTER_UCHAR((PUCHAR)((c)->Regs + (o)), (UCHAR)(v))
#define SD_WR16(c, o, v)  WRITE_REGISTER_USHORT((PUSHORT)((c)->Regs + (o)), (USHORT)(v))
#define SD_WR32(c, o, v)  WRITE_REGISTER_ULONG((PULONG)((c)->Regs + (o)), (ULONG)(v))
#define SD_STALL(us)      KeStallExecutionProcessor(us)
#define SD_LOG            DbgPrint
#include "sdiocore.c"

#define GS_TAG            'oidS'
#define SDH2_PHYS         0xD4280800UL
#define APMU_SDH1_PHYS    0xD4282858UL     /* SDH2's clock register (Linux PXA988_CLK_SDH1) */
#define GPIO_BANK1_PHYS   0xD4019004UL     /* GPIO 32..63 */
#define   GPIO_GPSR       0x18
#define   GPIO_GPCR       0x24
#define   GPIO_GSDR       0x54
#define WLAN_PD_BIT       (1UL << (51 - 32))
#define MFPR_PHYS         0xD401E000UL
#define   MFPR_MMC2_FIRST 0x0170           /* GPIO37..42 = MMC2 DAT3..DAT0, CMD, CLK: AF1 */
#define   MFPR_MMC2_LAST  0x0184
#define   MFPR_GPIO51     0x01A8           /* WLAN_PD: AF0 (GPIO) */
#define APMU_SDH_ON       0x0013           /* RST_N | AXI_RST_N | FCLK_EN */
#define APMU_DIV_SHIFT    8
#define APMU_DIV_MASK     0x0700
#define APMU_SRC_MASK     0x00C0

/* ---- the SD bus contract (layouts of the Surface RT Marvell driver's sdbus.lib) ---- */
#define GS_IOCTL_SD_SUBMIT_REQUEST  0x00043073UL   /* CTL_CODE(FILE_DEVICE_CONTROLLER, 0xC1C, METHOD_NEITHER, FILE_ANY_ACCESS) */
#define GS_SDBUS_SIGNATURE          0x43494453UL   /* 'SDIC' */
DEFINE_GUID(GS_GUID_SDBUS_INTERFACE_STANDARD, 0x6bb24d81, 0xe924, 0x4825, 0xaf, 0x49, 0x3a, 0xcd, 0x33, 0xc1, 0xd8, 0x20);
DEFINE_GUID(GS_GUID_BUS_TYPE_SD, 0xe700cc04, 0x4036, 0x4e89, 0x95, 0x79, 0x89, 0xeb, 0xf4, 0x5f, 0x00, 0xcd);   /* GUID_BUS_TYPE_SD */

typedef VOID (*GS_SDBUS_CALLBACK)(PVOID CallbackRoutineContext, ULONG InterruptType);

typedef struct _GS_INTERFACE_PARAMETERS {          /* SDBUS_INTERFACE_PARAMETERS, 0x14 bytes */
    USHORT          Size;
    USHORT          Reserved;
    PDEVICE_OBJECT  TargetObject;
    BOOLEAN         DeviceGeneratesInterrupts;
    BOOLEAN         CallbackAtDpcLevel;
    GS_SDBUS_CALLBACK CallbackRoutine;
    PVOID           CallbackRoutineContext;
} GS_INTERFACE_PARAMETERS, *PGS_INTERFACE_PARAMETERS;

typedef NTSTATUS (*GS_INIT_INTERFACE)(PVOID Context, PGS_INTERFACE_PARAMETERS Parameters);
typedef NTSTATUS (*GS_ACK_INTERRUPT)(PVOID Context);

typedef struct _GS_INTERFACE_STANDARD {            /* SDBUS_INTERFACE_STANDARD, 0x18 bytes */
    USHORT          Size;
    USHORT          Version;
    PVOID           Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    GS_INIT_INTERFACE      InitializeInterface;
    GS_ACK_INTERRUPT       AcknowledgeInterrupt;
} GS_INTERFACE_STANDARD, *PGS_INTERFACE_STANDARD;

typedef struct _GS_CMD_DESC {                      /* SDCMD_DESCRIPTOR, 0x14 bytes; Cmd is a UCHAR (rest of its word is stack garbage) */
    UCHAR Cmd;
    UCHAR Pad[3];
    ULONG CmdClass;                                /* 0 standard, 1 application */
    ULONG TransferDirection;                       /* 0 unspecified, 1 read, 2 write */
    ULONG TransferType;                            /* 1 cmd only, 2 single block, 3 multi block, 4 multi block no CMD12 */
    ULONG ResponseType;                            /* 7 = R5 */
} GS_CMD_DESC;

typedef struct _GS_REQUEST_PACKET {                /* SDBUS_REQUEST_PACKET, 0x48 bytes */
    ULONG       RequestFunction;                   /* 0x00: 0 get property, 1 set property, 2 device command */
    PVOID       UserContext[3];                    /* 0x04 */
    ULONG_PTR   Information;                       /* 0x10 */
    UCHAR       ResponseData[16];                  /* 0x14: R5 little endian, byte 0 = CMD52 data */
    UCHAR       ResponseLength;                    /* 0x24 */
    UCHAR       Reserved;
    USHORT      Reserved2;
    union {                                        /* 0x28 */
        struct { ULONG Property; PVOID Buffer; ULONG Length; } GetSetProperty;
        struct { GS_CMD_DESC CmdDesc; ULONG Argument; PMDL Mdl; ULONG Length; } DeviceCommand;
    } Parameters;
} GS_REQUEST_PACKET, *PGS_REQUEST_PACKET;

C_ASSERT(sizeof(GS_INTERFACE_PARAMETERS) == 0x14);
C_ASSERT(sizeof(GS_INTERFACE_STANDARD) == 0x18);
C_ASSERT(sizeof(GS_CMD_DESC) == 0x14);
C_ASSERT(FIELD_OFFSET(GS_REQUEST_PACKET, ResponseData) == 0x14);
C_ASSERT(FIELD_OFFSET(GS_REQUEST_PACKET, Parameters) == 0x28);
C_ASSERT(FIELD_OFFSET(GS_REQUEST_PACKET, Parameters.DeviceCommand.Argument) == 0x3C);
C_ASSERT(FIELD_OFFSET(GS_REQUEST_PACKET, Parameters.DeviceCommand.Mdl) == 0x40);
C_ASSERT(FIELD_OFFSET(GS_REQUEST_PACKET, Parameters.DeviceCommand.Length) == 0x44);
C_ASSERT(sizeof(GS_REQUEST_PACKET) == 0x48);

enum { SDRF_GET_PROPERTY = 0, SDRF_SET_PROPERTY = 1, SDRF_DEVICE_COMMAND = 2 };
enum {                                             /* SDBUS_PROPERTY (sizes as the Marvell driver passes them) */
    SDP_MEDIA_STATE = 1, SDP_WRITE_PROTECTED, SDP_FUNCTION_NUMBER, SDP_FUNCTION_TYPE, SDP_BUS_DRIVER_VERSION,
    SDP_BUS_WIDTH, SDP_BUS_CLOCK, SDP_BUS_INTERFACE_CONTROL, SDP_HOST_BLOCK_LENGTH, SDP_FUNCTION_BLOCK_LENGTH,
    SDP_FN0_BLOCK_LENGTH, SDP_FUNCTION_INT_ENABLE,
    SDP_13 = 13                                    /* BOOLEAN set to 1 at init and with SDP_FUNCTION_INT_ENABLE on the D3 path: interrupt forwarding */
};

/* ---- device extensions ---- */
typedef struct _GS_COMMON {
    BOOLEAN         IsFdo;
} GS_COMMON;

struct _GS_FDO;

typedef struct _GS_IFCTX {                         /* the interface Context: sdbus.lib reads Signature and Target */
    ULONG           Signature;
    PDEVICE_OBJECT  Target;
} GS_IFCTX;

typedef struct _GS_PDO {
    GS_COMMON       Common;
    GS_IFCTX        IfCtx;                         /* first field the function driver sees */
    struct _GS_FDO *Fdo;
    PDEVICE_OBJECT  Self;
    ULONG           Fn;
    BOOLEAN         Reported;                      /* listed in the last BusRelations */
    BOOLEAN         Deleted;
    BOOLEAN         Started;
    volatile LONG   IfRefs;
    GS_SDBUS_CALLBACK Callback;
    PVOID           CallbackCtx;
    BOOLEAN         CallbackAtDpc;
    volatile BOOLEAN IntOn;                        /* CCCR IEN for this function */
    volatile BOOLEAN Forward;                      /* SDP_13 */
    volatile BOOLEAN IntPending;                   /* callback made, no AcknowledgeInterrupt yet */
    ULONG           Ints;
} GS_PDO, *PGS_PDO;

typedef struct _GS_FDO {
    GS_COMMON       Common;
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  Lower;
    PDEVICE_OBJECT  Pdo;
    SDIO_HOST       Sd;
    ULONG           RegLen;
    volatile ULONG *ApmuClk;
    volatile UCHAR *Gpio1;
    volatile UCHAR *Mfpr;
    FAST_MUTEX      BusLock;
    BOOLEAN         Started;
    BOOLEAN         CardOk;
    ULONG           InitTries;
    PGS_PDO         Child[SDIO_MAX_FN + 1];
    PKTHREAD        Thread;
    KEVENT          StopEvt;
    BOOLEAN         TimerRes;
    ULONG           CardInts;
    ULONG           Requests;
    ULONG           ReqErrors;
} GS_FDO, *PGS_FDO;

static PDRIVER_OBJECT gDriver;

/* ---- helpers ---- */

static VOID GsDelayMs(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static PVOID GsMap(ULONG phys, ULONG len)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = phys;
    return MmMapIoSpace(pa, len, MmNonCached);
}

static VOID GsTraceStats(PGS_FDO f)
{
    GoyaTraceValue(1, ((f->Sd.Cmd53s & 0xFFFF) << 16) | ((f->Sd.Errors & 0xFF) << 8) | (f->CardInts & 0xFF));
}

/* Pins as the Linux DT has them (sdh2_wlan_pins, wlan_bt_ctrl_pins); normally set by the firmware */
static VOID GsPins(PGS_FDO f)
{
    ULONG off, v;
    if (!f->Mfpr) return;
    for (off = MFPR_MMC2_FIRST; off <= MFPR_MMC2_LAST; off += 4) {
        v = READ_REGISTER_ULONG((PULONG)(f->Mfpr + off));
        if ((v & 7) != 1) WRITE_REGISTER_ULONG((PULONG)(f->Mfpr + off), (v & ~7UL) | 1);
    }
    v = READ_REGISTER_ULONG((PULONG)(f->Mfpr + MFPR_GPIO51));
    if (v & 7) WRITE_REGISTER_ULONG((PULONG)(f->Mfpr + MFPR_GPIO51), v & ~7UL);
}

/* WLAN_PD (GPIO51) low for 20 ms, then high: the module restarts (Linux mmc-pwrseq-simple, reset-gpios
   GPIO51 active low, post-power-on-delay 20 ms; vendor kernel WIB_PDn). */
static VOID GsPowerCycle(PGS_FDO f)
{
    if (!f->Gpio1) return;
    WRITE_REGISTER_ULONG((PULONG)(f->Gpio1 + GPIO_GPCR), WLAN_PD_BIT);
    WRITE_REGISTER_ULONG((PULONG)(f->Gpio1 + GPIO_GSDR), WLAN_PD_BIT);
    GsDelayMs(20);
    WRITE_REGISTER_ULONG((PULONG)(f->Gpio1 + GPIO_GPSR), WLAN_PD_BIT);
    GsDelayMs(50);
}

static VOID GsClock(PGS_FDO f)
{
    ULONG v, d;
    f->Sd.BaseHz = SDIO_DEFAULT_BASE;
    if (!f->ApmuClk) return;
    v = READ_REGISTER_ULONG((PULONG)f->ApmuClk);
    if ((v & APMU_SDH_ON) != APMU_SDH_ON) {
        /* off: same setting as the firmware (pll1_416 / 8 = 52 MHz). AXI clock enable is in SDH0 (goyasdhc/firmware) */
        v = APMU_SDH_ON | (7 << APMU_DIV_SHIFT) | 0x0800;
        WRITE_REGISTER_ULONG((PULONG)f->ApmuClk, v);
        KeStallExecutionProcessor(50);
    }
    if (!(v & APMU_SRC_MASK)) {                    /* pll1_416 source: base = 416 MHz / (div + 1) */
        d = (v & APMU_DIV_MASK) >> APMU_DIV_SHIFT;
        f->Sd.BaseHz = 416000000UL / (d + 1);
    }
}

static BOOLEAN GsInitCard(PGS_FDO f)
{
    SD_STATUS st = SD_ENOCARD;
    ULONG tries;

    GoyaTraceStep(0x10);
    GsPins(f);
    GsClock(f);
    f->Sd.MaxHz = 26000000;                        /* Linux DT max-frequency for this slot */
    f->Sd.ClkDelay = 15;                           /* Linux DT mrvl,clk-delay-cycles */
    for (tries = 0; tries < 3; tries++) {
        f->InitTries++;
        if (tries) GsPowerCycle(f);
        ExAcquireFastMutex(&f->BusLock);
        st = SdioInitCard(&f->Sd);
        ExReleaseFastMutex(&f->BusLock);
        if (st == SD_OK) break;
        DbgPrint("goyasdio: card init failed (%d), try %u\n", st, tries + 1);
        if (tries == 1) f->Sd.MaxHz = 13000000;    /* last try slower */
    }
    if (st != SD_OK) {
        GoyaTraceStep(0x1F);
        GoyaTraceValue(0, 0xEEEE0000UL | ((ULONG)st << 8) | (f->InitTries & 0xFF));
        return FALSE;
    }
    GoyaTraceStep(0x11);
    GoyaTraceValue(0, ((ULONG)f->Sd.FnVid[1] << 16) | f->Sd.FnPid[1]);
    return TRUE;
}

/* ---- PnP strings ---- */

static PWCHAR GsHex4(PWCHAR p, ULONG v)
{
    static const WCHAR h[] = L"0123456789ABCDEF";
    int i;
    for (i = 12; i >= 0; i -= 4) *p++ = h[(v >> i) & 0xF];
    return p;
}

static PWCHAR GsStr(PWCHAR p, PCWSTR s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* "SD\VID_vvvv&PID_pppp[&FN_n]" */
static PWCHAR GsSdId(PWCHAR p, ULONG vid, ULONG pid, ULONG fn)
{
    p = GsStr(p, L"SD\\VID_"); p = GsHex4(p, vid);
    p = GsStr(p, L"&PID_"); p = GsHex4(p, pid);
    if (fn) { p = GsStr(p, L"&FN_"); *p++ = (WCHAR)(L'0' + fn); }
    return p;
}

static NTSTATUS GsQueryId(PGS_PDO p, BUS_QUERY_ID_TYPE type, PWCHAR *out)
{
    PGS_FDO f = p->Fdo;
    ULONG vid = f->Sd.FnVid[p->Fn], pid = f->Sd.FnPid[p->Fn];
    PWCHAR b, q;
    b = (PWCHAR)ExAllocatePoolWithTag(PagedPool, 256 * sizeof(WCHAR), GS_TAG);
    if (!b) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(b, 256 * sizeof(WCHAR));
    q = b;
    switch (type) {
    case BusQueryDeviceID:
        q = GsSdId(q, vid, pid, 0); *q = 0;
        break;
    case BusQueryHardwareIDs:
        q = GsSdId(q, vid, pid, p->Fn); *q++ = 0;
        q = GsSdId(q, vid, pid, 0); *q++ = 0;
        if (f->Sd.CardPid && (f->Sd.CardVid != vid || f->Sd.CardPid != pid)) { q = GsSdId(q, f->Sd.CardVid, f->Sd.CardPid, p->Fn); *q++ = 0; }
        *q = 0;
        break;
    case BusQueryCompatibleIDs:
        q = GsStr(q, L"SD\\CLASS_"); q = GsHex4(q, f->Sd.FnCode[p->Fn]); *q++ = 0;
        q = GsStr(q, L"GOYASDIO\\FUNCTION"); *q++ = 0;
        *q = 0;
        break;
    case BusQueryInstanceID:
        q = GsStr(q, L"FN"); *q++ = (WCHAR)(L'0' + p->Fn); *q = 0;
        break;
    default:
        ExFreePoolWithTag(b, GS_TAG);
        return STATUS_NOT_SUPPORTED;
    }
    *out = b;
    return STATUS_SUCCESS;
}

/* ---- request processing (PASSIVE_LEVEL, bus lock taken here) ---- */

/* Bring-up trace to the boot log (goyalog): up to GS_TRACE_REQS requests, then only failures (below).
   The firmware download loop (CMD53 writes to the function 1 data port 0x10000 and the CMD52 polls of
   its status 0x30 and length 0x40/0x41) is only shown for its first GS_TRACE_LOOP requests and then
   counted, so what the driver does after the download still fits. Rejected IOCTLs and unhandled PnP
   requests are logged separately. */
#define GS_TRACE_REQS 600
#define GS_TRACE_LOOP 40
static volatile LONG gTraceReqs, gTraceOther, gTraceLoop, gTraceSkipped, gTraceSawRead, gTraceInts;

static BOOLEAN GsIsDownloadLoop(PGS_REQUEST_PACKET r)
{
    ULONG a, addr;
    if (r->RequestFunction != SDRF_DEVICE_COMMAND || gTraceSawRead) return FALSE;  /* reads: download is over */
    a = r->Parameters.DeviceCommand.Argument;
    addr = (a >> 9) & 0x1FFFF;
    if (((a >> 28) & 7) != 1) return FALSE;
    if (r->Parameters.DeviceCommand.CmdDesc.Cmd == 53) return (a >> 31) && addr == 0x10000;
    return !(a >> 31) && (addr == 0x30 || addr == 0x40 || addr == 0x41);
}

/* Bring-up: the Marvell driver's own log. goyawifi.sys (mwls97w8arm.sys 14.62.17215.91) formats its MOAL/MLAN
   messages ("[MOAL file line func] text") into a ring in its .data: control block at RVA 0x11eba8, +16 the
   message mask, +24 the enable flag, 512 entries of 192 bytes at +0x1c, counter at +0x1801c (pre-increment,
   entry = counter & 511). Nothing reads the ring on Windows RT, so goyasdio finds the image from the interrupt
   callback the driver registers, checks it is that build ("moal_main.cpp" at RVA 0x85984), turns the log on
   and copies new entries to the boot log as requests go by. */
NTSYSAPI PVOID NTAPI RtlPcToFileHeader(PVOID PcValue, PVOID *BaseOfImage);
#define GW_LOG_RVA      0x11eba8
#define GW_LOG_NAME_RVA 0x85984
/* MLAN debug classes: MSG 1, FATAL 2, ERROR 4, DATA 8, CMND 0x10, EVENT 0x20, IOCTL 0x80, MPA_D 0x8000, DAT_D 0x10000,
   CMD_D 0x20000, EVT_D 0x40000, WARN 0x20000000, HEX_DUMP 0x80000000,
   INFO 0x40000000 (not INTR, FW_D/IF_D (every download block) or ENTRY) */
#define GW_LOG_MASK     0xE00780BF
static PUCHAR gWifiLog;
static LONG gWifiLogSeen;
static volatile LONG gWifiLogBusy;

static VOID GsWifiLogArm(VOID)
{
    if (!gWifiLog) return;
    if (*(volatile ULONG *)(gWifiLog + 16) != GW_LOG_MASK) *(volatile ULONG *)(gWifiLog + 16) = GW_LOG_MASK;
    if (*(volatile ULONG *)(gWifiLog + 24) != 1) *(volatile ULONG *)(gWifiLog + 24) = 1;
}

static VOID GsWifiLogAttach(PVOID routine)
{
    PVOID base = NULL;
    if (gWifiLog || !routine) return;
    RtlPcToFileHeader(routine, &base);
    if (!base || RtlCompareMemory((PUCHAR)base + GW_LOG_NAME_RVA, "moal_main.cpp", 14) != 14) {
        DbgPrint("goyasdio: function driver at %p is not the known Marvell build: its log stays off\n", base);
        return;
    }
    gWifiLog = (PUCHAR)base + GW_LOG_RVA;
    gWifiLogSeen = *(volatile LONG *)(gWifiLog + 0x1801c);
    GsWifiLogArm();
    DbgPrint("goyasdio: Marvell driver at %p, its log turned on (counter %d)\n", base, gWifiLogSeen);
}

static VOID GsWifiLogDump(VOID)
{
    LONG n, i;
    char line[193];
    if (!gWifiLog || InterlockedExchange(&gWifiLogBusy, 1)) return;
    GsWifiLogArm();
    n = *(volatile LONG *)(gWifiLog + 0x1801c);
    if (n - gWifiLogSeen > 511) {
        DbgPrint("mrvl: ... %d messages lost\n", n - gWifiLogSeen - 511);
        gWifiLogSeen = n - 511;
    }
    for (i = gWifiLogSeen + 1; i - n <= 0; i++) {
        ULONG k;
        RtlCopyMemory(line, gWifiLog + 0x1c + (i & 511) * 192, 192);
        line[192] = 0;
        for (k = 0; line[k]; k++) if (line[k] == '\n' || line[k] == '\r') line[k] = ' ';
        DbgPrint("mrvl: %s\n", line);
    }
    gWifiLogSeen = n;
    InterlockedExchange(&gWifiLogBusy, 0);
}

/* Data ports (function 1, 0x10001..0x1FFFF: ports 1-15 and multi-port aggregation; port 0 at 0x10000 carries
   commands and events): the first GS_TRACE_DATA transfers with their first 96 bytes, apart from the request
   cap above, to see the data path (TxPD/RxPD, 802.3 header, EAPOL 0x888e) once a connection starts. */
#define GS_TRACE_DATA 120
static volatile LONG gTraceData;

static VOID GsTraceData(PGS_PDO p, PGS_REQUEST_PACKET r, NTSTATUS s)
{
    ULONG a, addr, len, i, o, n;
    PMDL mdl;
    PUCHAR va;
    static const char hd[] = "0123456789abcdef";
    if (r->RequestFunction != SDRF_DEVICE_COMMAND || r->Parameters.DeviceCommand.CmdDesc.Cmd != 53) return;
    a = r->Parameters.DeviceCommand.Argument;
    addr = (a >> 9) & 0x1FFFF;
    if (((a >> 28) & 7) != 1 || addr <= 0x10000 || !gTraceSawRead) return;
    if (InterlockedIncrement(&gTraceData) > GS_TRACE_DATA) return;
    len = r->Parameters.DeviceCommand.Length;
    mdl = r->Parameters.DeviceCommand.Mdl;
    va = (mdl && len) ? (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority) : NULL;
    DbgPrint("goyasdio: f%u data %s port 0x%05x %s len %u st %08x\n", p->Fn, (a >> 31) ? "W" : "R", addr,
             (a & (1UL << 27)) ? "blk" : "byte", len, s);
    if (!va) return;
    n = len < 96 ? len : 96;
    for (o = 0; o < n; o += 32) {
        char hex[3 * 32 + 1];
        ULONG m = n - o < 32 ? n - o : 32;
        for (i = 0; i < m; i++) {
            hex[3 * i] = hd[va[o + i] >> 4];
            hex[3 * i + 1] = hd[va[o + i] & 15];
            hex[3 * i + 2] = ' ';
        }
        hex[3 * m] = 0;
        DbgPrint("goyasdio:   +%02x %s\n", o, hex);
    }
}

static VOID GsTraceReq(PGS_PDO p, PGS_REQUEST_PACKET r, NTSTATUS s)
{
    GsWifiLogDump();
    GsTraceData(p, r, s);
    if (GsIsDownloadLoop(r) && InterlockedIncrement(&gTraceLoop) > GS_TRACE_LOOP) {
        InterlockedIncrement(&gTraceSkipped);
        return;
    }
    if (InterlockedIncrement(&gTraceReqs) > GS_TRACE_REQS) return;
    if (gTraceSkipped) {
        DbgPrint("goyasdio: ... %d download-loop requests not shown (%d CMD53 so far, %u bus errors)\n",
                 InterlockedExchange(&gTraceSkipped, 0), p->Fdo ? p->Fdo->Sd.Cmd53s : 0, p->Fdo ? p->Fdo->Sd.Errors : 0);
    }
    if (r->RequestFunction == SDRF_DEVICE_COMMAND) {
        ULONG a = r->Parameters.DeviceCommand.Argument;
        UCHAR cmd = r->Parameters.DeviceCommand.CmdDesc.Cmd;
        ULONG r5 = 0;
        RtlCopyMemory(&r5, r->ResponseData, 4);
        if (cmd == 52)
            DbgPrint("goyasdio: f%u CMD52 %s fn%u 0x%05x = %02x -> %02x st %08x\n", p->Fn, (a >> 31) ? "W" : "R",
                     (a >> 28) & 7, (a >> 9) & 0x1FFFF, a & 0xFF, r5 & 0xFF, s);
        else {
            ULONG len = r->Parameters.DeviceCommand.Length, i, n;
            PMDL mdl = r->Parameters.DeviceCommand.Mdl;
            PUCHAR va = (mdl && len) ? (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority) : NULL;
            char hex[3 * 32 + 1];
            static const char hd[] = "0123456789abcdef";
            if (cmd == 53 && !(a >> 31)) InterlockedExchange(&gTraceSawRead, 1);
            n = len < 32 ? len : 32;
            hex[0] = 0;
            if (va && gTraceSawRead) {
                for (i = 0; i < n; i++) {
                    hex[3 * i] = hd[va[i] >> 4];
                    hex[3 * i + 1] = hd[va[i] & 15];
                    hex[3 * i + 2] = ' ';
                }
                hex[3 * n] = 0;
            }
            DbgPrint("goyasdio: f%u CMD%u %s fn%u 0x%05x %s count %u len %u st %08x %s\n", p->Fn, cmd, (a >> 31) ? "W" : "R",
                     (a >> 28) & 7, (a >> 9) & 0x1FFFF, (a & (1UL << 27)) ? "blk" : "byte", a & 0x1FF,
                     len, s, hex);
        }
    } else {
        ULONG v = 0, n = r->Parameters.GetSetProperty.Length;
        if (r->Parameters.GetSetProperty.Buffer && n) RtlCopyMemory(&v, r->Parameters.GetSetProperty.Buffer, n < 4 ? n : 4);
        DbgPrint("goyasdio: f%u %s property %u len %u value %x st %08x\n", p->Fn,
                 r->RequestFunction == SDRF_GET_PROPERTY ? "get" : r->RequestFunction == SDRF_SET_PROPERTY ? "set" : "???",
                 r->Parameters.GetSetProperty.Property, n, v, s);
    }
}

static NTSTATUS GsStatus(SD_STATUS st)
{
    switch (st) {
    case SD_OK:       return STATUS_SUCCESS;
    case SD_ETIMEOUT: return STATUS_IO_TIMEOUT;
    case SD_EPARAM:   return STATUS_INVALID_PARAMETER;
    case SD_ENOCARD:  return STATUS_NO_SUCH_DEVICE;
    default:          return STATUS_IO_DEVICE_ERROR;
    }
}

static NTSTATUS GsGetProperty(PGS_PDO p, ULONG prop, PVOID buf, ULONG len, ULONG_PTR *info)
{
    PGS_FDO f = p->Fdo;
    ULONG v, sz;
    switch (prop) {
    case SDP_MEDIA_STATE:           v = f->CardOk ? 1 : 0; sz = 4; break;
    case SDP_WRITE_PROTECTED:       v = 0; sz = 1; break;
    case SDP_FUNCTION_NUMBER:       v = p->Fn; sz = 1; break;
    case SDP_FUNCTION_TYPE:         v = 1; sz = 4; break;                 /* I/O function */
    case SDP_BUS_DRIVER_VERSION:    v = 0x200; sz = 2; break;             /* SDBUS_DRIVER_VERSION_2 */
    case SDP_BUS_WIDTH:             v = f->Sd.Wide ? 4 : 1; sz = 1; break;
    case SDP_BUS_CLOCK:             v = f->Sd.ClockHz / 1000; sz = 4; break;  /* kHz */
    case SDP_BUS_INTERFACE_CONTROL: v = f->Sd.BusIf; sz = 1; break;
    case SDP_HOST_BLOCK_LENGTH:     v = 2048; sz = 2; break;
    case SDP_FUNCTION_BLOCK_LENGTH: v = f->Sd.FnBlk[p->Fn]; sz = 2; break;
    case SDP_FN0_BLOCK_LENGTH:      v = f->Sd.FnBlk[0]; sz = 2; break;
    case SDP_FUNCTION_INT_ENABLE:   v = p->IntOn; sz = 1; break;
    case SDP_13:                    v = p->Forward; sz = 1; break;
    default:                        return STATUS_NOT_SUPPORTED;
    }
    if (!buf || len < sz) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(buf, len);
    RtlCopyMemory(buf, &v, sz);                    /* little endian */
    *info = sz;
    return STATUS_SUCCESS;
}

static NTSTATUS GsSetProperty(PGS_PDO p, ULONG prop, PVOID buf, ULONG len)
{
    PGS_FDO f = p->Fdo;
    ULONG v = 0;
    SD_STATUS st = SD_OK;
    if (!buf || len == 0) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&v, buf, len < 4 ? len : 4);
    switch (prop) {
    case SDP_BUS_CLOCK:                            /* kHz (values that only make sense in Hz are taken as Hz) */
        if (v == 0) return STATUS_INVALID_PARAMETER;
        st = SdioSetClock(&f->Sd, v > 400000 ? v : v * 1000);
        break;
    case SDP_BUS_WIDTH:
        st = SdioSetBusWidth(&f->Sd, (v & 0xFF) == 4);
        break;
    case SDP_BUS_INTERFACE_CONTROL:
        st = SdioWrite8(&f->Sd, 0, CCCR_BUSIF, (UCHAR)v);
        if (st == SD_OK) f->Sd.BusIf = (UCHAR)v;
        break;
    case SDP_FUNCTION_BLOCK_LENGTH:
        st = SdioSetBlockLen(&f->Sd, p->Fn, v & 0xFFFF);
        break;
    case SDP_FN0_BLOCK_LENGTH:
        st = SdioSetBlockLen(&f->Sd, 0, v & 0xFFFF);
        break;
    case SDP_FUNCTION_INT_ENABLE:
        st = SdioFnIntEnable(&f->Sd, p->Fn, (v & 0xFF) != 0);
        if (st == SD_OK) p->IntOn = (v & 0xFF) != 0;
        break;
    case SDP_13:
        p->Forward = (v & 0xFF) != 0;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    return GsStatus(st);
}

/* Firmware compatibility shim. The Surface RT driver (FP62 MLAN, written for the 8797) sends host commands
   the SM-T110's 8777 FP66 firmware does not implement; it answers them with result 2 (not supported) and the
   driver then fails its initialization (Code 10). For the commands listed here a "not supported" response
   read from the command port is reported as success instead. SDIO data-port packet: le16 length, le16 type
   (1 = command response), then the command header: le16 command | 0x8000, le16 size, le16 seq, le16 result. */
static const USHORT gGsOptionalCmds[] = { 0x00d4 };
static volatile LONG gGsShimLog;

static VOID GsCmdRespShim(PGS_PDO p, ULONG arg, PUCHAR va, ULONG len)
{
    USHORT type, id, res;
    ULONG i;
    if ((arg >> 31) || ((arg >> 28) & 7) != 1 || ((arg >> 9) & 0x1FFFF) < 0x10000 || len < 12) return;
    type = (USHORT)(va[2] | (va[3] << 8));
    id = (USHORT)(va[4] | (va[5] << 8));
    res = (USHORT)(va[10] | (va[11] << 8));
    if (type == 1 && id == 0x8003) {               /* GET_HW_SPEC response: the firmware's capabilities, in full */
        ULONG n = (ULONG)(va[0] | (va[1] << 8)), o;
        if (n > len) n = len;
        if (n > 128) n = 128;
        for (o = 0; o < n; o += 16) {
            char hex[3 * 16 + 1];
            static const char hd[] = "0123456789abcdef";
            ULONG k, m = n - o < 16 ? n - o : 16;
            for (k = 0; k < m; k++) {
                hex[3 * k] = hd[va[o + k] >> 4];
                hex[3 * k + 1] = hd[va[o + k] & 15];
                hex[3 * k + 2] = ' ';
            }
            hex[3 * m] = 0;
            DbgPrint("goyasdio: f%u HW_SPEC +%02x: %s\n", p->Fn, o, hex);
        }
    }
    if (type != 1 || !(id & 0x8000) || res != 2) return;
    for (i = 0; i < RTL_NUMBER_OF(gGsOptionalCmds); i++) {
        if ((id & 0x7FFF) == gGsOptionalCmds[i]) {
            va[10] = 0;
            va[11] = 0;
            if (InterlockedIncrement(&gGsShimLog) <= 20)
                DbgPrint("goyasdio: f%u firmware does not support command %04x: reported as done\n", p->Fn, id & 0x7FFF);
            return;
        }
    }
}

/* Received data (function 1 data ports 0x10001-0x1FFFF, read). Each packet: le16 length, le16 type (0 = data),
   then the RxPD: u8 bss_type, u8 bss_num, le16 rx_pkt_length, le16 rx_pkt_offset, le16 rx_pkt_type, ...
   goyawifi is a Native Wi-Fi driver written for the 8797 Windows firmware, which delivers the whole 802.11 frame
   at rx_pkt_offset; the driver hands that to Windows as it is (its receive filter reads the frame type from the
   first byte there). The 8777 FP66 firmware marks received frames rx_pkt_type 2 and delivers an Ethernet frame
   (DA, SA, type) at rx_pkt_offset, with the frame's 802.11 header copied in at RxPD+0x22. Windows then gets a
   frame it cannot parse (the destination address read as the frame control) and never answers WPA2 message 1.
   The frame is rebuilt in place as the 8797 firmware delivers it: 802.11 header, LLC/SNAP AA AA 03 00 00 00 +
   type, payload, ending where the Ethernet frame ended; rx_pkt_offset/length are updated and the type becomes 0.
   The firmware has already decrypted the payload but its header copy keeps the Protected bit; that is cleared, or
   Windows takes the start of the payload for a CCMP header and drops every encrypted frame (no DHCP lease).
   goyawifi takes RxPD byte 17 for "arrived unencrypted" (its Exclude Unencrypted filter drops such data frames);
   the 8777 has other data there (nonzero), so it is set from the Protected bit: 0 encrypted, 1 not. Byte 16
   (8797 flags, bits 2 and 3 read by MLAN) holds unrelated 8777 data too and is cleared.
   Aggregated reads hold several packets, each padded to the 256-byte block. */
#define GS_RXPD_HDR 0x22                       /* 802.11 header copy in the 8777 RxPD */
static volatile LONG gGsRxShimLog;

static VOID GsRxDataShim(PGS_PDO p, ULONG arg, PUCHAR va, ULONG len)
{
    ULONG off = 0, plen, r, po, pl, hl, e, npo, npl;
    PUCHAR h;
    if ((arg >> 31) || ((arg >> 28) & 7) != 1 || ((arg >> 9) & 0x1FFFF) <= 0x10000) return;
    while (off + 12 <= len) {
        plen = (ULONG)(va[off] | (va[off + 1] << 8));
        if (plen < 12 || plen > len - off) break;
        r = off + 4;                                   /* RxPD */
        if (va[off + 2] == 0 && va[off + 3] == 0 && va[r + 6] == 2 && va[r + 7] == 0 && plen >= 4 + GS_RXPD_HDR + 24) {
            pl = (ULONG)(va[r + 2] | (va[r + 3] << 8));
            po = (ULONG)(va[r + 4] | (va[r + 5] << 8));
            h = va + r + GS_RXPD_HDR;
            hl = 24 + ((h[1] & 3) == 3 ? 6 : 0) + ((h[0] & 0x80) ? 2 : 0) + (((h[0] & 0x80) && (h[1] & 0x80)) ? 4 : 0);
            e = r + po;
            if ((h[0] & 0x0C) == 0x08 && pl >= 14 && GS_RXPD_HDR + hl <= po && e + pl <= off + plen) {
                if (((va[e + 12] << 8) | va[e + 13]) >= 0x600) {      /* Ethernet II: SNAP header before the type */
                    va[e + 6] = 0xAA; va[e + 7] = 0xAA; va[e + 8] = 0x03; va[e + 9] = 0; va[e + 10] = 0; va[e + 11] = 0;
                    npo = po + 6 - hl; npl = pl - 6 + hl;
                } else {                                               /* 802.3: LLC follows the length */
                    npo = po + 14 - hl; npl = pl - 14 + hl;
                }
                va[r + 16] = 0;                        /* 8797 RxPD flags (bits 2, 3 read by MLAN): none */
                va[r + 17] = (h[1] & 0x40) ? 0 : 1;   /* 8797 RxPD byte 17: frame arrived unencrypted */
                RtlMoveMemory(va + r + npo, h, hl);
                va[r + npo + 1] &= ~0x40;              /* decrypted by the firmware: no Protected bit */
                va[r + 2] = (UCHAR)npl; va[r + 3] = (UCHAR)(npl >> 8);
                va[r + 4] = (UCHAR)npo; va[r + 5] = (UCHAR)(npo >> 8);
                va[r + 6] = 0;
                if (InterlockedIncrement(&gGsRxShimLog) <= 20)
                    DbgPrint("goyasdio: f%u rx 802.11 frame rebuilt: fc %02x%02x header %u, offset %u -> %u, length %u -> %u\n",
                             p->Fn, va[r + npo], va[r + npo + 1], hl, po, npo, pl, npl);
            } else if (InterlockedIncrement(&gGsRxShimLog) <= 20) {
                DbgPrint("goyasdio: f%u rx type 2 packet left alone: fc %02x%02x offset %u length %u\n", p->Fn, h[0], h[1], po, pl);
            }
        }
        off += (plen + 255) & ~255UL;
    }
}

/* Data to send (function 1 data ports, write): le16 length, le16 type 0, then the TxPD: u8 bss_type, u8 bss_num,
   le16 tx_pkt_length, le16 tx_pkt_offset, le16 tx_pkt_type, le32 tx_control, u8 priority, u8 flags, u8 delay,
   u8 [15]. In Native Wi-Fi mode goyawifi sends the 802.11 frame from Windows and sets TxPD byte 15 to 1 for the
   8797 Windows firmware; the 8777 FP66 firmware sends Ethernet frames. Each 802.11 data frame with an LLC/SNAP
   header becomes DA, SA, type + payload in place (ending where it ended), byte 15 becomes 0. */
static volatile LONG gGsTxShimLog;

static VOID GsTxDataShim(PGS_PDO p, ULONG arg, PUCHAR va, ULONG len)
{
    ULONG off = 0, plen, t, po, pl, hl, f, e, i;
    UCHAR da[6], sa[6];
    if (!(arg >> 31) || ((arg >> 28) & 7) != 1 || ((arg >> 9) & 0x1FFFF) <= 0x10000) return;
    while (off + 20 <= len) {
        plen = (ULONG)(va[off] | (va[off + 1] << 8));
        if (plen < 20 || plen > len - off) break;
        t = off + 4;                                   /* TxPD */
        pl = (ULONG)(va[t + 2] | (va[t + 3] << 8));
        po = (ULONG)(va[t + 4] | (va[t + 5] << 8));
        f = t + po;
        if (va[off + 2] == 0 && va[off + 3] == 0 && va[t + 15] == 1 && po >= 16 && pl >= 24 + 8 && f + pl <= off + plen) {
            hl = 24 + ((va[f + 1] & 3) == 3 ? 6 : 0) + ((va[f] & 0x80) ? 2 : 0) + (((va[f] & 0x80) && (va[f + 1] & 0x80)) ? 4 : 0);
            if ((va[f] & 0x0C) == 0x08 && (va[f + 1] & 3) != 3 && pl >= hl + 8 &&
                va[f + hl] == 0xAA && va[f + hl + 1] == 0xAA && va[f + hl + 2] == 0x03 &&
                va[f + hl + 3] == 0 && va[f + hl + 4] == 0 && va[f + hl + 5] == 0) {
                for (i = 0; i < 6; i++) {
                    da[i] = va[f + ((va[f + 1] & 1) ? 16 : 4) + i];             /* ToDS: DA is address 3 */
                    sa[i] = va[f + ((va[f + 1] & 2) ? 16 : 10) + i];
                }
                e = f + hl - 6;                                              /* the type stays where it is */
                RtlCopyMemory(va + e, da, 6);
                RtlCopyMemory(va + e + 6, sa, 6);
                po += hl - 6; pl -= hl - 6;
                va[t + 2] = (UCHAR)pl; va[t + 3] = (UCHAR)(pl >> 8);
                va[t + 4] = (UCHAR)po; va[t + 5] = (UCHAR)(po >> 8);
                va[t + 15] = 0;
                if (InterlockedIncrement(&gGsTxShimLog) <= 20)
                    DbgPrint("goyasdio: f%u tx 802.11 frame -> Ethernet: type %02x%02x, offset %u, length %u\n",
                             p->Fn, va[e + 12], va[e + 13], po, pl);
            } else if (InterlockedIncrement(&gGsTxShimLog) <= 20) {
                DbgPrint("goyasdio: f%u tx 802.11 frame left alone: fc %02x%02x length %u\n", p->Fn, va[f], va[f + 1], pl);
            }
        }
        off += (plen + 255) & ~255UL;
    }
}

static NTSTATUS GsDeviceCommand(PGS_PDO p, PGS_REQUEST_PACKET r)
{
    PGS_FDO f = p->Fdo;
    ULONG arg = r->Parameters.DeviceCommand.Argument, r5 = 0, fn;
    UCHAR cmd = r->Parameters.DeviceCommand.CmdDesc.Cmd;
    SD_STATUS st;

    if (cmd == 52) {
        st = SdioCmd52Arg(&f->Sd, arg, &r5);
    } else if (cmd == 53) {
        ULONG len = r->Parameters.DeviceCommand.Length;
        PMDL mdl = r->Parameters.DeviceCommand.Mdl;
        PUCHAR va;
        if (!mdl || !len || MmGetMdlByteCount(mdl) < len) return STATUS_INVALID_PARAMETER;
        va = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        if (!va) return STATUS_INSUFFICIENT_RESOURCES;
        fn = (arg >> 28) & 7;
        GsTxDataShim(p, arg, va, len);
        st = SdioCmd53(&f->Sd, arg, va, len, f->Sd.FnBlk[fn], &r5);
        if (st == SD_OK) {
            r->Information = len;
            GsCmdRespShim(p, arg, va, len);
            GsRxDataShim(p, arg, va, len);
        }
    } else {
        DbgPrint("goyasdio: CMD%u from function %u not supported\n", cmd, p->Fn);
        return STATUS_NOT_SUPPORTED;
    }
    RtlCopyMemory(r->ResponseData, &r5, 4);
    r->ResponseLength = 4;
    return GsStatus(st);
}

static NTSTATUS GsProcess(PGS_PDO p, PGS_REQUEST_PACKET r)
{
    PGS_FDO f = p->Fdo;
    NTSTATUS s;
    if (!f || p->Deleted || !f->CardOk) return STATUS_NO_SUCH_DEVICE;
    r->Information = 0;
    ExAcquireFastMutex(&f->BusLock);
    switch (r->RequestFunction) {
    case SDRF_GET_PROPERTY:
        s = GsGetProperty(p, r->Parameters.GetSetProperty.Property, r->Parameters.GetSetProperty.Buffer,
                          r->Parameters.GetSetProperty.Length, &r->Information);
        break;
    case SDRF_SET_PROPERTY:
        s = GsSetProperty(p, r->Parameters.GetSetProperty.Property, r->Parameters.GetSetProperty.Buffer,
                          r->Parameters.GetSetProperty.Length);
        break;
    case SDRF_DEVICE_COMMAND:
        s = GsDeviceCommand(p, r);
        break;
    default:
        s = STATUS_INVALID_PARAMETER;
    }
    ExReleaseFastMutex(&f->BusLock);
    f->Requests++;
    GsTraceReq(p, r, s);
    if (!NT_SUCCESS(s)) {
        f->ReqErrors++;
        if (f->ReqErrors <= 20)
            DbgPrint("goyasdio: request fn %u type %u (prop/cmd %u) failed %08x, R5 %08x int %08x\n", p->Fn, r->RequestFunction,
                     r->RequestFunction == SDRF_DEVICE_COMMAND ? r->Parameters.DeviceCommand.CmdDesc.Cmd : r->Parameters.GetSetProperty.Property,
                     s, f->Sd.LastR5, f->Sd.LastIntSts);
    }
    return s;
}

/* ---- interface ---- */

static PGS_PDO GsPdoFromCtx(PVOID ctx)
{
    GS_IFCTX *c = (GS_IFCTX *)ctx;
    if (!c || c->Signature != GS_SDBUS_SIGNATURE) return NULL;
    return CONTAINING_RECORD(c, GS_PDO, IfCtx);
}

static VOID GsIfRef(PVOID ctx)
{
    PGS_PDO p = GsPdoFromCtx(ctx);
    if (p) InterlockedIncrement(&p->IfRefs);
}

static VOID GsIfDeref(PVOID ctx)
{
    PGS_PDO p = GsPdoFromCtx(ctx);
    if (p) InterlockedDecrement(&p->IfRefs);
}

static NTSTATUS GsIfInitialize(PVOID ctx, PGS_INTERFACE_PARAMETERS ip)
{
    PGS_PDO p = GsPdoFromCtx(ctx);
    PGS_FDO f;
    if (!p || !ip || ip->Size < sizeof(GS_INTERFACE_PARAMETERS)) return STATUS_INVALID_PARAMETER;
    f = p->Fdo;
    p->CallbackCtx = ip->CallbackRoutineContext;
    p->CallbackAtDpc = ip->CallbackAtDpcLevel;
    p->Callback = ip->DeviceGeneratesInterrupts ? ip->CallbackRoutine : NULL;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) GsWifiLogAttach((PVOID)ip->CallbackRoutine);
    p->IntPending = FALSE;
    p->Forward = TRUE;
    if (ip->DeviceGeneratesInterrupts && KeGetCurrentIrql() == PASSIVE_LEVEL && f && f->CardOk) {
        /* the Windows SD bus enables the function's card interrupt when the driver registers a callback */
        ExAcquireFastMutex(&f->BusLock);
        if (SdioFnIntEnable(&f->Sd, p->Fn, TRUE) == SD_OK) p->IntOn = TRUE;
        ExReleaseFastMutex(&f->BusLock);
    }
    DbgPrint("goyasdio: function %u interface initialized, interrupts %u (%s level), IEN %u\n", p->Fn,
             ip->DeviceGeneratesInterrupts, ip->CallbackAtDpcLevel ? "dispatch" : "passive", p->IntOn);
    return STATUS_SUCCESS;
}

static NTSTATUS GsIfAck(PVOID ctx)
{
    PGS_PDO p = GsPdoFromCtx(ctx);
    if (!p) return STATUS_INVALID_PARAMETER;
    if (InterlockedIncrement(&gTraceInts) <= 60) DbgPrint("goyasdio: f%u acknowledge interrupt\n", p->Fn);
    p->IntPending = FALSE;                         /* the poll thread re-enables the status on its next tick */
    return STATUS_SUCCESS;
}

/* ---- card interrupt poll thread ---- */

static VOID GsPollThread(PVOID ctx)
{
    PGS_FDO f = (PGS_FDO)ctx;
    LARGE_INTEGER t;
    ULONG tick = 0, fn, nfwd;
    PGS_PDO fire, cand;

    ExSetTimerResolution(10000, TRUE);
    f->TimerRes = TRUE;
    for (;;) {
        PGS_PDO active = NULL;
        nfwd = 0;
        for (fn = 1; fn <= SDIO_MAX_FN; fn++) {
            cand = f->Child[fn];
            if (cand && cand->Started && cand->Callback && cand->IntOn && cand->Forward) {
                nfwd++;
                if (!cand->IntPending && !active) active = cand;
            }
        }
        t.QuadPart = nfwd ? -10000LL : -1000000LL;  /* 1 ms while a function waits for interrupts, else 100 ms */
        if (KeWaitForSingleObject(&f->StopEvt, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT) break;
        if ((++tick & 1023) == 0) GsTraceStats(f);
        if (!active || !f->CardOk) continue;
        if (!ExTryToAcquireFastMutex(&f->BusLock)) continue;       /* a request is running: next tick */
        fire = NULL;
        if (!f->Sd.CardIntOn) SdioCardIntStatus(&f->Sd, TRUE);
        if (SdioCardIntAsserted(&f->Sd)) {
            fire = active;
            if (nfwd > 1) {                        /* several functions: ask the card which one */
                UCHAR pend = 0;
                if (SdioRead8(&f->Sd, 0, CCCR_INTPEND, &pend) == SD_OK) {
                    fire = NULL;
                    for (fn = 1; fn <= SDIO_MAX_FN && !fire; fn++)
                        if ((pend & (1 << fn)) && f->Child[fn] && f->Child[fn]->Callback && !f->Child[fn]->IntPending) fire = f->Child[fn];
                }
            }
            if (fire) {
                SdioCardIntStatus(&f->Sd, FALSE);  /* masked until the driver acknowledges */
                fire->IntPending = TRUE;
                fire->Ints++;
                f->CardInts++;
            }
        }
        ExReleaseFastMutex(&f->BusLock);
        if (fire) {
            GS_SDBUS_CALLBACK cb = fire->Callback;
            if (InterlockedIncrement(&gTraceInts) <= 60)
                DbgPrint("goyasdio: f%u card interrupt -> callback (%s)\n", fire->Fn, fire->CallbackAtDpc ? "dispatch" : "passive");
            if (cb) {
                if (fire->CallbackAtDpc) {
                    KIRQL old;
                    KeRaiseIrql(DISPATCH_LEVEL, &old);
                    cb(fire->CallbackCtx, 0);
                    KeLowerIrql(old);
                } else {
                    cb(fire->CallbackCtx, 0);
                }
            }
        }
    }
    ExSetTimerResolution(0, FALSE);
    f->TimerRes = FALSE;
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID GsStartThread(PGS_FDO f)
{
    HANDLE h;
    if (f->Thread) return;
    KeClearEvent(&f->StopEvt);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, GsPollThread, f))) {
        ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&f->Thread, NULL);
        ZwClose(h);
    }
}

static VOID GsStopThread(PGS_FDO f)
{
    if (!f->Thread) return;
    KeSetEvent(&f->StopEvt, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(f->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(f->Thread);
    f->Thread = NULL;
}

/* ---- PDO ---- */

static NTSTATUS GsCreatePdo(PGS_FDO f, ULONG fn)
{
    PDEVICE_OBJECT d;
    PGS_PDO p;
    NTSTATUS s = IoCreateDevice(gDriver, sizeof(GS_PDO), NULL, FILE_DEVICE_UNKNOWN,
                                FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN, FALSE, &d);
    if (!NT_SUCCESS(s)) return s;
    p = (PGS_PDO)d->DeviceExtension;
    RtlZeroMemory(p, sizeof(*p));
    p->Common.IsFdo = FALSE;
    p->IfCtx.Signature = GS_SDBUS_SIGNATURE;
    p->IfCtx.Target = d;
    p->Fdo = f;
    p->Self = d;
    p->Fn = fn;
    p->Forward = TRUE;
    d->Flags |= DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    f->Child[fn] = p;
    return STATUS_SUCCESS;
}

static VOID GsPdoStop(PGS_PDO p)
{
    PGS_FDO f = p->Fdo;
    p->Callback = NULL;
    p->IntPending = FALSE;
    if (p->Started && f && f->CardOk && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        ExAcquireFastMutex(&f->BusLock);
        (VOID)SdioFnIntEnable(&f->Sd, p->Fn, FALSE);
        (VOID)SdioEnableFunction(&f->Sd, p->Fn, FALSE);
        ExReleaseFastMutex(&f->BusLock);
    }
    p->IntOn = FALSE;
    p->Started = FALSE;
}

static NTSTATUS GsPdoPnp(PGS_PDO p, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS s = irp->IoStatus.Status;
    PGS_FDO f = p->Fdo;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        s = STATUS_SUCCESS;
        if (f && f->CardOk) {
            SD_STATUS st;
            ExAcquireFastMutex(&f->BusLock);
            st = SdioEnableFunction(&f->Sd, p->Fn, TRUE);
            if (st == SD_OK && !f->Sd.FnBlk[p->Fn]) st = SdioSetBlockLen(&f->Sd, p->Fn, 64);
            ExReleaseFastMutex(&f->BusLock);
            if (st != SD_OK) { DbgPrint("goyasdio: function %u did not come ready (%d)\n", p->Fn, st); s = STATUS_DEVICE_NOT_READY; }
        }
        if (NT_SUCCESS(s)) p->Started = TRUE;
        break;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        GsPdoStop(p);
        s = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        GsPdoStop(p);
        s = STATUS_SUCCESS;
        if (!p->Reported || !f) {                  /* no longer on the bus: delete now (else at the parent's removal) */
            if (f && f->Child[p->Fn] == p) f->Child[p->Fn] = NULL;
            p->Deleted = TRUE;
            irp->IoStatus.Status = s;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            IoDeleteDevice(p->Self);
            return s;
        }
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        s = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_ID: {
        PWCHAR id = NULL;
        NTSTATUS q = GsQueryId(p, sp->Parameters.QueryId.IdType, &id);
        if (q != STATUS_NOT_SUPPORTED) { s = q; if (NT_SUCCESS(q)) irp->IoStatus.Information = (ULONG_PTR)id; }
        break;
    }
    case IRP_MN_QUERY_DEVICE_TEXT: {
        PWCHAR b, q;
        DEVICE_TEXT_TYPE tt = sp->Parameters.QueryDeviceText.DeviceTextType;
        if (tt != DeviceTextDescription && tt != DeviceTextLocationInformation) break;
        b = (PWCHAR)ExAllocatePoolWithTag(PagedPool, 96 * sizeof(WCHAR), GS_TAG);
        if (!b) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        RtlZeroMemory(b, 96 * sizeof(WCHAR));
        q = GsStr(b, tt == DeviceTextDescription ? L"SDIO function " : L"SDH2 SDIO function ");
        *q++ = (WCHAR)(L'0' + p->Fn);
        if (tt == DeviceTextDescription && f) { q = GsStr(q, L" ("); q = GsHex4(q, f->Sd.FnVid[p->Fn]); *q++ = L':'; q = GsHex4(q, f->Sd.FnPid[p->Fn]); *q++ = L')'; }
        irp->IoStatus.Information = (ULONG_PTR)b;
        s = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES c = sp->Parameters.DeviceCapabilities.Capabilities;
        ULONG i;
        if (!c || c->Version != 1 || c->Size < sizeof(DEVICE_CAPABILITIES)) { s = STATUS_UNSUCCESSFUL; break; }
        c->LockSupported = FALSE; c->EjectSupported = FALSE; c->Removable = FALSE; c->DockDevice = FALSE;
        c->UniqueID = FALSE; c->SilentInstall = TRUE; c->RawDeviceOK = FALSE; c->SurpriseRemovalOK = TRUE;
        c->DeviceD1 = FALSE; c->DeviceD2 = FALSE;
        c->WakeFromD0 = FALSE; c->WakeFromD1 = FALSE; c->WakeFromD2 = FALSE; c->WakeFromD3 = FALSE;
        c->Address = p->Fn; c->UINumber = p->Fn;
        c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++) c->DeviceState[i] = PowerDeviceD3;
        c->SystemWake = PowerSystemUnspecified; c->DeviceWake = PowerDeviceUnspecified;
        c->D1Latency = c->D2Latency = c->D3Latency = 0;
        s = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS r = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), GS_TAG);
            if (!r) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
            r->Count = 1; r->Objects[0] = p->Self;
            ObReferenceObject(p->Self);
            irp->IoStatus.Information = (ULONG_PTR)r;
            s = STATUS_SUCCESS;
        }
        break;
    case IRP_MN_QUERY_INTERFACE:
        if (IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType, &GS_GUID_SDBUS_INTERFACE_STANDARD)) {
            PGS_INTERFACE_STANDARD i = (PGS_INTERFACE_STANDARD)sp->Parameters.QueryInterface.Interface;
            if (!i || sp->Parameters.QueryInterface.Size < sizeof(GS_INTERFACE_STANDARD)) { s = STATUS_INVALID_PARAMETER; break; }
            i->Size = sizeof(GS_INTERFACE_STANDARD);
            i->Version = sp->Parameters.QueryInterface.Version;
            i->Context = &p->IfCtx;
            i->InterfaceReference = GsIfRef;
            i->InterfaceDereference = GsIfDeref;
            i->InitializeInterface = GsIfInitialize;
            i->AcknowledgeInterrupt = GsIfAck;
            GsIfRef(&p->IfCtx);
            DbgPrint("goyasdio: SD bus interface (version %x) handed to function %u\n", i->Version, p->Fn);
            s = STATUS_SUCCESS;
        } else if (InterlockedIncrement(&gTraceOther) <= 40) {
            const GUID *g = sp->Parameters.QueryInterface.InterfaceType;
            DbgPrint("goyasdio: f%u query interface {%08x-%04x-%04x-%02x%02x-...} size %u version %u: not ours\n", p->Fn,
                     g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], sp->Parameters.QueryInterface.Size,
                     sp->Parameters.QueryInterface.Version);
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION b = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION), GS_TAG);
        if (!b) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        b->BusTypeGuid = GS_GUID_BUS_TYPE_SD;
        b->LegacyBusType = InterfaceTypeUndefined;
        b->BusNumber = 0;
        irp->IoStatus.Information = (ULONG_PTR)b;
        s = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        s = STATUS_SUCCESS;
        break;
    default:                                       /* resources, usage notifications ...: status unchanged */
        if (InterlockedIncrement(&gTraceOther) <= 40)
            DbgPrint("goyasdio: f%u PnP minor %02x not handled (status %08x)\n", p->Fn, sp->MinorFunction, s);
        break;
    }
    if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE || sp->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
        GsWifiLogDump();                           /* the function driver may be unloaded after this */
        gWifiLog = NULL;
    }
    if (sp->MinorFunction == IRP_MN_START_DEVICE || sp->MinorFunction == IRP_MN_STOP_DEVICE ||
        sp->MinorFunction == IRP_MN_REMOVE_DEVICE || sp->MinorFunction == IRP_MN_SURPRISE_REMOVAL)
        DbgPrint("goyasdio: f%u PnP minor %02x -> %08x (%d download-loop requests not shown, %u CMD53, %u bus errors)\n",
                 p->Fn, sp->MinorFunction, s, InterlockedExchange(&gTraceSkipped, 0), f ? f->Sd.Cmd53s : 0, f ? f->Sd.Errors : 0);
    irp->IoStatus.Status = s;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}

static NTSTATUS GsPdoPower(PGS_PDO p, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS s = irp->IoStatus.Status;
    UNREFERENCED_PARAMETER(p);
    switch (sp->MinorFunction) {
    case IRP_MN_SET_POWER:
        if (sp->Parameters.Power.Type == DevicePowerState) PoSetPowerState(p->Self, DevicePowerState, sp->Parameters.Power.State);
        s = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_POWER:
        s = STATUS_SUCCESS;
        break;
    case IRP_MN_WAIT_WAKE:
        s = STATUS_NOT_SUPPORTED;
        break;
    default:
        break;
    }
    PoStartNextPowerIrp(irp);
    irp->IoStatus.Status = s;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}

static VOID GsWorker(PDEVICE_OBJECT d, PVOID ctx)
{
    PIRP irp = (PIRP)ctx;
    PIO_WORKITEM wi = (PIO_WORKITEM)irp->Tail.Overlay.DriverContext[0];
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    PGS_PDO p = (PGS_PDO)d->DeviceExtension;
    PGS_REQUEST_PACKET r = (PGS_REQUEST_PACKET)sp->Parameters.DeviceIoControl.Type3InputBuffer;
    NTSTATUS s;
    GT_GUARD(0x21, s = GsProcess(p, r), s = STATUS_UNSUCCESSFUL);
    IoFreeWorkItem(wi);
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = r->Information;    /* sdbus.lib copies it into the packet when the request pended */
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS GsPdoInternalIoctl(PGS_PDO p, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    PGS_REQUEST_PACKET r = (PGS_REQUEST_PACKET)sp->Parameters.DeviceIoControl.Type3InputBuffer;
    NTSTATUS s;

    if (sp->Parameters.DeviceIoControl.IoControlCode != GS_IOCTL_SD_SUBMIT_REQUEST || !r ||
        sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(GS_REQUEST_PACKET)) {
        if (InterlockedIncrement(&gTraceOther) <= 40)
            DbgPrint("goyasdio: f%u rejected internal IOCTL %08x input %p len %u (want %08x, len >= %u)\n", p->Fn,
                     sp->Parameters.DeviceIoControl.IoControlCode, r, sp->Parameters.DeviceIoControl.InputBufferLength,
                     GS_IOCTL_SD_SUBMIT_REQUEST, (ULONG)sizeof(GS_REQUEST_PACKET));
        s = STATUS_INVALID_DEVICE_REQUEST;
    } else if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        /* asynchronous submit from DISPATCH_LEVEL: do it in a work item */
        PIO_WORKITEM wi = IoAllocateWorkItem(p->Self);
        if (!wi) { s = STATUS_INSUFFICIENT_RESOURCES; }
        else {
            irp->Tail.Overlay.DriverContext[0] = wi;
            IoMarkIrpPending(irp);
            IoQueueWorkItem(wi, GsWorker, DelayedWorkQueue, irp);
            return STATUS_PENDING;
        }
    } else {
        GT_GUARD(0x20, s = GsProcess(p, r), s = STATUS_UNSUCCESSFUL);
    }
    irp->IoStatus.Status = s;
    irp->IoStatus.Information = (s != STATUS_INVALID_DEVICE_REQUEST && r) ? r->Information : 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}

/* ---- FDO ---- */

static NTSTATUS GsSyncCompletion(PDEVICE_OBJECT d, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(d);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS GsForwardSync(PGS_FDO f, PIRP irp)
{
    KEVENT e;
    NTSTATUS s;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, GsSyncCompletion, &e, TRUE, TRUE, TRUE);
    s = IoCallDriver(f->Lower, irp);
    if (s == STATUS_PENDING) {
        KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, NULL);
        s = irp->IoStatus.Status;
    }
    return s;
}

static NTSTATUS GsPassDown(PGS_FDO f, PIRP irp)
{
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(f->Lower, irp);
}

static VOID GsUnmap(PGS_FDO f)
{
    if (f->Sd.Regs) { MmUnmapIoSpace((PVOID)f->Sd.Regs, f->RegLen); f->Sd.Regs = NULL; }
    if (f->ApmuClk) { MmUnmapIoSpace((PVOID)f->ApmuClk, 4); f->ApmuClk = NULL; }
    if (f->Gpio1) { MmUnmapIoSpace((PVOID)f->Gpio1, 0x64); f->Gpio1 = NULL; }
    if (f->Mfpr) { MmUnmapIoSpace((PVOID)f->Mfpr, 0x200); f->Mfpr = NULL; }
}

static NTSTATUS GsFdoStart(PGS_FDO f, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    PCM_RESOURCE_LIST rl = sp->Parameters.StartDevice.AllocatedResourcesTranslated;
    ULONG i;

    GoyaTraceStep(0x01);
    if (rl && rl->Count) {
        PCM_PARTIAL_RESOURCE_LIST pl = &rl->List[0].PartialResourceList;
        for (i = 0; i < pl->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[i];
            if (d->Type == CmResourceTypeMemory && !f->Sd.Regs && d->u.Memory.Length >= 0x100) {
                f->RegLen = d->u.Memory.Length;
                f->Sd.Regs = (volatile UCHAR *)MmMapIoSpace(d->u.Memory.Start, f->RegLen, MmNonCached);
            }
        }
    }
    if (!f->Sd.Regs) {                             /* no resources from ACPI: the known address */
        f->RegLen = 0x800;
        f->Sd.Regs = (volatile UCHAR *)GsMap(SDH2_PHYS, f->RegLen);
    }
    if (!f->Sd.Regs) return STATUS_INSUFFICIENT_RESOURCES;
    f->Sd.Os = f;
    /* also used by goyasdhc/goyagpio/the firmware: mapped here without claiming them as resources */
    f->ApmuClk = (volatile ULONG *)GsMap(APMU_SDH1_PHYS, 4);
    f->Gpio1 = (volatile UCHAR *)GsMap(GPIO_BANK1_PHYS, 0x64);
    f->Mfpr = (volatile UCHAR *)GsMap(MFPR_PHYS, 0x200);

    f->CardOk = GsInitCard(f);
    f->Started = TRUE;
    if (f->CardOk) GsStartThread(f);
    GoyaTraceStep(0x02);
    return STATUS_SUCCESS;                         /* without a card the bus starts with no children */
}

static VOID GsFdoStop(PGS_FDO f)
{
    ULONG fn;
    GsStopThread(f);
    for (fn = 1; fn <= SDIO_MAX_FN; fn++) if (f->Child[fn]) GsPdoStop(f->Child[fn]);
    if (f->CardOk && f->Sd.Regs) {
        ExAcquireFastMutex(&f->BusLock);
        SD_WR16(&f->Sd, SDHC_CLKCTRL, 0);
        ExReleaseFastMutex(&f->BusLock);
    }
    f->CardOk = FALSE;
    f->Started = FALSE;
    GsUnmap(f);
}

static NTSTATUS GsBusRelations(PGS_FDO f, PIRP irp)
{
    PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)irp->IoStatus.Information, r;
    ULONG n = 0, fn, k = 0, oldn = old ? old->Count : 0;

    if (f->CardOk) {
        for (fn = 1; fn <= f->Sd.NumFuncs && fn <= SDIO_MAX_FN; fn++)
            if (!f->Child[fn] && f->Sd.FnVid[fn]) GsCreatePdo(f, fn);
    }
    for (fn = 1; fn <= SDIO_MAX_FN; fn++) {
        if (f->Child[fn]) { f->Child[fn]->Reported = f->CardOk; if (f->CardOk) n++; }
    }
    r = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + (oldn + n) * sizeof(PDEVICE_OBJECT), GS_TAG);
    if (!r) return STATUS_INSUFFICIENT_RESOURCES;
    for (k = 0; k < oldn; k++) r->Objects[k] = old->Objects[k];
    for (fn = 1; fn <= SDIO_MAX_FN; fn++) {
        if (f->Child[fn] && f->Child[fn]->Reported) { r->Objects[k++] = f->Child[fn]->Self; ObReferenceObject(f->Child[fn]->Self); }
    }
    r->Count = k;
    if (old) ExFreePool(old);
    irp->IoStatus.Information = (ULONG_PTR)r;
    return STATUS_SUCCESS;
}

static NTSTATUS GsFdoPnp(PGS_FDO f, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS s;
    ULONG fn;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        s = GsForwardSync(f, irp);
        if (NT_SUCCESS(s)) GT_GUARD(0x03, s = GsFdoStart(f, irp), s = STATUS_UNSUCCESSFUL);
        irp->IoStatus.Status = s;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return s;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type != BusRelations) break;
        s = GsBusRelations(f, irp);
        if (!NT_SUCCESS(s)) { irp->IoStatus.Status = s; IoCompleteRequest(irp, IO_NO_INCREMENT); return s; }
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        GsFdoStop(f);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE: {
        PDEVICE_OBJECT lower = f->Lower;
        GsFdoStop(f);
        for (fn = 1; fn <= SDIO_MAX_FN; fn++) {
            PGS_PDO p = f->Child[fn];
            if (p) { f->Child[fn] = NULL; p->Fdo = NULL; p->Deleted = TRUE; IoDeleteDevice(p->Self); }
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        s = IoCallDriver(lower, irp);
        IoDetachDevice(lower);
        IoDeleteDevice(f->Self);
        return s;
    }
    default:
        break;
    }
    return GsPassDown(f, irp);
}

/* ---- dispatch ---- */

static NTSTATUS GsDispatchPnp(PDEVICE_OBJECT d, PIRP irp)
{
    GS_COMMON *c = (GS_COMMON *)d->DeviceExtension;
    return c->IsFdo ? GsFdoPnp((PGS_FDO)c, irp) : GsPdoPnp((PGS_PDO)c, irp);
}

static NTSTATUS GsDispatchPower(PDEVICE_OBJECT d, PIRP irp)
{
    GS_COMMON *c = (GS_COMMON *)d->DeviceExtension;
    if (c->IsFdo) {
        PoStartNextPowerIrp(irp);
        IoSkipCurrentIrpStackLocation(irp);
        return PoCallDriver(((PGS_FDO)c)->Lower, irp);
    }
    return GsPdoPower((PGS_PDO)c, irp);
}

static NTSTATUS GsDispatchInternal(PDEVICE_OBJECT d, PIRP irp)
{
    GS_COMMON *c = (GS_COMMON *)d->DeviceExtension;
    NTSTATUS s;
    if (c->IsFdo) return GsPassDown((PGS_FDO)c, irp);
    if (((PGS_PDO)c)->Deleted) s = STATUS_NO_SUCH_DEVICE;
    else return GsPdoInternalIoctl((PGS_PDO)c, irp);
    irp->IoStatus.Status = s;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}

static NTSTATUS GsDispatchOther(PDEVICE_OBJECT d, PIRP irp)
{
    GS_COMMON *c = (GS_COMMON *)d->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS s;
    if (c->IsFdo && sp->MajorFunction == IRP_MJ_SYSTEM_CONTROL) return GsPassDown((PGS_FDO)c, irp);
    s = (sp->MajorFunction == IRP_MJ_CREATE || sp->MajorFunction == IRP_MJ_CLOSE || sp->MajorFunction == IRP_MJ_CLEANUP)
        ? STATUS_SUCCESS : (sp->MajorFunction == IRP_MJ_SYSTEM_CONTROL ? irp->IoStatus.Status : STATUS_INVALID_DEVICE_REQUEST);
    if (s == STATUS_INVALID_DEVICE_REQUEST && InterlockedIncrement(&gTraceOther) <= 40)
        DbgPrint("goyasdio: %s got IRP major %02x minor %02x: rejected\n", c->IsFdo ? "bus" : "function", sp->MajorFunction, sp->MinorFunction);
    irp->IoStatus.Status = s;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}

static NTSTATUS GsAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT d;
    PGS_FDO f;
    NTSTATUS s = IoCreateDevice(drv, sizeof(GS_FDO), NULL, FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN, FALSE, &d);
    if (!NT_SUCCESS(s)) return s;
    f = (PGS_FDO)d->DeviceExtension;
    RtlZeroMemory(f, sizeof(*f));
    f->Common.IsFdo = TRUE;
    f->Self = d;
    f->Pdo = pdo;
    ExInitializeFastMutex(&f->BusLock);
    KeInitializeEvent(&f->StopEvt, NotificationEvent, FALSE);
    f->Lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!f->Lower) { IoDeleteDevice(d); return STATUS_NO_SUCH_DEVICE; }
    d->Flags |= DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    GoyaTraceStep(0x00);
    return STATUS_SUCCESS;
}

static VOID GsUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    ULONG i;
    UNREFERENCED_PARAMETER(reg);
    gDriver = drv;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = GsDispatchOther;
    drv->MajorFunction[IRP_MJ_PNP] = GsDispatchPnp;
    drv->MajorFunction[IRP_MJ_POWER] = GsDispatchPower;
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = GsDispatchInternal;
    drv->DriverExtension->AddDevice = GsAddDevice;
    drv->DriverUnload = GsUnload;
    DbgPrint("goyasdio: loaded\n");
    return STATUS_SUCCESS;
}
