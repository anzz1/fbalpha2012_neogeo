// 680x0 (Sixty Eight K) Interface
#include "burnint.h"
#include "m68000_intf.h"
#include "m68000_debug.h"

#ifdef EMU_M68K
INT32 nSekM68KContextSize[SEK_MAX];
INT8* SekM68KContext[SEK_MAX];
#endif

INT32 nSekCount = -1;							// Number of allocated 68000s
struct SekExt *SekExt[SEK_MAX] = { NULL, }, *pSekExt = NULL;

INT32 nSekActive = -1;								// The cpu which is currently being emulated
INT32 nSekCyclesTotal, nSekCyclesScanline, nSekCyclesSegment, nSekCyclesDone, nSekCyclesToDo;

INT32 nSekCPUType[SEK_MAX], nSekCycles[SEK_MAX], nSekIRQPending[SEK_MAX];

#if defined (FBA_DEBUG)

void (*SekDbgBreakpointHandlerRead)(UINT32, INT32);
void (*SekDbgBreakpointHandlerFetch)(UINT32, INT32);
void (*SekDbgBreakpointHandlerWrite)(UINT32, INT32);

UINT32 (*SekDbgFetchByteDisassembler)(UINT32);
UINT32 (*SekDbgFetchWordDisassembler)(UINT32);
UINT32 (*SekDbgFetchLongDisassembler)(UINT32);

static struct { UINT32 address; INT32 id; } BreakpointDataRead[9]  = { { 0, 0 }, };
static struct { UINT32 address; INT32 id; } BreakpointDataWrite[9] = { { 0, 0 }, };
static struct { UINT32 address; INT32 id; } BreakpointFetch[9] = { { 0, 0 }, };

#endif

#if defined (FBA_DEBUG)

inline static void CheckBreakpoint_R(UINT32 a, const UINT32 m)
{
	a &= m;

	for (INT32 i = 0; BreakpointDataRead[i].address; i++) {
		if ((BreakpointDataRead[i].address & m) == a) {
			SekDbgBreakpointHandlerRead(a, BreakpointDataRead[i].id);
			return;
		}
	}
}

inline static void CheckBreakpoint_W(UINT32 a, const UINT32 m)
{
	a &= m;

	for (INT32 i = 0; BreakpointDataWrite[i].address; i++) {
		if ((BreakpointDataWrite[i].address & m) == a) {
			SekDbgBreakpointHandlerWrite(a, BreakpointDataWrite[i].id);
			return;
		}
	}
}

inline static void CheckBreakpoint_PC()
{
	for (INT32 i = 0; BreakpointFetch[i].address; i++) {
		if (BreakpointFetch[i].address == (UINT32)SekGetPC(-1)) {
			SekDbgBreakpointHandlerFetch(SekGetPC(-1), BreakpointFetch[i].id);
			return;
		}
	}
}

inline static void SingleStep_PC()
{
	SekDbgBreakpointHandlerFetch(SekGetPC(-1), 0);
}

#endif

// ----------------------------------------------------------------------------
// Default memory access handlers

UINT8 __fastcall DefReadByte(UINT32 a) { return 0; }
void __fastcall DefWriteByte(UINT32 a, UINT8 b) { }

#define DEFWORDHANDLERS(i)																				\
	UINT16 __fastcall DefReadWord##i(UINT32 a) { SEK_DEF_READ_WORD(i, a) }				\
	void __fastcall DefWriteWord##i(UINT32 a, UINT16 d) { SEK_DEF_WRITE_WORD(i, a ,d) }
#define DEFLONGHANDLERS(i)																				\
	UINT32 __fastcall DefReadLong##i(UINT32 a) { SEK_DEF_READ_LONG(i, a) }					\
	void __fastcall DefWriteLong##i(UINT32 a, UINT32 d) { SEK_DEF_WRITE_LONG(i, a , d) }

DEFWORDHANDLERS(0)
DEFLONGHANDLERS(0)

#if SEK_MAXHANDLER >= 2
 DEFWORDHANDLERS(1)
 DEFLONGHANDLERS(1)
#endif

#if SEK_MAXHANDLER >= 3
 DEFWORDHANDLERS(2)
 DEFLONGHANDLERS(2)
#endif

#if SEK_MAXHANDLER >= 4
 DEFWORDHANDLERS(3)
 DEFLONGHANDLERS(3)
#endif

#if SEK_MAXHANDLER >= 5
 DEFWORDHANDLERS(4)
 DEFLONGHANDLERS(4)
#endif

#if SEK_MAXHANDLER >= 6
 DEFWORDHANDLERS(5)
 DEFLONGHANDLERS(5)
#endif

#if SEK_MAXHANDLER >= 7
 DEFWORDHANDLERS(6)
 DEFLONGHANDLERS(6)
#endif

#if SEK_MAXHANDLER >= 8
 DEFWORDHANDLERS(7)
 DEFLONGHANDLERS(7)
#endif

#if SEK_MAXHANDLER >= 9
 DEFWORDHANDLERS(8)
 DEFLONGHANDLERS(8)
#endif

#if SEK_MAXHANDLER >= 10
 DEFWORDHANDLERS(9)
 DEFLONGHANDLERS(9)
#endif

// ----------------------------------------------------------------------------
// Memory access functions

// Mapped Memory lookup (               for read)
#define FIND_R(x) pSekExt->MemMap[ x >> SEK_SHIFT]
// Mapped Memory lookup (+ SEK_WADD     for write)
#define FIND_W(x) pSekExt->MemMap[(x >> SEK_SHIFT) + SEK_WADD]
// Mapped Memory lookup (+ SEK_WADD * 2 for fetch)
#define FIND_F(x) pSekExt->MemMap[(x >> SEK_SHIFT) + SEK_WADD * 2]

// Normal memory access functions
inline static UINT8 ReadByte(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("read8 0x%08X\n"), a);

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		return pr[a & SEK_PAGEM];
	}
	return pSekExt->ReadByte[(uintptr_t)pr](a);
}

inline static UINT8 FetchByte(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("fetch8 0x%08X\n"), a);

	pr = FIND_F(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		return pr[a & SEK_PAGEM];
	}
	return pSekExt->ReadByte[(uintptr_t)pr](a);
}

inline static void WriteByte(UINT32 a, UINT8 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("write8 0x%08X\n"), a);

	pr = FIND_W(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		pr[a & SEK_PAGEM] = (UINT8)d;
		return;
	}
	pSekExt->WriteByte[(uintptr_t)pr](a, d);
}

inline static void WriteByteROM(UINT32 a, UINT8 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		pr[a & SEK_PAGEM] = (UINT8)d;
		return;
	}
	pSekExt->WriteByte[(uintptr_t)pr](a, d);
}

inline static UINT16 ReadWord(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("read16 0x%08X\n"), a);

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		return BURN_ENDIAN_SWAP_INT16(*((UINT16*)(pr + (a & SEK_PAGEM))));
	}
	return pSekExt->ReadWord[(uintptr_t)pr](a);
}

inline static UINT16 FetchWord(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("fetch16 0x%08X\n"), a);

	pr = FIND_F(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		return BURN_ENDIAN_SWAP_INT16(*((UINT16*)(pr + (a & SEK_PAGEM))));
	}
	return pSekExt->ReadWord[(uintptr_t)pr](a);
}

inline static void WriteWord(UINT32 a, UINT16 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("write16 0x%08X\n"), a);

	pr = FIND_W(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		*((UINT16*)(pr + (a & SEK_PAGEM))) = (UINT16)BURN_ENDIAN_SWAP_INT16(d);
		return;
	}
	pSekExt->WriteWord[(uintptr_t)pr](a, d);
}

inline static void WriteWordROM(UINT32 a, UINT16 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		*((UINT16*)(pr + (a & SEK_PAGEM))) = (UINT16)d;
		return;
	}
	pSekExt->WriteWord[(uintptr_t)pr](a, d);
}

inline static UINT32 ReadLong(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("read32 0x%08X\n"), a);

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		UINT32 r = *((UINT32*)(pr + (a & SEK_PAGEM)));
		r = (r >> 16) | (r << 16);
		return BURN_ENDIAN_SWAP_INT32(r);
	}
	return pSekExt->ReadLong[(uintptr_t)pr](a);
}

inline static UINT32 FetchLong(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("fetch32 0x%08X\n"), a);

	pr = FIND_F(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		UINT32 r = *((UINT32*)(pr + (a & SEK_PAGEM)));
		r = (r >> 16) | (r << 16);
		return BURN_ENDIAN_SWAP_INT32(r);
	}
	return pSekExt->ReadLong[(uintptr_t)pr](a);
}

inline static void WriteLong(UINT32 a, UINT32 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

//	bprintf(PRINT_NORMAL, _T("write32 0x%08X\n"), a);

	pr = FIND_W(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		d = (d >> 16) | (d << 16);
		*((UINT32*)(pr + (a & SEK_PAGEM))) = BURN_ENDIAN_SWAP_INT32(d);
		return;
	}
	pSekExt->WriteLong[(uintptr_t)pr](a, d);
}

inline static void WriteLongROM(UINT32 a, UINT32 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);
	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		d = (d >> 16) | (d << 16);
		*((UINT32*)(pr + (a & SEK_PAGEM))) = d;
		return;
	}
	pSekExt->WriteLong[(uintptr_t)pr](a, d);
}

#if defined (FBA_DEBUG)

// Breakpoint checking memory access functions
UINT8 __fastcall ReadByteBP(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);

	CheckBreakpoint_R(a, ~0);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		return pr[a & SEK_PAGEM];
	}
	return pSekExt->ReadByte[(uintptr_t)pr](a);
}

void __fastcall WriteByteBP(UINT32 a, UINT8 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_W(a);

	CheckBreakpoint_W(a, ~0);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		a ^= 1;
		pr[a & SEK_PAGEM] = (UINT8)d;
		return;
	}
	pSekExt->WriteByte[(uintptr_t)pr](a, d);
}

UINT16 __fastcall ReadWordBP(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);

	CheckBreakpoint_R(a, ~1);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		return *((UINT16*)(pr + (a & SEK_PAGEM)));
	}
	return pSekExt->ReadWord[(uintptr_t)pr](a);
}

void __fastcall WriteWordBP(UINT32 a, UINT16 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_W(a);

	CheckBreakpoint_W(a, ~1);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		*((UINT16*)(pr + (a & SEK_PAGEM))) = (UINT16)d;
		return;
	}
	pSekExt->WriteWord[(uintptr_t)pr](a, d);
}

UINT32 __fastcall ReadLongBP(UINT32 a)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_R(a);

	CheckBreakpoint_R(a, ~1);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		UINT32 r = *((UINT32*)(pr + (a & SEK_PAGEM)));
		r = (r >> 16) | (r << 16);
		return r;
	}
	return pSekExt->ReadLong[(uintptr_t)pr](a);
}

void __fastcall WriteLongBP(UINT32 a, UINT32 d)
{
	UINT8* pr;

	a &= 0xFFFFFF;

	pr = FIND_W(a);

	CheckBreakpoint_W(a, ~1);

	if ((uintptr_t)pr >= SEK_MAXHANDLER) {
		d = (d >> 16) | (d << 16);
		*((UINT32*)(pr + (a & SEK_PAGEM))) = d;
		return;
	}
	pSekExt->WriteLong[(uintptr_t)pr](a, d);
}

#endif

#ifdef EMU_M68K
unsigned int __fastcall M68KReadByte(unsigned int a) { return (unsigned int)ReadByte(a); }
unsigned int __fastcall M68KReadWord(unsigned int a) { return (unsigned int)ReadWord(a); }
unsigned int __fastcall M68KReadLong(unsigned int a) { return               ReadLong(a); }

unsigned int __fastcall M68KFetchByte(unsigned int a) { return (unsigned int)FetchByte(a); }
unsigned int __fastcall M68KFetchWord(unsigned int a) { return (unsigned int)FetchWord(a); }
unsigned int __fastcall M68KFetchLong(unsigned int a) { return               FetchLong(a); }

#ifdef FBA_DEBUG
UINT32 __fastcall M68KReadByteBP(UINT32 a) { return (UINT32)ReadByteBP(a); }
UINT32 __fastcall M68KReadWordBP(UINT32 a) { return (UINT32)ReadWordBP(a); }
UINT32 __fastcall M68KReadLongBP(UINT32 a) { return               ReadLongBP(a); }

void __fastcall M68KWriteByteBP(UINT32 a, UINT32 d) { WriteByteBP(a, d); }
void __fastcall M68KWriteWordBP(UINT32 a, UINT32 d) { WriteWordBP(a, d); }
void __fastcall M68KWriteLongBP(UINT32 a, UINT32 d) { WriteLongBP(a, d); }

void M68KCheckBreakpoint() { CheckBreakpoint_PC(); }
void M68KSingleStep() { SingleStep_PC(); }

UINT32 (__fastcall *M68KReadByteDebug)(UINT32);
UINT32 (__fastcall *M68KReadWordDebug)(UINT32);
UINT32 (__fastcall *M68KReadLongDebug)(UINT32);

void (__fastcall *M68KWriteByteDebug)(UINT32, UINT32);
void (__fastcall *M68KWriteWordDebug)(UINT32, UINT32);
void (__fastcall *M68KWriteLongDebug)(UINT32, UINT32);
#endif

void __fastcall M68KWriteByte(unsigned int a, unsigned int d) { WriteByte(a, d); }
void __fastcall M68KWriteWord(unsigned int a, unsigned int d) { WriteWord(a, d); }
void __fastcall M68KWriteLong(unsigned int a, unsigned int d) { WriteLong(a, d); }
#endif

// ----------------------------------------------------------------------------
// Memory accesses (non-emu specific)

UINT32 SekReadByte(UINT32 a) { return (UINT32)ReadByte(a); }
UINT32 SekReadWord(UINT32 a) { return (UINT32)ReadWord(a); }
UINT32 SekReadLong(UINT32 a) { return ReadLong(a); }

UINT32 SekFetchByte(UINT32 a) { return (UINT32)FetchByte(a); }
UINT32 SekFetchWord(UINT32 a) { return (UINT32)FetchWord(a); }
UINT32 SekFetchLong(UINT32 a) { return FetchLong(a); }

void SekWriteByte(UINT32 a, UINT8 d) { WriteByte(a, d); }
void SekWriteWord(UINT32 a, UINT16 d) { WriteWord(a, d); }
void SekWriteLong(UINT32 a, UINT32 d) { WriteLong(a, d); }

void SekWriteByteROM(UINT32 a, UINT8 d) { WriteByteROM(a, d); }
void SekWriteWordROM(UINT32 a, UINT16 d) { WriteWordROM(a, d); }
void SekWriteLongROM(UINT32 a, UINT32 d) { WriteLongROM(a, d); }

// ----------------------------------------------------------------------------
// Callbacks for Musashi

#ifdef EMU_M68K
int M68KIRQAcknowledge(int nIRQ)
{
	if (nSekIRQPending[nSekActive] & SEK_IRQSTATUS_AUTO) {
		m68k_set_irq(0);
		nSekIRQPending[nSekActive] = 0;
	}
	
	if (pSekExt->IrqCallback) {
		return pSekExt->IrqCallback(nIRQ);
	}

	return M68K_INT_ACK_AUTOVECTOR;
}

void M68KResetCallback(void)
{
	if (pSekExt->ResetCallback)
		pSekExt->ResetCallback();
}

void M68KRTECallback(void)
{
	if (pSekExt->RTECallback) {
		pSekExt->RTECallback();
	}
}

void M68KcmpildCallback(unsigned int val, int reg)
{
	if (pSekExt->CmpCallback)
		pSekExt->CmpCallback(val, reg);
}
#endif

// ----------------------------------------------------------------------------
// Initialisation/exit/reset

#ifdef EMU_M68K
static INT32 SekInitCPUM68K(INT32 nCount, INT32 nCPUType)
{
	nSekCPUType[nCount] = nCPUType;

	switch (nCPUType) {
		case 0x68000:
			m68k_set_cpu_type(M68K_CPU_TYPE_68000);
			break;
		case 0x68010:
			m68k_set_cpu_type(M68K_CPU_TYPE_68010);
			break;
		case 0x68EC020:
			m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
			break;
		default:
			return 1;
	}

	nSekM68KContextSize[nCount] = m68k_context_size();
	SekM68KContext[nCount] = (INT8*)malloc(nSekM68KContextSize[nCount]);
	if (SekM68KContext[nCount] == NULL) {
		return 1;
	}
	memset(SekM68KContext[nCount], 0, nSekM68KContextSize[nCount]);
	m68k_get_context(SekM68KContext[nCount]);

	return 0;
}
#endif

void SekNewFrame()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekNewFrame called without init\n"));
#endif

	for (INT32 i = 0; i <= nSekCount; i++) {
		nSekCycles[i] = 0;
	}

	nSekCyclesTotal = 0;
}

void SekSetCyclesScanline(INT32 nCycles)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetCyclesScanline called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetCyclesScanline called when no CPU open\n"));
#endif

	nSekCyclesScanline = nCycles;
}

static UINT8 SekCheatRead(UINT32 a)
{
	return SekReadByte(a);
}

static struct cpu_core_config SekCheatCpuConfig =
{
	SekOpen,
	SekClose,
	SekCheatRead,
	SekWriteByteROM,
	SekGetActive,
	SekTotalCycles,
	SekNewFrame,
	SekRun,
	SekRunEnd,
	SekReset,
	(1<<24),	// 0x1000000
	0
};

INT32 SekInit(INT32 nCount, INT32 nCPUType)
{
	struct SekExt* ps = NULL;

	if (nSekActive >= 0) {
		SekClose();
		nSekActive = -1;
	}

	if (nCount > nSekCount)
		nSekCount = nCount;

	// Allocate cpu extenal data (memory map etc)
	SekExt[nCount] = (struct SekExt*)malloc(sizeof(struct SekExt));
	if (SekExt[nCount] == NULL) {
		SekExit();
		return 1;
	}
	memset(SekExt[nCount], 0, sizeof(struct SekExt));

	// Put in default memory handlers
	ps = SekExt[nCount];

	for (INT32 j = 0; j < SEK_MAXHANDLER; j++) {
		ps->ReadByte[j]  = DefReadByte;
		ps->WriteByte[j] = DefWriteByte;
	}

	ps->ReadWord[0]  = DefReadWord0;
	ps->WriteWord[0] = DefWriteWord0;
	ps->ReadLong[0]  = DefReadLong0;
	ps->WriteLong[0] = DefWriteLong0;

#if SEK_MAXHANDLER >= 2
	ps->ReadWord[1]  = DefReadWord1;
	ps->WriteWord[1] = DefWriteWord1;
	ps->ReadLong[1]  = DefReadLong1;
	ps->WriteLong[1] = DefWriteLong1;
#endif

#if SEK_MAXHANDLER >= 3
	ps->ReadWord[2]  = DefReadWord2;
	ps->WriteWord[2] = DefWriteWord2;
	ps->ReadLong[2]  = DefReadLong2;
	ps->WriteLong[2] = DefWriteLong2;
#endif

#if SEK_MAXHANDLER >= 4
	ps->ReadWord[3]  = DefReadWord3;
	ps->WriteWord[3] = DefWriteWord3;
	ps->ReadLong[3]  = DefReadLong3;
	ps->WriteLong[3] = DefWriteLong3;
#endif

#if SEK_MAXHANDLER >= 5
	ps->ReadWord[4]  = DefReadWord4;
	ps->WriteWord[4] = DefWriteWord4;
	ps->ReadLong[4]  = DefReadLong4;
	ps->WriteLong[4] = DefWriteLong4;
#endif

#if SEK_MAXHANDLER >= 6
	ps->ReadWord[5]  = DefReadWord5;
	ps->WriteWord[5] = DefWriteWord5;
	ps->ReadLong[5]  = DefReadLong5;
	ps->WriteLong[5] = DefWriteLong5;
#endif

#if SEK_MAXHANDLER >= 7
	ps->ReadWord[6]  = DefReadWord6;
	ps->WriteWord[6] = DefWriteWord6;
	ps->ReadLong[6]  = DefReadLong6;
	ps->WriteLong[6] = DefWriteLong6;
#endif

#if SEK_MAXHANDLER >= 8
	ps->ReadWord[7]  = DefReadWord7;
	ps->WriteWord[7] = DefWriteWord7;
	ps->ReadLong[7]  = DefReadLong7;
	ps->WriteLong[7] = DefWriteLong7;
#endif

#if SEK_MAXHANDLER >= 9
	ps->ReadWord[8]  = DefReadWord8;
	ps->WriteWord[8] = DefWriteWord8;
	ps->ReadLong[8]  = DefReadLong8;
	ps->WriteLong[8] = DefWriteLong8;
#endif

#if SEK_MAXHANDLER >= 10
	ps->ReadWord[9]  = DefReadWord9;
	ps->WriteWord[9] = DefWriteWord9;
	ps->ReadLong[9]  = DefReadLong9;
	ps->WriteLong[9] = DefWriteLong9;
#endif

#if SEK_MAXHANDLER >= 11
	for (int j = 10; j < SEK_MAXHANDLER; j++) {
		ps->ReadWord[j]  = DefReadWord0;
		ps->WriteWord[j] = DefWriteWord0;
		ps->ReadLong[j]  = DefReadLong0;
		ps->WriteLong[j] = DefWriteLong0;
	}
#endif

	// Map the normal memory handlers
	SekDbgDisableBreakpoints();

#ifdef EMU_M68K
		m68k_init();
		if (SekInitCPUM68K(nCount, nCPUType)) {
			SekExit();
			return 1;
		}
#endif

	nSekCycles[nCount] = 0;
	nSekIRQPending[nCount] = 0;

	nSekCyclesTotal = 0;
	nSekCyclesScanline = 0;

	CpuCheatRegister(nCount, &SekCheatCpuConfig);

	return 0;
}

#ifdef EMU_M68K
static void SekCPUExitM68K(INT32 i)
{
		if(SekM68KContext[i]) {
			free(SekM68KContext[i]);
			SekM68KContext[i] = NULL;
		}
}
#endif

INT32 SekExit()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekExit called without init\n"));
#endif

	// Deallocate cpu extenal data (memory map etc)
	for (INT32 i = 0; i <= nSekCount; i++) {

#ifdef EMU_M68K
		SekCPUExitM68K(i);
#endif

		// Deallocate other context data
		if (SekExt[i]) {
			free(SekExt[i]);
			SekExt[i] = NULL;
		}
	}

	pSekExt = NULL;

	nSekActive = -1;
	nSekCount = -1;
	
	return 0;
}

void SekReset()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekReset called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekReset called when no CPU open\n"));
#endif

#ifdef EMU_M68K
		m68k_pulse_reset();
#endif

}

// ----------------------------------------------------------------------------
// Control the active CPU

// Open a CPU
void SekOpen(const INT32 i)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekOpen called without init\n"));
	if (i > nSekCount) bprintf(PRINT_ERROR, _T("SekOpen called with invalid index %x\n"), i);
	if (nSekActive != -1) bprintf(PRINT_ERROR, _T("SekOpen called when CPU already open with index %x\n"), i);
#endif

	if (i != nSekActive) {
		nSekActive = i;

		pSekExt = SekExt[nSekActive];						// Point to cpu context

#ifdef EMU_M68K
			m68k_set_context(SekM68KContext[nSekActive]);
#endif
		nSekCyclesTotal = nSekCycles[nSekActive];
	}
}

// Close the active cpu
void SekClose()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekClose called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekClose called when no CPU open\n"));
#endif

#ifdef EMU_M68K
		m68k_get_context(SekM68KContext[nSekActive]);
#endif

	nSekCycles[nSekActive] = nSekCyclesTotal;
	
	nSekActive = -1;
}

// Get the current CPU
INT32 SekGetActive()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekGetActive called without init\n"));
#endif

	return nSekActive;
}

// Set the status of an IRQ line on the active CPU
void SekSetIRQLine(const INT32 line, const INT32 status)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetIRQLine called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetIRQLine called when no CPU open\n"));
#endif

//	bprintf(PRINT_NORMAL, _T("  - irq line %i -> %i\n"), line, status);

	if (status) {
		nSekIRQPending[nSekActive] = line | status;

#ifdef EMU_M68K
			m68k_set_irq(line);
#endif
		return;
	}

	nSekIRQPending[nSekActive] = 0;

#ifdef EMU_M68K
		m68k_set_irq(0);
#endif

}

// Adjust the active CPU's timeslice
void SekRunAdjust(const INT32 nCycles)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekRunAdjust called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekRunAdjust called when no CPU open\n"));
#endif

	if (nCycles < 0 && -1 < -nCycles) {
		SekRunEnd();
		return;
	}

#ifdef EMU_M68K
		nSekCyclesToDo += nCycles;
		m68k_modify_timeslice(nCycles);
#endif

}

// End the active CPU's timeslice
void SekRunEnd()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekRunEnd called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekRunEnd called when no CPU open\n"));
#endif

#ifdef EMU_M68K
		m68k_end_timeslice();
#endif

}

// Run the active CPU
INT32 SekRun(const INT32 nCycles)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekRun called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekRun called when no CPU open\n"));
#endif

#ifdef EMU_M68K
		nSekCyclesToDo = nCycles;

		nSekCyclesSegment = m68k_execute(nCycles);

		nSekCyclesTotal += nSekCyclesSegment;
		nSekCyclesToDo = -1;

		return nSekCyclesSegment;
#else
		return 0;
#endif

}

// ----------------------------------------------------------------------------
// Breakpoint support

void SekDbgDisableBreakpoints()
{
#if defined FBA_DEBUG && defined EMU_M68K
		m68k_set_instr_hook_callback(NULL);

		M68KReadByteDebug = M68KReadByte;
		M68KReadWordDebug = M68KReadWord;
		M68KReadLongDebug = M68KReadLong;

		M68KWriteByteDebug = M68KWriteByte;
		M68KWriteWordDebug = M68KWriteWord;
		M68KWriteLongDebug = M68KWriteLong;
#endif
}

#if defined (FBA_DEBUG)

void SekDbgEnableBreakpoints()
{
	if (BreakpointDataRead[0].address || BreakpointDataWrite[0].address || BreakpointFetch[0].address) {
		SekDbgDisableBreakpoints();

		if (BreakpointFetch[0].address) {
			m68k_set_instr_hook_callback(M68KCheckBreakpoint);
		}

		if (BreakpointDataRead[0].address) {
			M68KReadByteDebug = M68KReadByteBP;
			M68KReadWordDebug = M68KReadWordBP;
			M68KReadLongDebug = M68KReadLongBP;
		}

		if (BreakpointDataWrite[0].address) {
			M68KWriteByteDebug = M68KWriteByteBP;
			M68KWriteWordDebug = M68KWriteWordBP;
			M68KWriteLongDebug = M68KWriteLongBP;
		}
	} else {
		SekDbgDisableBreakpoints();
	}
}

void SekDbgEnableSingleStep()
{
#if defined FBA_DEBUG && defined EMU_M68K
	m68k_set_instr_hook_callback(M68KSingleStep);
#endif
}

INT32 SekDbgSetBreakpointDataRead(UINT32 nAddress, INT32 nIdentifier)
{
	for (INT32 i = 0; i < 8; i++) {
		if (BreakpointDataRead[i].id == nIdentifier) {

			if	(nAddress) {							// Change breakpoint
				BreakpointDataRead[i].address = nAddress;
			} else {									// Delete breakpoint
				for ( ; i < 8; i++) {
					BreakpointDataRead[i] = BreakpointDataRead[i + 1];
				}
			}

			SekDbgEnableBreakpoints();
			return 0;
		}
	}

	// No breakpoints present, add it to the 1st slot
	BreakpointDataRead[0].address = nAddress;
	BreakpointDataRead[0].id = nIdentifier;

	SekDbgEnableBreakpoints();
	return 0;
}

INT32 SekDbgSetBreakpointDataWrite(UINT32 nAddress, INT32 nIdentifier)
{
	for (INT32 i = 0; i < 8; i++) {
		if (BreakpointDataWrite[i].id == nIdentifier) {

			if (nAddress) {								// Change breakpoint
				BreakpointDataWrite[i].address = nAddress;
			} else {									// Delete breakpoint
				for ( ; i < 8; i++) {
					BreakpointDataWrite[i] = BreakpointDataWrite[i + 1];
				}
			}

			SekDbgEnableBreakpoints();
			return 0;
		}
	}

	// No breakpoints present, add it to the 1st slot
	BreakpointDataWrite[0].address = nAddress;
	BreakpointDataWrite[0].id = nIdentifier;

	SekDbgEnableBreakpoints();
	return 0;
}

INT32 SekDbgSetBreakpointFetch(UINT32 nAddress, INT32 nIdentifier)
{
	for (INT32 i = 0; i < 8; i++) {
		if (BreakpointFetch[i].id == nIdentifier) {

			if (nAddress) {								// Change breakpoint
				BreakpointFetch[i].address = nAddress;
			} else {									// Delete breakpoint
				for ( ; i < 8; i++) {
					BreakpointFetch[i] = BreakpointFetch[i + 1];
				}
			}

			SekDbgEnableBreakpoints();
			return 0;
		}
	}

	// No breakpoints present, add it to the 1st slot
	BreakpointFetch[0].address = nAddress;
	BreakpointFetch[0].id = nIdentifier;

	SekDbgEnableBreakpoints();
	return 0;
}

#endif

// ----------------------------------------------------------------------------
// Memory map setup

// Note - each page is 1 << SEK_BITS.
INT32 SekMapMemory(UINT8* pMemory, UINT32 nStart, UINT32 nEnd, INT32 nType)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekMapMemory called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekMapMemory called when no CPU open\n"));
#endif

	UINT8* Ptr = pMemory - nStart;
	UINT8** pMemMap = pSekExt->MemMap + (nStart >> SEK_SHIFT);

	// Special case for ROM banks
	if (nType == SM_ROM) {
		for (UINT32 i = (nStart & ~SEK_PAGEM); i <= nEnd; i += SEK_PAGE_SIZE, pMemMap++) {
			pMemMap[0]			  = Ptr + i;
			pMemMap[SEK_WADD * 2] = Ptr + i;
		}

		return 0;
	}

	for (UINT32 i = (nStart & ~SEK_PAGEM); i <= nEnd; i += SEK_PAGE_SIZE, pMemMap++) {

		if (nType & SM_READ) {					// Read
			pMemMap[0]			  = Ptr + i;
		}
		if (nType & SM_WRITE) {					// Write
			pMemMap[SEK_WADD]	  = Ptr + i;
		}
		if (nType & SM_FETCH) {					// Fetch
			pMemMap[SEK_WADD * 2] = Ptr + i;
		}
	}

	return 0;
}

INT32 SekMapHandler(uintptr_t nHandler, UINT32 nStart, UINT32 nEnd, INT32 nType)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekMapHander called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekMapHandler called when no CPU open\n"));
#endif

	UINT8** pMemMap = pSekExt->MemMap + (nStart >> SEK_SHIFT);

	// Add to memory map
	for (UINT32 i = (nStart & ~SEK_PAGEM); i <= nEnd; i += SEK_PAGE_SIZE, pMemMap++) {

		if (nType & SM_READ) {					// Read
			pMemMap[0]			  = (UINT8*)nHandler;
		}
		if (nType & SM_WRITE) {					// Write
			pMemMap[SEK_WADD]	  = (UINT8*)nHandler;
		}
		if (nType & SM_FETCH) {					// Fetch
			pMemMap[SEK_WADD * 2] = (UINT8*)nHandler;
		}
	}

	return 0;
}

// Set callbacks
INT32 SekSetResetCallback(pSekResetCallback pCallback)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetResetCallback called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetResetCallback called when no CPU open\n"));
#endif

	pSekExt->ResetCallback = pCallback;

	return 0;
}

INT32 SekSetRTECallback(pSekRTECallback pCallback)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetRTECallback called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetRTECallback called when no CPU open\n"));
#endif

	pSekExt->RTECallback = pCallback;

	return 0;
}

INT32 SekSetIrqCallback(pSekIrqCallback pCallback)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetIrqCallback called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetIrqCallback called when no CPU open\n"));
#endif

	pSekExt->IrqCallback = pCallback;

	return 0;
}

INT32 SekSetCmpCallback(pSekCmpCallback pCallback)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetCmpCallback called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetCmpCallback called when no CPU open\n"));
#endif

	pSekExt->CmpCallback = pCallback;

	return 0;
}

// Set handlers
INT32 SekSetReadByteHandler(INT32 i, pSekReadByteHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetReadByteHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetReadByteHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->ReadByte[i] = pHandler;

	return 0;
}

INT32 SekSetWriteByteHandler(INT32 i, pSekWriteByteHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetWriteByteHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetWriteByteHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->WriteByte[i] = pHandler;

	return 0;
}

INT32 SekSetReadWordHandler(INT32 i, pSekReadWordHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetReadWordHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetReadWordHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->ReadWord[i] = pHandler;

	return 0;
}

INT32 SekSetWriteWordHandler(INT32 i, pSekWriteWordHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetWriteWordHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetWriteWordHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->WriteWord[i] = pHandler;

	return 0;
}

INT32 SekSetReadLongHandler(INT32 i, pSekReadLongHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetReadLongHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetReadLongHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->ReadLong[i] = pHandler;

	return 0;
}

INT32 SekSetWriteLongHandler(INT32 i, pSekWriteLongHandler pHandler)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekSetWriteLongHandler called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekSetWriteLongHandler called when no CPU open\n"));
#endif

	if (i >= SEK_MAXHANDLER) {
		return 1;
	}

	pSekExt->WriteLong[i] = pHandler;

	return 0;
}

// ----------------------------------------------------------------------------
// Query register values

INT32 SekGetPC(INT32 n)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekGetPC called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekGetPC called when no CPU open\n"));
#endif

#ifdef EMU_M68K
		return m68k_get_reg(NULL, M68K_REG_PC);
#else
		return 0;
#endif

}

INT32 SekDbgGetCPUType()
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekDbgGetCPUType called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekDbgGetCPUType called when no CPU open\n"));
#endif

	switch (nSekCPUType[nSekActive]) {
		case 0:
		case 0x68000:
			return M68K_CPU_TYPE_68000;
		case 0x68010:
			return M68K_CPU_TYPE_68010;
		case 0x68EC020:
			return M68K_CPU_TYPE_68EC020;
	}

	return 0;
}

INT32 SekDbgGetPendingIRQ(void)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekDbgGetPendingIRQ called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekDbgGetPendingIRQ called when no CPU open\n"));
#endif

	return nSekIRQPending[nSekActive] & 7;
}

UINT32 SekDbgGetRegister(enum SekRegister nRegister)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekDbgGetRegister called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekDbgGetRegister called when no CPU open\n"));
#endif
	switch (nRegister) {
		case SEK_REG_D0:
			return m68k_get_reg(NULL, M68K_REG_D0);
		case SEK_REG_D1:
			return m68k_get_reg(NULL, M68K_REG_D1);
		case SEK_REG_D2:
			return m68k_get_reg(NULL, M68K_REG_D2);
		case SEK_REG_D3:
			return m68k_get_reg(NULL, M68K_REG_D3);
		case SEK_REG_D4:
			return m68k_get_reg(NULL, M68K_REG_D4);
		case SEK_REG_D5:
			return m68k_get_reg(NULL, M68K_REG_D5);
		case SEK_REG_D6:
			return m68k_get_reg(NULL, M68K_REG_D6);
		case SEK_REG_D7:
			return m68k_get_reg(NULL, M68K_REG_D7);

		case SEK_REG_A0:
			return m68k_get_reg(NULL, M68K_REG_A0);
		case SEK_REG_A1:
			return m68k_get_reg(NULL, M68K_REG_A1);
		case SEK_REG_A2:
			return m68k_get_reg(NULL, M68K_REG_A2);
		case SEK_REG_A3:
			return m68k_get_reg(NULL, M68K_REG_A3);
		case SEK_REG_A4:
			return m68k_get_reg(NULL, M68K_REG_A4);
		case SEK_REG_A5:
			return m68k_get_reg(NULL, M68K_REG_A5);
		case SEK_REG_A6:
			return m68k_get_reg(NULL, M68K_REG_A6);
		case SEK_REG_A7:
			return m68k_get_reg(NULL, M68K_REG_A7);

		case SEK_REG_PC:
			return m68k_get_reg(NULL, M68K_REG_PC);

		case SEK_REG_SR:
			return m68k_get_reg(NULL, M68K_REG_SR);

		case SEK_REG_SP:
			return m68k_get_reg(NULL, M68K_REG_SP);
		case SEK_REG_USP:
			return m68k_get_reg(NULL, M68K_REG_USP);
		case SEK_REG_ISP:
			return m68k_get_reg(NULL, M68K_REG_ISP);
		case SEK_REG_MSP:
			return m68k_get_reg(NULL, M68K_REG_MSP);

		case SEK_REG_VBR:
			return m68k_get_reg(NULL, M68K_REG_VBR);

		case SEK_REG_SFC:
			return m68k_get_reg(NULL, M68K_REG_SFC);
		case SEK_REG_DFC:
			return m68k_get_reg(NULL, M68K_REG_DFC);

		case SEK_REG_CACR:
			return m68k_get_reg(NULL, M68K_REG_CACR);
		case SEK_REG_CAAR:
			return m68k_get_reg(NULL, M68K_REG_CAAR);

		default:
			return 0;
	}
}

BOOL SekDbgSetRegister(enum SekRegister nRegister, UINT32 nValue)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekDbgSetRegister called without init\n"));
	if (nSekActive == -1) bprintf(PRINT_ERROR, _T("SekDbgSetRegister called when no CPU open\n"));
#endif

	switch (nRegister) {
		case SEK_REG_D0:
		case SEK_REG_D1:
		case SEK_REG_D2:
		case SEK_REG_D3:
		case SEK_REG_D4:
		case SEK_REG_D5:
		case SEK_REG_D6:
		case SEK_REG_D7:
			break;

		case SEK_REG_A0:
		case SEK_REG_A1:
		case SEK_REG_A2:
		case SEK_REG_A3:
		case SEK_REG_A4:
		case SEK_REG_A5:
		case SEK_REG_A6:
		case SEK_REG_A7:
			break;

		case SEK_REG_PC:
			if (nSekCPUType[nSekActive] != 0) {
				m68k_set_reg(M68K_REG_PC, nValue);
			}
			SekClose();
			return TRUE;

		case SEK_REG_SR:
			break;

		case SEK_REG_SP:
		case SEK_REG_USP:
		case SEK_REG_ISP:
		case SEK_REG_MSP:
			break;

		case SEK_REG_VBR:
			break;

		case SEK_REG_SFC:
		case SEK_REG_DFC:
			break;

		case SEK_REG_CACR:
		case SEK_REG_CAAR:
			break;

		default:
			break;
	}

	return FALSE;
}

// ----------------------------------------------------------------------------
// Savestate support

INT32 SekScan(INT32 nAction)
{
#if defined FBA_DEBUG
	if (!DebugCPU_SekInitted) bprintf(PRINT_ERROR, _T("SekScan called without init\n"));
#endif

	// Scan the 68000 states
	struct BurnArea ba;

	if ((nAction & ACB_DRIVER_DATA) == 0) {
		return 1;
	}

	memset(&ba, 0, sizeof(ba));

	nSekActive = -1;

	for (INT32 i = 0; i <= nSekCount; i++) {
		char szName[] = "MC68000 #n";
		szName[9] = '0' + i;

		SCAN_VAR(nSekCPUType[i]);
		SCAN_VAR(nSekIRQPending[i]); // fix for Gradius 2 s.states -dink feb.3.2015

#ifdef EMU_M68K
			if (nSekCPUType[i] != 0) {
				ba.Data = SekM68KContext[i];

            // for savestate portability: preserve our cpu's pointers, they are set up in DrvInit() and can be specific to different systems.
  				// Therefore we scan the cpu context structure up until right before the pointers
  				ba.nLen = m68k_context_size_no_pointers();
				ba.szName = szName;
				BurnAcb(&ba);
			}
#endif
	}
	return 0;
}
