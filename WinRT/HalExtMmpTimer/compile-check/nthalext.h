/* MINIMAL SHIM for compile-checking only; layouts are NOT the real WDK ones. */
typedef long NTSTATUS; typedef unsigned long ULONG, *PULONG; typedef unsigned char UCHAR,*PUCHAR, BOOLEAN;
typedef unsigned int UINT32; typedef unsigned long long ULONGLONG; typedef long long LONGLONG; typedef void *PVOID; typedef unsigned short USHORT;
typedef void VOID;
typedef long LONG; long InterlockedCompareExchange(volatile long *Destination, long Exchange, long Comparand);
#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)
#define __in
#define FORCEINLINE inline
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_CONFLICTING_ADDRESSES ((NTSTATUS)0xC0000018)
typedef union { struct { unsigned long LowPart; long HighPart; }; long long QuadPart; } PHYSICAL_ADDRESS;
typedef enum { MmNonCached, MmCached, MmWriteCombined } MEMORY_CACHING_TYPE;
typedef enum { TimerModePeriodic, TimerModeOneShot } TIMER_MODE;
typedef enum { TimerUnknown } TIMER_KNOWN_TYPE;
typedef enum { LevelSensitive } INTERRUPT_MODE;
typedef enum { InterruptActiveHigh } INTERRUPT_POLARITY;
#define TIMER_PERIODIC_CAPABLE 1
#define TIMER_ONE_SHOT_CAPABLE 2
#define TIMER_GENERATES_LINE_BASED_INTERRUPTS 4
#define TIMER_ALWAYS_ON 8
#define TIMER_COUNTER_READABLE 16
typedef struct { ULONG Length; USHORT Type, Subtype; ULONG Uid; } CSRT_RESOURCE_DESCRIPTOR_HEADER, *PCSRT_RESOURCE_DESCRIPTOR_HEADER;
typedef struct { ULONG Length; } CSRT_RESOURCE_GROUP_HEADER, *PCSRT_RESOURCE_GROUP_HEADER;
#define CSRT_RD_TYPE_TIMER 2
#define CSRT_RD_SUBTYPE_TIMER 1
#define CSRT_RD_UID_ANY 0xFFFFFFFF
typedef struct { struct { NTSTATUS (*Initialize)(PVOID); ULONGLONG (*QueryCounter)(PVOID); void (*AcknowledgeInterrupt)(PVOID); NTSTATUS (*ArmTimer)(PVOID,TIMER_MODE,ULONGLONG); void (*Stop)(PVOID);} FunctionTable;
  PVOID InternalData; ULONG InternalDataSize, CounterBitWidth, CounterFrequency, MaxDivisor, Capabilities, Identifier; TIMER_KNOWN_TYPE KnownType;
  struct { INTERRUPT_MODE Mode; INTERRUPT_POLARITY Polarity; ULONG Gsi; } Interrupt; } TIMER_INITIALIZATION_BLOCK;
#define INITIALIZE_TIMER_HEADER(b)
NTSTATUS HalRegisterPermanentAddressUsage(PHYSICAL_ADDRESS, ULONG);
PVOID HalMapIoSpace(PHYSICAL_ADDRESS, ULONG, MEMORY_CACHING_TYPE);
NTSTATUS RegisterResourceDescriptor(ULONG, PCSRT_RESOURCE_GROUP_HEADER, PCSRT_RESOURCE_DESCRIPTOR_HEADER, TIMER_INITIALIZATION_BLOCK*);
PCSRT_RESOURCE_DESCRIPTOR_HEADER GetNextResourceDescriptor(ULONG, PCSRT_RESOURCE_GROUP_HEADER, PCSRT_RESOURCE_DESCRIPTOR_HEADER, ULONG, ULONG, ULONG);
#define READ_REGISTER_NOFENCE_ULONG(p) (*(volatile ULONG*)(p))
#define WRITE_REGISTER_NOFENCE_ULONG(p,v) (*(volatile ULONG*)(p)=(v))
void *RtlZeroMemory_(void*, unsigned long);
#define RtlZeroMemory(d,l) __builtin_memset(d,0,l)
#define UNREFERENCED_PARAMETER(P) ((void)(P))
