/* Licensed under the MIT License.
   Derived from Microsoft imx-iotcore hals/halext/HalExtiMX6Timers.

Module Name:

    HalExtension.c

Abstract:

    HAL Extension Module for the Marvell PXA986/PXA988 APB timer 0.
    Registers two timers described by CSRT descriptors:
      - MMP_TIMER_TYPE_EVENT   (counter 0, GIC IRQ from the descriptor): the
        interrupt timer Windows uses for the clock tick.
      - MMP_TIMER_TYPE_COUNTER (counter 1): a free-running readable counter
        used as the performance counter.
    Cortex-A9 has no Generic Timer, so this module is what gives the Windows
    HAL its timers.

*/

#include <nthalext.h>
#include "HalExtCompat.h"
#include "MmpTimer.h"

//
// CSRT timer descriptor (must match GoyaWifiAcpiDxe.c)
//
#pragma pack(push, 1)
typedef struct _CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE {
    CSRT_RESOURCE_DESCRIPTOR_HEADER Header;
    UINT32 TimerType;
    UINT32 ClockSource;
    UINT32 Frequency;
    UINT32 FrequencyScale;
    UINT32 BaseAddress;
    UINT32 Interrupt;
} CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE, *PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE;
#pragma pack(pop)

typedef struct _MMP_TIMER_INTERNAL_DATA {
    PULONG BaseAddressPtr;      // mapped timer block
    ULONG  PeriodInTicks;       // non-zero: periodic emulation
} MMP_TIMER_INTERNAL_DATA, *PMMP_TIMER_INTERNAL_DATA;

static FORCEINLINE ULONG ReadReg(PVOID Base, ULONG Reg)
{
    return READ_REGISTER_NOFENCE_ULONG((ULONG *)((PUCHAR)Base + Reg));
}

static FORCEINLINE VOID WriteReg(PVOID Base, ULONG Reg, ULONG Value)
{
    WRITE_REGISTER_NOFENCE_ULONG((ULONG *)((PUCHAR)Base + Reg), Value);
}

//
// ---- On-screen progress markers (debug aid, GOYA_HALEXT_MARKERS) --------------------------------
//
// Coloured 32x32 squares in the top-left corner of the boot framebuffer (the firmware's GOP buffer at
// 0x17000000, 1024x600, 4096 bytes per line, BGRA), so that a phone video of the boot shows how far
// the timer side gets before a crash. Slots (left to right):
//   0 AddResourceGroup called (white)     1 event timer registration (green ok / red failed /
//   2 counter registration (green/red)       yellow: CSRT descriptor length mismatch)
//   3 event Initialize called (blue)      4 counter Initialize called (blue)
//   5 ArmTimer: yellow periodic, cyan one-shot, red rejected     6 Stop called (orange)
//   7 flips magenta/green on every timer interrupt
//   8-15 interrupt count / 8 in binary (white = 1), most significant bit first
//   16 QueryCounter used (blue), flips to purple every 4096 calls
//   17 processor that took the last timer interrupt (cyan CPU0, orange CPU1)
//   18 CPU1 has read the counter (yellow)   19 CPU1 has armed/acknowledged the timer (orange)
//
// The same facts go to breadcrumbs in DRAM (CPU0's ACPI parking page + 0xE00, reserved by the
// firmware and kept over a warm restart); Pxa988SmpDxe logs them on the next boot (cpu1.txt).
//
#ifndef GOYA_HALEXT_MARKERS
#define GOYA_HALEXT_MARKERS 1
#endif

#define FB_PHYS     0x17000000ULL
#define FB_PITCH    4096
#define FB_ROWS     48

#define C_WHITE   0x00FFFFFF
#define C_GREEN   0x0000E000
#define C_RED     0x00FF0000
#define C_YELLOW  0x00FFFF00
#define C_BLUE    0x000060FF
#define C_CYAN    0x0000FFFF
#define C_ORANGE  0x00FF8000
#define C_MAGENTA 0x00FF00FF
#define C_PURPLE  0x008000FF
#define C_DARK    0x00303030

static volatile ULONG *gFb;
static ULONG gIrqCount;
static ULONG gQueryCount;

#define HALX_PHYS   0x08180000ULL       // CPU0's parking page (PXA988_PARKING_PAGES)
#define HALX_OFFSET 0xE00
#define HALX_MAGIC  0x584C4148          // "HALX"
enum { HxMagic, HxIrqs, HxIrqsCpu1, HxIrqCpu, HxQueries, HxQueriesCpu1, HxArms, HxArmMode, HxArmTicks, HxCpu1Seen };
static volatile ULONG *gHx;

static ULONG CurrentCpu(VOID)
{
    return _MoveFromCoprocessor(15, 0, 0, 0, 5) & 3;     // MPIDR.Aff0
}

static VOID Mark(ULONG Slot, ULONG Color)
{
#if GOYA_HALEXT_MARKERS
    ULONG X, Y;
    volatile ULONG *Row;

    if (gFb == NULL || Slot > 23) {
        return;
    }
    for (Y = 8; Y < 40; Y++) {
        Row = (volatile ULONG *)((volatile UCHAR *)gFb + Y * FB_PITCH) + 8 + Slot * 40;
        for (X = 0; X < 32; X++) {
            Row[X] = Color;
        }
    }
#else
    UNREFERENCED_PARAMETER(Slot);
    UNREFERENCED_PARAMETER(Color);
#endif
}

static VOID MarkMap(VOID)
{
#if GOYA_HALEXT_MARKERS
    PHYSICAL_ADDRESS Fb;

    if (gFb == NULL) {
        Fb.QuadPart = FB_PHYS;
        gFb = (volatile ULONG *)HalMapIoSpace(Fb, FB_ROWS * FB_PITCH, MmWriteCombined);
    }
#endif
    if (gHx == NULL) {
        PHYSICAL_ADDRESS Hx;
        volatile ULONG *Page;
        ULONG I;

        Hx.QuadPart = HALX_PHYS;
        Page = (volatile ULONG *)HalMapIoSpace(Hx, 0x1000, MmNonCached);
        if (Page != NULL) {
            Page = (volatile ULONG *)((volatile UCHAR *)Page + HALX_OFFSET);
            for (I = 1; I < 16; I++) {
                Page[I] = 0;
            }
            Page[HxMagic] = HALX_MAGIC;
            gHx = Page;
        }
    }
}

static VOID Cpu1Seen(ULONG Slot, ULONG Color)
{
    if (gHx != NULL) {
        gHx[HxCpu1Seen]++;
    }
    Mark(Slot, Color);
}

static VOID MarkIrq(VOID)
{
    ULONG I, V;
    ULONG Cpu = CurrentCpu();

    if (gHx != NULL) {
        gHx[HxIrqs]++;
        gHx[HxIrqCpu] = Cpu;
        if (Cpu != 0) {
            gHx[HxIrqsCpu1]++;
        }
    }
    Mark(17, (Cpu == 0) ? C_CYAN : C_ORANGE);
    if (Cpu != 0) {
        Cpu1Seen(19, C_ORANGE);
    }

    gIrqCount++;
    Mark(7, (gIrqCount & 1) ? C_MAGENTA : C_GREEN);
    if ((gIrqCount & 7) == 0) {
        V = gIrqCount >> 3;
        for (I = 0; I < 8; I++) {
            Mark(8 + I, (V & (0x80UL >> I)) ? C_WHITE : C_DARK);
        }
    }
}

//
// ---- Counter 1: free-running readable counter -------------------------------
//

static NTSTATUS HalpMmpCounterInitialize(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    PULONG Base = T->BaseAddressPtr;
    ULONG Cer = ReadReg(Base, MMP_TMR_CER);

    Mark(4, C_BLUE);

    //
    // Same sequence as the firmware's MmpTimerLib and the vendor kernel
    // (timer_config): fast clock, free running, no interrupt, enabled.
    // The functional/APB clock is left as the firmware configured it.
    //
    WriteReg(Base, MMP_TMR_CCR, ReadReg(Base, MMP_TMR_CCR) & ~(3UL << 2));
    WriteReg(Base, MMP_TMR_CMR, ReadReg(Base, MMP_TMR_CMR) | (1UL << MMP_FREE_COUNTER));
    WriteReg(Base, MMP_TMR_PLCR(MMP_FREE_COUNTER), 0);
    WriteReg(Base, MMP_TMR_ICR(MMP_FREE_COUNTER), 0x7);
    WriteReg(Base, MMP_TMR_IER(MMP_FREE_COUNTER), 0);
    WriteReg(Base, MMP_TMR_CER, Cer | (1UL << MMP_FREE_COUNTER));
    return STATUS_SUCCESS;
}

static volatile ULONG gLastCounter;

static ULONGLONG HalpMmpCounterQuery(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    ULONG Value = 0;
    ULONG Last;
    ULONG I;

    if (gHx != NULL) {
        gHx[HxQueries]++;
    }
    if (CurrentCpu() != 0) {
        if (gHx != NULL) {
            gHx[HxQueriesCpu1]++;
        }
        Cpu1Seen(18, C_YELLOW);
    }
    if ((gQueryCount++ & 0xFFF) == 0) {
        Mark(16, (gQueryCount & 0x1000) ? C_PURPLE : C_BLUE);
    }

    //
    // The counter cannot be read directly: request a latch, then read the
    // latched value back. The latch takes a few cycles of the 3.25 MHz
    // counter clock (~1 us); three APB reads were not always enough, which
    // returns the previous latch (time standing still) - a suspected cause of
    // the intermittent early stops. Now: at least MMP_COUNTER_READ_SPINS reads,
    // then until two consecutive reads agree, and never backwards. No stall
    // function may be used here (the HAL's stall depends on this counter).
    //
    WriteReg(T->BaseAddressPtr, MMP_TMR_CVWR(MMP_FREE_COUNTER), 1);
    for (I = 0; I < MMP_COUNTER_READ_SPINS; I++) {
        Value = ReadReg(T->BaseAddressPtr, MMP_TMR_CVWR(MMP_FREE_COUNTER));
    }
    for (I = 0; I < 16; I++) {
        ULONG Again = ReadReg(T->BaseAddressPtr, MMP_TMR_CVWR(MMP_FREE_COUNTER));
        if (Again == Value) {
            break;
        }
        Value = Again;
    }

    //
    // Monotonic: a value slightly behind the last one returned is a bad latch,
    // not a wrap (a wrap is a small positive difference modulo 2^32).
    //
    // Two processors may read at once: update gLastCounter only if no one else did meanwhile.
    for (;;) {
        Last = gLastCounter;
        if ((LONG)(Value - Last) < 0 && (Last - Value) < 0x40000000UL) {
            Value = Last;
            break;
        }
        if ((ULONG)InterlockedCompareExchange((volatile LONG *)&gLastCounter, (LONG)Value, (LONG)Last) == Last) {
            break;
        }
    }
    return Value;
}

//
// ---- Counter 0: match interrupt timer ---------------------------------------
//

static NTSTATUS HalpMmpEventInitialize(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    PULONG Base = T->BaseAddressPtr;

    Mark(3, C_BLUE);

    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) & ~(1UL << MMP_EVENT_COUNTER));
    WriteReg(Base, MMP_TMR_CCR, ReadReg(Base, MMP_TMR_CCR) & ~(3UL << 0));   // counter 0: fast clock
    WriteReg(Base, MMP_TMR_CMR, ReadReg(Base, MMP_TMR_CMR) | (1UL << MMP_EVENT_COUNTER)); // not used for counter 0 by the vendor kernel; harmless
    WriteReg(Base, MMP_TMR_PLCR(MMP_EVENT_COUNTER), 1);          // periodic reload from PLVR(0)=0, as the vendor kernel
    WriteReg(Base, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x7);
    WriteReg(Base, MMP_TMR_IER(MMP_EVENT_COUNTER), 0);
    return STATUS_SUCCESS;
}

static VOID HalpMmpEventStopCounter(PULONG Base)
{
    // 0x02 = only counter 1 stays enabled (vendor kernel writes the same)
    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) & ~(1UL << MMP_EVENT_COUNTER));
}

static VOID HalpMmpEventProgram(PULONG Base, ULONG Ticks)
{
    HalpMmpEventStopCounter(Base);
    WriteReg(Base, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    WriteReg(Base, MMP_TMR_IER(MMP_EVENT_COUNTER), 0x1);
    WriteReg(Base, MMP_TMR_TN_MM(MMP_EVENT_COUNTER, 0), Ticks - 1);
    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) | (1UL << MMP_EVENT_COUNTER));
}

static NTSTATUS HalpMmpEventArm(__in PVOID TimerDataPtr, __in TIMER_MODE Mode, __in ULONGLONG TickCount)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    if (TickCount == 0 || TickCount >= MMP_RESET_MAX_TICKS) {
        Mark(5, C_RED);
        return STATUS_INVALID_PARAMETER;
    }

    if (gHx != NULL) {
        gHx[HxArms]++;
        gHx[HxArmMode] = (ULONG)Mode | (CurrentCpu() << 8);
        gHx[HxArmTicks] = (ULONG)TickCount;
    }
    if (CurrentCpu() != 0) {
        Cpu1Seen(19, C_ORANGE);
    }

    switch (Mode) {
    case TimerModePeriodic:
        T->PeriodInTicks = (ULONG)TickCount;   // re-armed in AcknowledgeInterrupt
        Mark(5, C_YELLOW);
        break;
    case TimerModeOneShot:
        T->PeriodInTicks = 0;
        Mark(5, C_CYAN);
        break;
    default:
        Mark(5, C_RED);
        return STATUS_INVALID_PARAMETER;
    }

    HalpMmpEventProgram(T->BaseAddressPtr, (ULONG)TickCount);
    return STATUS_SUCCESS;
}

static VOID HalpMmpEventAcknowledge(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    WriteReg(T->BaseAddressPtr, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    HalpMmpEventStopCounter(T->BaseAddressPtr);        // as the vendor kernel: stop after each match
    MarkIrq();

    if (T->PeriodInTicks != 0) {
        HalpMmpEventProgram(T->BaseAddressPtr, T->PeriodInTicks);
    }
}

static VOID HalpMmpEventStop(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    Mark(6, C_ORANGE);
    WriteReg(T->BaseAddressPtr, MMP_TMR_IER(MMP_EVENT_COUNTER), 0);
    HalpMmpEventStopCounter(T->BaseAddressPtr);
    WriteReg(T->BaseAddressPtr, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    T->PeriodInTicks = 0;
}

//
// ---- Registration -----------------------------------------------------------
//

static NTSTATUS
HalpMmpTimerRegister(
    ULONG Handle,
    PCSRT_RESOURCE_GROUP_HEADER ResourceGroup,
    PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE Csrt,
    BOOLEAN IsEvent
    )
{
    NTSTATUS Status;
    PHYSICAL_ADDRESS PhysicalAddress;
    MMP_TIMER_INTERNAL_DATA InternalData;
    TIMER_INITIALIZATION_BLOCK NewTimer;

    RtlZeroMemory(&InternalData, sizeof(InternalData));

    PhysicalAddress.QuadPart = (LONGLONG)Csrt->BaseAddress;

    // Both descriptors share the block; the HAL tolerates the repeat registration
    // on the first call only, so ignore "already registered" on the second.
    Status = HalRegisterPermanentAddressUsage(PhysicalAddress, 0x1000);
    if (!NT_SUCCESS(Status) && Status != STATUS_CONFLICTING_ADDRESSES) {
        return Status;
    }

    InternalData.BaseAddressPtr = HalMapIoSpace(PhysicalAddress, 0x1000, MmNonCached);
    if (InternalData.BaseAddressPtr == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&NewTimer, sizeof(NewTimer));
    INITIALIZE_TIMER_HEADER(&NewTimer);

    NewTimer.InternalData = &InternalData;
    NewTimer.InternalDataSize = sizeof(InternalData);
    NewTimer.CounterBitWidth = 32;
    NewTimer.CounterFrequency = Csrt->Frequency / Csrt->FrequencyScale;
    NewTimer.MaxDivisor = 1;
    NewTimer.KnownType = TimerUnknown;
    NewTimer.Identifier = Csrt->Header.Uid;

    if (IsEvent) {
        NewTimer.FunctionTable.Initialize = HalpMmpEventInitialize;
        NewTimer.FunctionTable.AcknowledgeInterrupt = HalpMmpEventAcknowledge;
        NewTimer.FunctionTable.ArmTimer = HalpMmpEventArm;
        NewTimer.FunctionTable.Stop = HalpMmpEventStop;
        //
        // Only flags the Windows 8 (build 8520) HAL is known to accept: Microsoft's own 8520 timer
        // extensions (HalExtSP804) register the same TIMER_INITIALIZATION_BLOCK version/size (1/0x68)
        // and never set TIMER_ALWAYS_ON (0x8000, Windows 10 header).
        //
        NewTimer.Capabilities = TIMER_PERIODIC_CAPABLE |
                                TIMER_ONE_SHOT_CAPABLE |
                                TIMER_GENERATES_LINE_BASED_INTERRUPTS;
        NewTimer.Interrupt.Mode = LevelSensitive;
        NewTimer.Interrupt.Polarity = InterruptActiveHigh;
        NewTimer.Interrupt.Gsi = Csrt->Interrupt;
    } else {
        NewTimer.FunctionTable.Initialize = HalpMmpCounterInitialize;
        NewTimer.FunctionTable.QueryCounter = HalpMmpCounterQuery;
        NewTimer.Capabilities = TIMER_COUNTER_READABLE;
    }

    Status = RegisterResourceDescriptor(Handle, ResourceGroup, &Csrt->Header, &NewTimer);
    Mark(IsEvent ? 1 : 2, NT_SUCCESS(Status) ? C_GREEN : C_RED);
    return Status;
}

NTSTATUS
AddResourceGroup (
    __in ULONG Handle,
    __in PCSRT_RESOURCE_GROUP_HEADER ResourceGroup
    )
{
    PCSRT_RESOURCE_DESCRIPTOR_HEADER Header = NULL;
    PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE Timer;
    NTSTATUS Status;

    MarkMap();
    Mark(0, C_WHITE);

    for (;;) {
        Header = GetNextResourceDescriptor(Handle, ResourceGroup, Header,
                                           CSRT_RD_TYPE_TIMER,
                                           CSRT_RD_SUBTYPE_TIMER,
                                           CSRT_RD_UID_ANY);
        if (Header == NULL) {
            break;
        }

        if (Header->Length != sizeof(CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE)) {
            Mark(1, C_YELLOW);
            return STATUS_INVALID_PARAMETER;
        }

        Timer = (PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE)Header;
        switch (Timer->TimerType) {
        case MMP_TIMER_TYPE_EVENT:
            Status = HalpMmpTimerRegister(Handle, ResourceGroup, Timer, TRUE);
            break;
        case MMP_TIMER_TYPE_COUNTER:
            Status = HalpMmpTimerRegister(Handle, ResourceGroup, Timer, FALSE);
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }

        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    return STATUS_SUCCESS;
}
