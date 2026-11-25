/* Battery and charging for the Galaxy Tab 3 Lite (SM-T110) - KMDF battery miniclass driver (battc.sys)
   for Windows 8.0 RT / 10.

   battcore.c does the work (gauge from the 88PM822 battery voltage, thermistor, SMB328 charger kept
   programmed and its watchdog fed, RT8973A cable detection); this file is the Windows glue:
     - a passive-level timer runs BcPoll every 2 s while the device is in D0;
     - the results are published under a wait lock and reported through the battery class
       (BatteryClassInitializeDevice / BatteryClassIoctl / BatteryClassStatusNotify), which gives Windows
       the battery icon, the percentage, "plugged in / charging" and the low-battery actions.

   No ACPI resources (Dsdt.asl BAT0, HID MRVL000A): the register blocks are mapped directly, like
   goyakeys does with the GPIO block. TWSI2 is not claimed by any other Windows driver (I2C2 is hidden);
   the firmware runtime services use it only to reset / power off. The GPIO direction set/clear
   registers keep this driver from read-modify-writing registers other drivers may use. */
#define GOYA_TRACE_ID 2
#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include "..\common\goyatrace.h"

#define GB_TAG 'bBoG'

#define GPIO_PHYS   0xD4019000ULL
#define GPIO_LEN    0x200
#define MFPR_PHYS   0xD401E000ULL
#define MFPR_LEN    0x300
#define TWSI2_PHYS  0xD4037000ULL
#define TWSI2_LEN   0x40
#define APBCP_PHYS  0xD403B000ULL
#define APBCP_LEN   0x40
#define APBCP_TWSI2 0x28

/* ---- battcore glue ---- */
#define BC_HOST_OS_FIELDS  PUCHAR Gpio, Mfpr, Twsi, Apbc; KIRQL OldIrql; ULONG LockDepth;
#define BC_GPIO_RD(c, o)      READ_REGISTER_ULONG((volatile ULONG *)((c)->Gpio + (o)))
#define BC_GPIO_WR(c, o, v)   WRITE_REGISTER_ULONG((volatile ULONG *)((c)->Gpio + (o)), (ULONG)(v))
#define BC_MFPR_RD(c, o)      READ_REGISTER_ULONG((volatile ULONG *)((c)->Mfpr + (o)))
#define BC_MFPR_WR(c, o, v)   WRITE_REGISTER_ULONG((volatile ULONG *)((c)->Mfpr + (o)), (ULONG)(v))
#define BC_TW_RD(c, o)        READ_REGISTER_ULONG((volatile ULONG *)((c)->Twsi + (o)))
#define BC_TW_WR(c, o, v)     WRITE_REGISTER_ULONG((volatile ULONG *)((c)->Twsi + (o)), (ULONG)(v))
#define BC_APBC_RD(c)         READ_REGISTER_ULONG((volatile ULONG *)((c)->Apbc + APBCP_TWSI2))
#define BC_APBC_WR(c, v)      WRITE_REGISTER_ULONG((volatile ULONG *)((c)->Apbc + APBCP_TWSI2), (ULONG)(v))
#define BC_UDELAY(us)         KeStallExecutionProcessor(us)
#define BC_MSLEEP(c, ms)      GbSleepMs(ms)
#define BC_NOW_MS(c)          ((ULONG)(KeQueryInterruptTime() / 10000))
#define BC_LOG(...)           DbgPrint(__VA_ARGS__)
/* one bus transaction (<= ~1 ms) at DISPATCH_LEVEL, so a bit-banged clock is never stretched by a
   thread switch */
#define BC_LOCK(c)            do { if ((c)->LockDepth++ == 0) KeRaiseIrql(DISPATCH_LEVEL, &(c)->OldIrql); } while (0)
#define BC_UNLOCK(c)          do { if (--(c)->LockDepth == 0) KeLowerIrql((c)->OldIrql); } while (0)

static VOID GbSleepMs(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

#include "battcore.c"

/* what the battery class sees, updated after every poll */
typedef struct _GB_PUBLISHED {
    BOOLEAN Ready;              /* gauge seeded: the battery "exists" from now on */
    ULONG   PowerState;
    ULONG   CapacityMwh;
    ULONG   VoltageMv;          /* BATTERY_UNKNOWN_VOLTAGE when the PMIC did not answer */
    BOOLEAN TempOk;
    LONG    TempDeci;
} GB_PUBLISHED;

typedef struct _GOYA_BATT {
    WDFDEVICE    Device;
    WDFTIMER     Timer;
    BOOLEAN      Running;
    BC_HOST      Hw;
    BOOLEAN      HwOk;
    WDFWAITLOCK  Lock;          /* Pub (taken by the battery class callbacks) */
    GB_PUBLISHED Pub;
    WDFWAITLOCK  ClassLock;     /* ClassHandle. Lock may be taken while ClassLock is held (the class
                                   calls GbQuery* from inside BatteryClassIoctl), never the other way */
    PVOID        ClassHandle;
    ULONG        Notifies;
} GOYA_BATT, *PGOYA_BATT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GOYA_BATT, GetBatt)

#define GB_TAG_VALUE  1

/* ================= battery class callbacks (PASSIVE_LEVEL) ================= */

static NTSTATUS GbQueryTag(PVOID Context, PULONG BatteryTag)
{
    PGOYA_BATT B = (PGOYA_BATT)Context;
    BOOLEAN ready;
    WdfWaitLockAcquire(B->Lock, NULL);
    ready = B->Pub.Ready;
    WdfWaitLockRelease(B->Lock);
    *BatteryTag = ready ? GB_TAG_VALUE : BATTERY_TAG_INVALID;
    return ready ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS GbCopy(PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength, const VOID *Data, ULONG Size)
{
    *ReturnedLength = Size;
    if (BufferLength < Size) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buffer, Data, Size);
    return STATUS_SUCCESS;
}

static NTSTATUS GbQueryInformation(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level, LONG AtRate,
                                   PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    PGOYA_BATT B = (PGOYA_BATT)Context;
    static const WCHAR name[] = L"Galaxy Tab 3 Lite battery";
    static const WCHAR maker[] = L"Samsung";
    static const WCHAR id[] = L"GOYA-EB-BT111";
    GB_PUBLISHED p;
    UNREFERENCED_PARAMETER(AtRate);

    *ReturnedLength = 0;
    if (BatteryTag != GB_TAG_VALUE) return STATUS_NO_SUCH_DEVICE;
    WdfWaitLockAcquire(B->Lock, NULL);
    p = B->Pub;
    WdfWaitLockRelease(B->Lock);

    switch (Level) {
    case BatteryInformation: {
        BATTERY_INFORMATION bi;
        RtlZeroMemory(&bi, sizeof(bi));
        bi.Capabilities = BATTERY_SYSTEM_BATTERY;
        bi.Technology = 1;                                   /* rechargeable */
        bi.Chemistry[0] = 'L'; bi.Chemistry[1] = 'I'; bi.Chemistry[2] = 'O'; bi.Chemistry[3] = 'N';
        bi.DesignedCapacity = BC_DESIGN_MWH;
        bi.FullChargedCapacity = BC_DESIGN_MWH;
        bi.DefaultAlert1 = BC_DESIGN_MWH * 7 / 100;          /* low */
        bi.DefaultAlert2 = BC_DESIGN_MWH * 4 / 100;          /* warning */
        bi.CriticalBias = 0;
        bi.CycleCount = 0;
        return GbCopy(Buffer, BufferLength, ReturnedLength, &bi, sizeof(bi));
    }
    case BatteryGranularityInformation: {
        BATTERY_REPORTING_SCALE s;
        s.Granularity = BC_DESIGN_MWH / 100;                 /* 1 % steps */
        s.Capacity = BC_DESIGN_MWH;
        return GbCopy(Buffer, BufferLength, ReturnedLength, &s, sizeof(s));
    }
    case BatteryTemperature: {
        ULONG k;
        if (!p.TempOk) return STATUS_INVALID_DEVICE_REQUEST;
        k = (ULONG)(p.TempDeci + 2732);                      /* 0.1 K */
        return GbCopy(Buffer, BufferLength, ReturnedLength, &k, sizeof(k));
    }
    case BatteryEstimatedTime: {
        ULONG t = BATTERY_UNKNOWN_TIME;                      /* no current measurement */
        return GbCopy(Buffer, BufferLength, ReturnedLength, &t, sizeof(t));
    }
    case BatteryDeviceName:
        return GbCopy(Buffer, BufferLength, ReturnedLength, name, sizeof(name));
    case BatteryManufactureName:
        return GbCopy(Buffer, BufferLength, ReturnedLength, maker, sizeof(maker));
    case BatteryUniqueID:
        return GbCopy(Buffer, BufferLength, ReturnedLength, id, sizeof(id));
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS GbQueryStatus(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS Status)
{
    PGOYA_BATT B = (PGOYA_BATT)Context;
    GB_PUBLISHED p;
    if (BatteryTag != GB_TAG_VALUE) return STATUS_NO_SUCH_DEVICE;
    WdfWaitLockAcquire(B->Lock, NULL);
    p = B->Pub;
    WdfWaitLockRelease(B->Lock);
    if (!p.Ready) return STATUS_NO_SUCH_DEVICE;
    Status->PowerState = p.PowerState;
    Status->Capacity = p.CapacityMwh;
    Status->Voltage = p.VoltageMv;
    Status->Rate = BATTERY_UNKNOWN_RATE;
    return STATUS_SUCCESS;
}

static NTSTATUS GbSetStatusNotify(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY Notify)
{
    /* every change of power state or percentage is reported anyway (BatteryClassStatusNotify) */
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Notify);
    return BatteryTag == GB_TAG_VALUE ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS GbDisableStatusNotify(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

static NTSTATUS GbSetInformation(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    return STATUS_NOT_SUPPORTED;
}

/* ================= polling ================= */

static VOID OnTimer_(WDFTIMER Timer)
{
    PGOYA_BATT B = GetBatt((WDFDEVICE)WdfTimerGetParentObject(Timer));
    BC_HOST *c = &B->Hw;
    BOOLEAN changed;

    if (!B->Running || !B->HwOk) return;
    changed = BcPoll(c);

    WdfWaitLockAcquire(B->Lock, NULL);
    B->Pub.Ready = c->Seeded;
    B->Pub.PowerState = BcPowerState(c);
    B->Pub.CapacityMwh = BcCapacityMwh(c);
    B->Pub.VoltageMv = c->VbatOk ? c->VbatMv : BATTERY_UNKNOWN_VOLTAGE;
    B->Pub.TempOk = c->TempOk;
    B->Pub.TempDeci = c->TempDeci;
    WdfWaitLockRelease(B->Lock);

    if (changed && c->Seeded) {
        WdfWaitLockAcquire(B->ClassLock, NULL);
        if (B->ClassHandle != NULL) {
            B->Notifies++;
            BatteryClassStatusNotify(B->ClassHandle);
        }
        WdfWaitLockRelease(B->ClassLock);
    }

    if (B->Running) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(BC_POLL_MS));
}

static VOID OnTimer(WDFTIMER Timer)
{
    GT_GUARD(0x01, OnTimer_(Timer), ;);
}

/* ================= PnP / power ================= */

static PUCHAR GbMap(ULONGLONG phys, SIZE_T len)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)phys;
    return (PUCHAR)MmMapIoSpace(pa, len, MmNonCached);
}

static VOID GbUnmap(PGOYA_BATT B)
{
    if (B->Hw.Gpio) { MmUnmapIoSpace(B->Hw.Gpio, GPIO_LEN); B->Hw.Gpio = NULL; }
    if (B->Hw.Mfpr) { MmUnmapIoSpace(B->Hw.Mfpr, MFPR_LEN); B->Hw.Mfpr = NULL; }
    if (B->Hw.Twsi) { MmUnmapIoSpace(B->Hw.Twsi, TWSI2_LEN); B->Hw.Twsi = NULL; }
    if (B->Hw.Apbc) { MmUnmapIoSpace(B->Hw.Apbc, APBCP_LEN); B->Hw.Apbc = NULL; }
}

static NTSTATUS OnPrepareHardware_(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PGOYA_BATT B = GetBatt(Device);
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    B->Hw.Gpio = GbMap(GPIO_PHYS, GPIO_LEN);
    B->Hw.Mfpr = GbMap(MFPR_PHYS, MFPR_LEN);
    B->Hw.Twsi = GbMap(TWSI2_PHYS, TWSI2_LEN);
    B->Hw.Apbc = GbMap(APBCP_PHYS, APBCP_LEN);
    if (!B->Hw.Gpio || !B->Hw.Mfpr || !B->Hw.Twsi || !B->Hw.Apbc) {
        GbUnmap(B);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS OnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x02, r_ = OnPrepareHardware_(Device, Raw, Translated), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS OnReleaseHardware_(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    GbUnmap(GetBatt(Device));
    return STATUS_SUCCESS;
}

static NTSTATUS OnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    NTSTATUS r_ = STATUS_SUCCESS;
    GT_GUARD(0x03, r_ = OnReleaseHardware_(Device, Translated), r_ = STATUS_SUCCESS);
    return r_;
}

static NTSTATUS OnD0Entry_(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PGOYA_BATT B = GetBatt(Device);
    int rc;
    UNREFERENCED_PARAMETER(Prev);
    if (!B->HwOk) {
        rc = BcInit(&B->Hw);
        /* without the PMIC there is no gauge, but keeping the charger programmed still matters */
        if (rc != 0) DbgPrint("goyabatt: PMIC not answering (%d), charger only\n", rc);
        B->HwOk = TRUE;
    }
    B->Running = TRUE;
    WdfTimerStart(B->Timer, WDF_REL_TIMEOUT_IN_MS(10));
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x04, r_ = OnD0Entry_(Device, Prev), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS OnD0Exit_(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PGOYA_BATT B = GetBatt(Device);
    UNREFERENCED_PARAMETER(Target);
    B->Running = FALSE;
    WdfTimerStop(B->Timer, TRUE);
    BcLogState(&B->Hw, "stopping");
    return STATUS_SUCCESS;
}

static NTSTATUS OnD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    NTSTATUS r_ = STATUS_SUCCESS;
    GT_GUARD(0x05, r_ = OnD0Exit_(Device, Target), r_ = STATUS_SUCCESS);
    return r_;
}

static NTSTATUS OnSelfManagedIoInit_(WDFDEVICE Device)
{
    PGOYA_BATT B = GetBatt(Device);
    BATTERY_MINIPORT_INFO mi;
    PVOID handle = NULL;
    NTSTATUS st;

    RtlZeroMemory(&mi, sizeof(mi));
    mi.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    mi.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    mi.Context = B;
    mi.QueryTag = GbQueryTag;
    mi.QueryInformation = GbQueryInformation;
    mi.SetInformation = GbSetInformation;
    mi.QueryStatus = GbQueryStatus;
    mi.SetStatusNotify = GbSetStatusNotify;
    mi.DisableStatusNotify = GbDisableStatusNotify;
    mi.Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    mi.DeviceName = NULL;
    st = BatteryClassInitializeDevice(&mi, &handle);
    DbgPrint("goyabatt: battery class %08x\n", st);
    if (!NT_SUCCESS(st)) return st;
    WdfWaitLockAcquire(B->ClassLock, NULL);
    B->ClassHandle = handle;
    WdfWaitLockRelease(B->ClassLock);
    return STATUS_SUCCESS;
}

static NTSTATUS OnSelfManagedIoInit(WDFDEVICE Device)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x07, r_ = OnSelfManagedIoInit_(Device), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static VOID OnSelfManagedIoCleanup_(WDFDEVICE Device)
{
    PGOYA_BATT B = GetBatt(Device);
    WdfWaitLockAcquire(B->ClassLock, NULL);
    if (B->ClassHandle != NULL) BatteryClassUnload(B->ClassHandle);
    B->ClassHandle = NULL;
    WdfWaitLockRelease(B->ClassLock);
}

static VOID OnSelfManagedIoCleanup(WDFDEVICE Device)
{
    GT_GUARD(0x08, OnSelfManagedIoCleanup_(Device), ;);
}

/* IOCTL_BATTERY_* go to the battery class, everything else to the framework */
static NTSTATUS OnIoctlPreprocess_(WDFDEVICE Device, PIRP Irp)
{
    PGOYA_BATT B = GetBatt(Device);
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    WdfWaitLockAcquire(B->ClassLock, NULL);
    if (B->ClassHandle != NULL) st = BatteryClassIoctl(B->ClassHandle, Irp);
    WdfWaitLockRelease(B->ClassLock);
    if (st == STATUS_NOT_SUPPORTED) {
        IoSkipCurrentIrpStackLocation(Irp);
        st = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
    }
    return st;
}

static NTSTATUS OnIoctlPreprocess(WDFDEVICE Device, PIRP Irp)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x09, r_ = OnIoctlPreprocess_(Device, Irp), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS OnDeviceAdd_(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    PGOYA_BATT B;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = OnPrepareHardware;
    pnp.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnp.EvtDeviceD0Entry = OnD0Entry;
    pnp.EvtDeviceD0Exit = OnD0Exit;
    pnp.EvtDeviceSelfManagedIoInit = OnSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = OnSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    st = WdfDeviceInitAssignWdmIrpPreprocessCallback(Init, OnIoctlPreprocess, IRP_MJ_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(st)) return st;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, GOYA_BATT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    st = WdfDeviceCreate(&Init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;
    B = GetBatt(dev);
    B->Device = dev;

    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = dev;
    st = WdfWaitLockCreate(&a, &B->Lock);
    if (!NT_SUCCESS(st)) return st;
    st = WdfWaitLockCreate(&a, &B->ClassLock);
    if (!NT_SUCCESS(st)) return st;

    /* passive-level timers cannot be periodic: one-shot, re-armed by OnTimer */
    WDF_TIMER_CONFIG_INIT(&tc, OnTimer);
    tc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = dev;
    a.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&tc, &a, &B->Timer);
}

static NTSTATUS OnDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x06, r_ = OnDeviceAdd_(Driver, Init), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}

static NTSTATUS DriverEntry_(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, OnDeviceAdd);
    cfg.DriverPoolTag = GB_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS r_ = STATUS_UNSUCCESSFUL;
    GT_GUARD(0x00, r_ = DriverEntry_(DriverObject, RegistryPath), r_ = STATUS_UNSUCCESSFUL);
    return r_;
}
