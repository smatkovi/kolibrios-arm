/* kfar_arc for KolibriOS for ARM: the PPMd decoder for *.7z archives
 * (ppmd.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/ppmd.inc,
 * ported there from 7-Zip's C++ sources (c) Igor Pavlov, which are based
 * on Dmitry Shkarin's PPMd var.H, by diamond (Evgeny Grechnikov),
 * GPL-2.0 (kfar_arc.h).
 *
 * The model lives in the decoder's memory after its data (Base, the
 * archive's memory size): the sub-allocator's units of 12 bytes, the
 * contexts (NumStats, SummFreq, Stats, Suffix; a context of one state
 * keeps it at SummFreq) and their states (Symbol, Freq, Successor: 6
 * bytes, the successor unaligned) with pointers in them, as the
 * original's; the symbols masked by CharMask's stamps (EscCount). The
 * labels are CInfo's, CDecodeInfo's and CSubAllocator's methods, the
 * original's comments name them. */
#include "kfar_arc.h"

/* CSubAllocator constants */
#define N1 4
#define N2 4
#define N3 4
#define N4 ((128 + 3 - 1 * N1 - 2 * N2 - 3 * N3) / 4)
#define UNIT_SIZE 12
#define N_INDEXES (N1 + N2 + N3 + N4)
#define kExtraSize (UNIT_SIZE * 3)
#define kMaxMemBlockSize (0xFFFFFFFFu - kExtraSize)
/* Context constants */
#define INT_BITS 7
#define PERIOD_BITS 7
#define TOT_BITS (INT_BITS + PERIOD_BITS)
#define INTERVAL (1 << INT_BITS)
#define BIN_SCALE (1 << TOT_BITS)
#define MAX_FREQ 124
#define MAX_O 255

static u8 NS2Indx[256], NS2BSIndx[256], HB2Flag[256], Indx2Units[N_INDEXES], Units2Indx[128];
static const u16 InitBinEsc[8] = { 0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051 };
static const u8 ExpEscape[16] = { 25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2 };

/* SEE2_CONTEXT */
typedef struct see2 { u16 Summ; u8 Shift; u8 Count; } see2;

typedef struct ppmd_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	/* RangeDecoder data */
	u32 inLen;
	const u8 *inPtr;
	u32 code;
	u32 range;
	u32 outSize;            /* number of bytes rest for output */
	/* PPMD data */
	u8 order;
	u8 GlueCount;
	u8 bInited;
	u8 pad;
	u32 usedMemorySize;
	/* CSubAllocator data */
	u8 *HeapStart;
	u8 *LoUnit;
	u8 *HiUnit;
	u8 *pText;
	u8 *UnitsStart;
	u8 *FreeList[N_INDEXES];
	/* CDecodeInfo (inherits from CInfo) data */
	see2 SEE2Cont[25][16];
	see2 DummySEE2Cont;
	u8 *MinContext;
	u8 *MaxContext;
	u8 *FoundState;         /* found next state transition */
	u32 NumMasked;
	u32 InitEsc;
	s32 OrderFall;
	s32 RunLength;
	s32 InitRL;
	u8 CharMask[256];
	u8 EscCount;
	u8 PrintCount;
	u8 PrevSuccess;
	u8 HiBitsFlag;
	u16 BinSumm[128][64];
	u8 Base[] __attribute__((aligned(4)));  /* kExtraSize + usedMemorySize */
} ppmd_decoder;

/* the records: a state (6 bytes), a context (UNIT_SIZE), a free block */
#define ST_SYMBOL(s) ((s)[0])
#define ST_FREQ(s) ((s)[1])
static inline u8 *ST_SUCC(const u8 *s) { u32 v; memcpy(&v, s + 2, 4); return (u8 *)(uintptr_t)v; }
static inline void ST_SET_SUCC(u8 *s, const void *p) { u32 v = (u32)(uintptr_t)p; memcpy(s + 2, &v, 4); }
#define CTX_NUMSTATS(c) (*(u16 *)(c))
#define CTX_SUMMFREQ(c) (*(u16 *)((c) + 2))
#define CTX_STATS(c) (*(u8 **)((c) + 4))
#define CTX_SUFFIX(c) (*(u8 **)((c) + 8))
#define CTX_ONESTATE(c) ((c) + 2)
/* MEM_BLK of GlueFreeBlocks: Stamp, NU, next, prev */
#define BLK_STAMP(b) (*(u16 *)(b))
#define BLK_NU(b) (*(u16 *)((b) + 2))
#define BLK_NEXT(b) (*(u8 **)((b) + 4))
#define BLK_PREV(b) (*(u8 **)((b) + 8))
#define U2B(nu) ((u32)(nu) * UNIT_SIZE)

/* ppmd.inc: init_ppmd: the tables */
void init_ppmd(void)
{
	/* NS2Indx table */
	u32 i, k, m;
	for (i = 0; i < 3; i++) NS2Indx[i] = i;
	for (m = i, k = 1; i < 256; i++) {
		NS2Indx[i] = m;
		if (!--k) k = ++m - 2;
	}
	/* NS2BSIndx table */
	NS2BSIndx[0] = 0;
	NS2BSIndx[1] = 2;
	memset(NS2BSIndx + 2, 4, 9);
	memset(NS2BSIndx + 11, 6, 256 - 11);
	/* HB2Flag table */
	memset(HB2Flag, 0, 0x40);
	memset(HB2Flag + 0x40, 8, 0x100 - 0x40);
	/* Indx2Units table: 1-4, 6-12 by 2, 15-24 by 3, 28-128 by 4 */
	static const u8 first[12] = { 1, 2, 3, 4, 6, 8, 10, 12, 15, 18, 21, 24 };
	memcpy(Indx2Units, first, 12);
	for (i = 12, k = 28; i < N_INDEXES; i++, k += 4) Indx2Units[i] = k;
	/* Units2Indx table */
	for (i = 0, k = 1; k <= 128; k++) {
		if (Indx2Units[i] < k) i++;
		Units2Indx[k - 1] = i;
	}
}

/* ---- CSubAllocator ---- */

static void InsertNode(ppmd_decoder *ebp, u8 *p, u32 indx)
{
	*(u8 **)p = ebp->FreeList[indx];
	ebp->FreeList[indx] = p;
}

static u8 *RemoveNode(ppmd_decoder *ebp, u32 indx)
{
	u8 *p = ebp->FreeList[indx];
	ebp->FreeList[indx] = *(u8 **)p;
	return p;
}

/* CSubAllocator::SplitBlock(pv, oldIndx, newIndx) */
static void SplitBlock(ppmd_decoder *ebp, u8 *pv, u32 OldIndx, u32 NewIndx)
{
	u32 UDiff = Indx2Units[OldIndx] - Indx2Units[NewIndx];
	u8 *p = pv + U2B(Indx2Units[NewIndx]);
	u32 i = Units2Indx[UDiff - 1];
	if (Indx2Units[i] != UDiff) {
		InsertNode(ebp, p, --i);
		p += U2B(i = Indx2Units[i]);
		UDiff -= i;
	}
	/* .aur2 */
	InsertNode(ebp, p, Units2Indx[UDiff - 1]);
}

/* CSubAllocator::GlueFreeBlocks, called from AllocUnitsRare: the free
 * blocks in one list (s0, after the heap's end: an extra MEM_BLK with
 * Stamp = 0 before it), the neighbours joined, the lists made again */
static void GlueFreeBlocks(ppmd_decoder *ebp)
{
	ebp->GlueCount = 255;
	u8 *end = ebp->HeapStart + ebp->usedMemorySize;
	BLK_STAMP(end) = 0;
	u8 *s0 = end + UNIT_SIZE;
	if (ebp->LoUnit != ebp->HiUnit) *ebp->LoUnit = 0;
	BLK_NEXT(s0) = BLK_PREV(s0) = s0;
	for (u32 i = 0; i < N_INDEXES; i++)
		while (ebp->FreeList[i]) {
			/* .gfb1: p->insertAt(&s0) */
			u8 *p = RemoveNode(ebp, i);
			BLK_PREV(p) = s0;
			BLK_NEXT(p) = BLK_NEXT(s0);
			BLK_PREV(BLK_NEXT(s0)) = p;
			BLK_NEXT(s0) = p;
			BLK_STAMP(p) = 0xFFFF;
			BLK_NU(p) = Indx2Units[i];
		}
	/* .gfb3: join each block with the free ones after it */
	for (u8 *p = BLK_NEXT(s0); p != s0; p = BLK_NEXT(p))
		for (;;) {
			/* .gfb4 */
			u8 *p1 = p + U2B(BLK_NU(p));
			if (BLK_STAMP(p1) != 0xFFFF || BLK_NU(p) + BLK_NU(p1) >= 0x10000) break;
			BLK_NEXT(BLK_PREV(p1)) = BLK_NEXT(p1);
			BLK_PREV(BLK_NEXT(p1)) = BLK_PREV(p1);
			BLK_NU(p) += BLK_NU(p1);
		}
	/* .gfb5: back into the lists */
	for (u8 *p; (p = BLK_NEXT(s0)) != s0;) {
		BLK_PREV(BLK_NEXT(p)) = s0;
		BLK_NEXT(s0) = BLK_NEXT(p);
		u32 sz = BLK_NU(p);
		for (; sz > 128; sz -= 128, p += U2B(128)) InsertNode(ebp, p, N_INDEXES - 1);     /* .gfb6 */
		/* .gfb7 */
		u32 i = Units2Indx[sz - 1];
		if (Indx2Units[i] != sz) {
			u32 k = sz - Indx2Units[--i];
			InsertNode(ebp, p + U2B(sz - k), k - 1);
		}
		InsertNode(ebp, p, i);
	}
}

/* CSubAllocator::AllocUnitsRare(indx) */
static u8 *AllocUnitsRare(ppmd_decoder *ebp, u32 indx)
{
	if (!ebp->GlueCount) {
		GlueFreeBlocks(ebp);
		if (ebp->FreeList[indx]) return RemoveNode(ebp, indx);
	}
	/* .aur.cont */
	u32 i = indx;
	do {
		/* .aur1 */
		if (++i == N_INDEXES) {
			/* .aur3: from the text's end */
			ebp->GlueCount--;
			u32 n = U2B(Indx2Units[indx]);
			if ((u32)(ebp->UnitsStart - ebp->pText) > n) return ebp->UnitsStart -= n;
			return 0;               /* .aur4 */
		}
	} while (!ebp->FreeList[i]);
	u8 *RetVal = RemoveNode(ebp, i);
	SplitBlock(ebp, RetVal, i, indx);
	return RetVal;
}

/* CSubAllocator::AllocContext */
static u8 *AllocContext(ppmd_decoder *ebp)
{
	if (ebp->HiUnit != ebp->LoUnit) return ebp->HiUnit -= UNIT_SIZE;
	if (ebp->FreeList[0]) return RemoveNode(ebp, 0);
	return AllocUnitsRare(ebp, 0);
}

/* CSubAllocator::AllocUnits(ecx = NU) */
static u8 *AllocUnits(ppmd_decoder *ebp, u32 NU)
{
	u32 indx = Units2Indx[NU - 1];
	if (ebp->FreeList[indx]) return RemoveNode(ebp, indx);
	u8 *RetVal = ebp->LoUnit;
	if (RetVal + U2B(Indx2Units[indx]) <= ebp->HiUnit) {
		ebp->LoUnit = RetVal + U2B(Indx2Units[indx]);
		return RetVal;
	}
	return AllocUnitsRare(ebp, indx);
}

/* CSubAllocator::ExpandUnits(oldPtr, oldNU) */
static u8 *ExpandUnits(ppmd_decoder *ebp, u8 *OldPtr, u32 OldNU)
{
	u32 i0 = Units2Indx[OldNU - 1], i1 = Units2Indx[OldNU];
	if (i0 == i1) return OldPtr;
	u8 *ptr = AllocUnits(ebp, OldNU + 1);
	if (ptr) {
		memcpy(ptr, OldPtr, U2B(OldNU));
		InsertNode(ebp, OldPtr, i0);
	}
	return ptr;
}

/* CSubAllocator::ShrinkUnits(oldPtr, oldNU, newNU) */
static u8 *ShrinkUnits(ppmd_decoder *ebp, u8 *OldPtr, u32 OldNU, u32 NewNU)
{
	u32 i0 = Units2Indx[OldNU - 1], i1 = Units2Indx[NewNU - 1];
	if (i0 == i1) return OldPtr;
	if (ebp->FreeList[i1]) {
		u8 *ptr = RemoveNode(ebp, i1);
		memcpy(ptr, OldPtr, U2B(NewNU));
		InsertNode(ebp, OldPtr, i0);
		return ptr;
	}
	SplitBlock(ebp, OldPtr, i0, i1);
	return OldPtr;
}

/* CSubAllocator::FreeUnits(ptr, oldNU) */
static void FreeUnits(ppmd_decoder *ebp, u8 *ptr, u32 OldNU)
{
	InsertNode(ebp, ptr, Units2Indx[OldNU - 1]);
}

/* ---- CInfo ---- */

/* a state's six bytes swapped with another's */
static void swap_states(u8 *a, u8 *b)
{
	u8 t[6];
	memcpy(t, a, 6);
	memcpy(a, b, 6);
	memcpy(b, t, 6);
}

/* CInfo::RestartModelRare */
static void RestartModelRare(ppmd_decoder *ebp)
{
	memset(ebp->CharMask, 0, sizeof(ebp->CharMask));
	/* CSubAllocator::InitSubAllocator */
	ebp->GlueCount = 0;
	memset(ebp->FreeList, 0, sizeof(ebp->FreeList));
	ebp->pText = ebp->HeapStart;
	ebp->HiUnit = ebp->HeapStart + ebp->usedMemorySize;
	ebp->LoUnit = ebp->UnitsStart = ebp->HiUnit - ebp->usedMemorySize / (8 * UNIT_SIZE) * 7 * UNIT_SIZE;
	/* (end) */
	u32 o = ebp->order < 12 ? ebp->order : 12;
	ebp->InitRL = ebp->RunLength = -(s32)o - 1;
	u8 *c = AllocContext(ebp);
	ebp->MinContext = ebp->MaxContext = c;
	CTX_SUFFIX(c) = 0;
	ebp->OrderFall = ebp->order;
	CTX_NUMSTATS(c) = 256;
	CTX_SUMMFREQ(c) = 256 + 1;
	u8 *s = AllocUnits(ebp, 256 / 2);
	ebp->FoundState = s;
	CTX_STATS(c) = s;
	ebp->PrevSuccess = 0;
	for (u32 i = 0; i < 256; i++, s += 6) {
		ST_SYMBOL(s) = i;
		ST_FREQ(s) = 1;
		ST_SET_SUCC(s, 0);
	}
	/* .rmr1 */
	for (u32 i = 0; i < 128; i++)
		for (u32 k = 0; k < 8; k++) {
			u16 v = BIN_SCALE - InitBinEsc[k] / (i + 2);
			for (u32 m = 0; m < 64; m += 8) ebp->BinSumm[i][k + m] = v;
		}
	for (u32 i = 0; i < 25; i++)
		for (u32 k = 0; k < 16; k++) {
			ebp->SEE2Cont[i][k].Summ = (5 * i + 10) << (PERIOD_BITS - 4);
			ebp->SEE2Cont[i][k].Shift = PERIOD_BITS - 4;
			ebp->SEE2Cont[i][k].Count = 4;
		}
}

/* CInfo::StartModelRare(.order) (an order below 2: 7-Zip never makes
 * them, the original says, and the C++ code fails on them: a data error) */
static void StartModelRare(ppmd_decoder *ebp)
{
	ebp->EscCount = 1;
	ebp->PrintCount = 1;
	if (ebp->order < 2) return_err();
	ebp->DummySEE2Cont.Shift = PERIOD_BITS;
	RestartModelRare(ebp);
}

/* RestartModel: CInfo::UpdateModel's RESTART_MODEL */
static void RestartModel(ppmd_decoder *ebp)
{
	RestartModelRare(ebp);
	ebp->EscCount = 0;
	ebp->PrintCount = 0xFF;
}

/* CInfo::CreateSuccessors(skip, p1) */
static u8 *CreateSuccessors(ppmd_decoder *ebp, bool skip, u8 *p1)
{
	u8 *pc = ebp->MinContext, *UpBranch = ST_SUCC(ebp->FoundState);
	u8 *ps[MAX_O];
	u32 n = 0;
	u8 *p;
	if (!skip) {
		ps[n++] = ebp->FoundState;
		if (!CTX_SUFFIX(pc)) goto NO_LOOP;
	}
	if (p1) {
		p = p1;
		pc = CTX_SUFFIX(pc);
		goto LOOP_ENTRY;
	}
	do {
		/* .csloopstart */
		pc = CTX_SUFFIX(pc);
		if (CTX_NUMSTATS(pc) != 1) {
			p = CTX_STATS(pc);
			while (ST_SYMBOL(p) != ST_SYMBOL(ebp->FoundState)) p += 6;
		} else {
			p = CTX_ONESTATE(pc);
		}
	LOOP_ENTRY:
		if (ST_SUCC(p) != UpBranch) {
			pc = ST_SUCC(p);
			break;
		}
		ps[n++] = p;
	} while (CTX_SUFFIX(pc));
NO_LOOP:
	if (!n) return pc;
	/* the new contexts' one state */
	u8 up[6];
	ST_SYMBOL(up) = *UpBranch;
	ST_SET_SUCC(up, UpBranch + 1);
	if (CTX_NUMSTATS(pc) != 1) {
		p = CTX_STATS(pc);
		while (ST_SYMBOL(p) != ST_SYMBOL(up)) p += 6;
		u32 cf = ST_FREQ(p) - 1;
		u32 s0 = CTX_SUMMFREQ(pc) - CTX_NUMSTATS(pc) - cf;
		ST_FREQ(up) = 1 + (2 * cf <= s0 ? 5 * cf > s0 : (2 * cf + 3 * s0 - 1) / (2 * s0));
	} else {
		ST_FREQ(up) = ST_FREQ(CTX_ONESTATE(pc));
	}
	do {
		/* .cs3: PPM_CONTEXT::createChild(this, pStats, FirstState) */
		u8 *c = AllocContext(ebp);
		if (!c) return 0;
		CTX_NUMSTATS(c) = 1;
		memcpy(CTX_ONESTATE(c), up, 6);
		CTX_SUFFIX(c) = pc;
		ST_SET_SUCC(ps[--n], c);
		pc = c;
	} while (n);
	return pc;
}

/* CInfo::UpdateModel */
static void UpdateModel(ppmd_decoder *ebp)
{
	u8 fs[6], *p = 0, *pc, *Successor;
	memcpy(fs, ebp->FoundState, 6);
	if (ST_FREQ(fs) < MAX_FREQ / 4 && (pc = CTX_SUFFIX(ebp->MinContext))) {
		if (CTX_NUMSTATS(pc) != 1) {
			p = CTX_STATS(pc);
			if (ST_SYMBOL(p) != ST_SYMBOL(fs)) {
				do p += 6; while (ST_SYMBOL(p) != ST_SYMBOL(fs));
				if (ST_FREQ(p) >= ST_FREQ(p - 6)) {
					swap_states(p, p - 6);
					p -= 6;
				}
			}
			/* .um0 */
			if (ST_FREQ(p) < MAX_FREQ - 9) {
				ST_FREQ(p) += 2;
				CTX_SUMMFREQ(pc) += 2;
			}
		} else {
			/* .um1 */
			p = CTX_ONESTATE(pc);
			ST_FREQ(p) += ST_FREQ(p) < 32;
		}
	}
	/* .um2 */
	if (!ebp->OrderFall) {
		u8 *c = CreateSuccessors(ebp, true, p);
		ebp->MinContext = ebp->MaxContext = c;
		ST_SET_SUCC(ebp->FoundState, c);
		if (!c) RestartModel(ebp);
		return;
	}
	/* .um3 */
	*ebp->pText++ = ST_SYMBOL(fs);
	Successor = ebp->pText;
	if (ebp->pText >= ebp->UnitsStart) {
		RestartModel(ebp);
		return;
	}
	if (ST_SUCC(fs)) {
		if (ST_SUCC(fs) <= ebp->pText) {
			u8 *c = CreateSuccessors(ebp, false, p);
			ST_SET_SUCC(fs, c);
			if (!c) {
				RestartModel(ebp);
				return;
			}
		}
		if (!--ebp->OrderFall) {
			Successor = ST_SUCC(fs);
			ebp->pText -= ebp->MaxContext != ebp->MinContext;
		}
	} else {
		/* .um4 */
		ST_SET_SUCC(ebp->FoundState, Successor);
		ST_SET_SUCC(fs, ebp->MinContext);
	}
	/* .um5 */
	u32 ns = CTX_NUMSTATS(ebp->MinContext);
	u32 s0 = CTX_SUMMFREQ(ebp->MinContext) - ns - (ST_FREQ(fs) - 1);
	for (pc = ebp->MaxContext; pc != ebp->MinContext; pc = CTX_SUFFIX(pc)) {
		/* .um6 */
		u32 ns1 = CTX_NUMSTATS(pc);
		if (ns1 != 1) {
			if (!(ns1 & 1)) {
				u8 *ppp = ExpandUnits(ebp, CTX_STATS(pc), ns1 >> 1);
				if (!ppp) {
					RestartModel(ebp);
					return;
				}
				CTX_STATS(pc) = ppp;
			}
			/* .um7 */
			CTX_SUMMFREQ(pc) += (2 * ns1 < ns) + 2 * ((4 * ns1 <= ns) & (CTX_SUMMFREQ(pc) <= 8 * ns1));
		} else {
			/* .um8 */
			p = AllocUnits(ebp, 1);
			if (!p) {
				RestartModel(ebp);
				return;
			}
			memcpy(p, CTX_ONESTATE(pc), 6);
			CTX_STATS(pc) = p;
			if (ST_FREQ(p) < MAX_FREQ / 4 - 1) ST_FREQ(p) += ST_FREQ(p);
			else ST_FREQ(p) = MAX_FREQ - 4;
			CTX_SUMMFREQ(pc) = ST_FREQ(p) + ebp->InitEsc + (ns > 3);
		}
		/* .um9 */
		u32 cf = 2 * ST_FREQ(fs) * (CTX_SUMMFREQ(pc) + 6);
		u32 sf = s0 + CTX_SUMMFREQ(pc);
		if (cf < 6 * sf) {
			cf = 1 + (cf > sf) + (cf >= 4 * sf);
			CTX_SUMMFREQ(pc) += 3;
		} else {
			/* .um10 */
			cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
			CTX_SUMMFREQ(pc) += cf;
		}
		/* .um11 */
		p = CTX_STATS(pc) + ns1 * 6;
		ST_SET_SUCC(p, Successor);
		ST_SYMBOL(p) = ST_SYMBOL(fs);
		ST_FREQ(p) = cf;
		CTX_NUMSTATS(pc) = ++ns1;
	}
	/* .um12 */
	ebp->MaxContext = ebp->MinContext = ST_SUCC(fs);
}

/* rescale: the frequencies halved, the states sorted again, the zero
 * ones dropped */
static void rescale(ppmd_decoder *ebp)
{
	u8 *esi = ebp->MinContext;
	u32 OldNS = CTX_NUMSTATS(esi), ecx = OldNS - 1;
	u8 *stats = CTX_STATS(esi), *ebx = ebp->FoundState;
	/* .r0: the found state to the front */
	for (; ebx != stats; ebx -= 6) swap_states(ebx, ebx - 6);
	/* .r1 */
	ST_FREQ(ebx) += 4;
	CTX_SUMMFREQ(esi) += 4;
	u32 EscFreq = CTX_SUMMFREQ(esi) - ST_FREQ(ebx);
	u32 adder = ebp->OrderFall != 0;
	ST_FREQ(ebx) = (ST_FREQ(ebx) + adder) >> 1;
	CTX_SUMMFREQ(esi) = ST_FREQ(ebx);
	do {
		/* .r2 */
		ebx += 6;
		EscFreq -= ST_FREQ(ebx);
		ST_FREQ(ebx) = (ST_FREQ(ebx) + adder) >> 1;
		CTX_SUMMFREQ(esi) += ST_FREQ(ebx);
		if (ST_FREQ(ebx) > ST_FREQ(ebx - 6)) {
			u8 t[6], *s1 = ebx;
			memcpy(t, s1, 6);
			do {
				memcpy(s1, s1 - 6, 6);
				s1 -= 6;
			} while (s1 != stats && ST_FREQ(t) > ST_FREQ(s1 - 6));
			memcpy(s1, t, 6);
		}
		/* .r3 */
	} while (--ecx);
	if (!ST_FREQ(ebx)) {
		do {
			ecx++;
			ebx -= 6;
		} while (!ST_FREQ(ebx));
		EscFreq += ecx;
		CTX_NUMSTATS(esi) -= ecx;
		if (CTX_NUMSTATS(esi) == 1) {
			u8 t[6];
			memcpy(t, stats, 6);
			do {
				ST_FREQ(t) = (ST_FREQ(t) + 1) >> 1;
				EscFreq >>= 1;
			} while (EscFreq > 1);
			FreeUnits(ebp, stats, (OldNS + 1) >> 1);
			ebp->FoundState = CTX_ONESTATE(esi);
			memcpy(ebp->FoundState, t, 6);
			return;
		}
	}
	/* .r4 */
	CTX_SUMMFREQ(esi) += (EscFreq + 1) >> 1;
	u32 n0 = (OldNS + 1) >> 1, n1 = (CTX_NUMSTATS(esi) + 1) >> 1;
	if (n0 != n1) CTX_STATS(esi) = ShrinkUnits(ebp, stats, n0, n1);
	ebp->FoundState = CTX_STATS(esi);
}

/* ---- CRangeDecoder (7-Zip's) ---- */

/* ppmd_decoder.refill */
static void ppmd_refill(ppmd_decoder *ebp)
{
	streamInfo *eax = ebp->inStream;
	fillBuf(eax);
	ebp->inPtr = eax->bufPtr;
	if (!eax->bufDataLen) return_err();
	ebp->inLen = eax->bufDataLen - 1;
}

/* RangeDecoder.Normalize */
static void Normalize(ppmd_decoder *ebp)
{
	while (ebp->range < 1u << 24) {
		if (ebp->inLen-- == 0) ppmd_refill(ebp);
		ebp->range <<= 8;
		ebp->code = ebp->code << 8 | *ebp->inPtr++;
	}
}

/* RangeDecoder.Decode(start, size) */
static void RangeDecoder_Decode(ppmd_decoder *ebp, u32 start, u32 size)
{
	ebp->code -= start * ebp->range;
	ebp->range *= size;
	Normalize(ebp);
}

/* RangeDecoder.GetThreshold(total) */
static u32 GetThreshold(ppmd_decoder *ebp, u32 total)
{
	ebp->range /= total;
	return ebp->code / ebp->range;
}

/* RangeDecoder.DecodeBit(size0, numTotalBits = .TOT_BITS) */
static u32 RangeDecoder_DecodeBit(ppmd_decoder *ebp, u32 size0)
{
	u32 bound = (ebp->range >> TOT_BITS) * size0;
	if (ebp->code < bound) {
		ebp->range = bound;
		Normalize(ebp);
		return 0;
	}
	ebp->code -= bound;
	ebp->range -= bound;
	Normalize(ebp);
	return 1;
}

/* ---- CDecodeInfo ---- */

/* CInfo::GetBinSumm(rs, numStates) */
static u16 *GetBinSumm(ppmd_decoder *ebp, const u8 *rs, u32 numStates)
{
	ebp->HiBitsFlag = HB2Flag[ST_SYMBOL(ebp->FoundState)];
	u32 i = ebp->PrevSuccess + NS2BSIndx[numStates - 1] + ebp->HiBitsFlag + 2 * HB2Flag[ST_SYMBOL(rs)] +
		((u32)ebp->RunLength >> 26 & 0x20);
	return &ebp->BinSumm[ST_FREQ(rs) - 1][i];
}

/* CInfo::makeEscFreq2(Diff): the SEE context and the escape's frequency */
static see2 *makeEscFreq2(ppmd_decoder *ebp, u32 Diff, u32 *scale)
{
	u8 *mc = ebp->MinContext;
	if (CTX_NUMSTATS(mc) == 256) {
		/* .mef0 */
		*scale = 1;
		return &ebp->DummySEE2Cont;
	}
	u32 n = CTX_NUMSTATS(mc);
	see2 *s = &ebp->SEE2Cont[NS2Indx[Diff - 1]][(Diff < (u32)CTX_NUMSTATS(CTX_SUFFIX(mc)) - n) +
		2 * (CTX_SUMMFREQ(mc) < 11 * n) + 4 * (Diff < ebp->NumMasked) + ebp->HiBitsFlag];
	u32 r = s->Summ >> s->Shift;
	s->Summ -= r;
	*scale = r + (r == 0);
	return s;
}

/* CInfo::NextContext */
static void NextContext(ppmd_decoder *ebp)
{
	u8 *ebx = ST_SUCC(ebp->FoundState);
	if (!ebp->OrderFall && ebx > ebp->pText) {
		ebp->MinContext = ebp->MaxContext = ebx;
		return;
	}
	/* .nc0 */
	UpdateModel(ebp);
	if (!ebp->EscCount) {
		ebp->EscCount = 1;
		memset(ebp->CharMask, 0, sizeof(ebp->CharMask));
	}
}

/* CDecodeInfo::DecodeSymbol: the next byte */
static u8 DecodeSymbol(ppmd_decoder *ebp)
{
	u8 *esi = ebp->MinContext;
	if (CTX_NUMSTATS(esi) != 1) {
		/* CDecodeInfo::DecodeSymbol1 */
		u8 *ebx = CTX_STATS(esi);
		u32 count = GetThreshold(ebp, CTX_SUMMFREQ(esi));
		u32 hiCnt = ST_FREQ(ebx);
		if (count < hiCnt) {
			ebp->PrevSuccess = 2 * hiCnt > CTX_SUMMFREQ(esi);
			ebp->RunLength += ebp->PrevSuccess;
			RangeDecoder_Decode(ebp, 0, hiCnt);
			ebp->FoundState = ebx;
			ST_FREQ(ebx) = hiCnt + 4;
			CTX_SUMMFREQ(esi) += 4;
			if (hiCnt + 4 > MAX_FREQ) rescale(ebp);
			goto dsfnd;
		}
		/* .ds0 */
		ebp->PrevSuccess = 0;
		u32 i = CTX_NUMSTATS(esi) - 1;
		do {
			/* .ds1 */
			ebx += 6;
			hiCnt += ST_FREQ(ebx);
			if (hiCnt > count) {
				/* .ds2 */
				RangeDecoder_Decode(ebp, hiCnt - ST_FREQ(ebx), ST_FREQ(ebx));
				/* .update1 */
				ebp->FoundState = ebx;
				ST_FREQ(ebx) += 4;
				CTX_SUMMFREQ(esi) += 4;
				if (ST_FREQ(ebx) > ST_FREQ(ebx - 6)) {
					swap_states(ebx, ebx - 6);
					ebp->FoundState = ebx -= 6;
					if (ST_FREQ(ebx) > MAX_FREQ) rescale(ebp);
				}
				goto dsfnd;
			}
		} while (--i);
		/* an escape: all the context's symbols masked */
		ebp->HiBitsFlag = HB2Flag[ST_SYMBOL(ebp->FoundState)];
		RangeDecoder_Decode(ebp, hiCnt, CTX_SUMMFREQ(esi) - hiCnt);
		ebp->NumMasked = CTX_NUMSTATS(esi);
		for (u32 k = 0; k < CTX_NUMSTATS(esi); k++, ebx -= 6) ebp->CharMask[ST_SYMBOL(ebx)] = ebp->EscCount;
		ebp->FoundState = 0;
	} else {
		/* .binsymbol: CDecodeInfo::DecodeBinSymbol */
		u8 *rs = CTX_ONESTATE(esi);
		u16 *prob = GetBinSumm(ebp, rs, CTX_NUMSTATS(CTX_SUFFIX(esi)));
		if (!RangeDecoder_DecodeBit(ebp, *prob)) {
			ebp->FoundState = rs;
			ST_FREQ(rs) += ST_FREQ(rs) < 128;
			*prob -= ((*prob + (1 << (PERIOD_BITS - 2))) >> PERIOD_BITS) - INTERVAL;
			ebp->PrevSuccess = 1;
			ebp->RunLength++;
			goto dsfnd;
		}
		/* .ds3 */
		*prob -= (*prob + (1 << (PERIOD_BITS - 2))) >> PERIOD_BITS;
		ebp->InitEsc = ExpEscape[*prob >> 10];
		ebp->NumMasked = 1;
		ebp->CharMask[ST_SYMBOL(rs)] = ebp->EscCount;
		ebp->PrevSuccess = 0;
		ebp->FoundState = 0;
	}
	/* .dscmn */
	while (!ebp->FoundState) {
		/* .ds4 */
		do {
			ebp->OrderFall++;
			esi = CTX_SUFFIX(ebp->MinContext);
			if (!esi) return_err();         /* no end-of-stream mark */
			ebp->MinContext = esi;
		} while (CTX_NUMSTATS(esi) == ebp->NumMasked);
		/* CDecodeInfo::DecodeSymbol2 */
		u32 diff = CTX_NUMSTATS(esi) - ebp->NumMasked, escFreq;
		see2 *see = makeEscFreq2(ebp, diff, &escFreq);
		u8 *ps[256], *ebx = CTX_STATS(esi);
		u32 hiCnt = 0, n = 0;
		for (; n < diff; ebx += 6) {
			if (ebp->CharMask[ST_SYMBOL(ebx)] == ebp->EscCount) continue;
			hiCnt += ST_FREQ(ebx);
			ps[n++] = ebx;
		}
		u32 total = escFreq + hiCnt;
		u32 count = GetThreshold(ebp, total);
		if (count < hiCnt) {
			u32 k = 0, c = 0;
			for (;; k++) {
				c += ST_FREQ(ps[k]);
				if (count < c) break;
			}
			ebx = ps[k];
			RangeDecoder_Decode(ebp, c - ST_FREQ(ebx), ST_FREQ(ebx));
			/* the SEE context's update */
			if (see->Shift < PERIOD_BITS && !--see->Count) {
				see->Summ <<= 1;
				see->Count = 3 << see->Shift++;
			}
			/* .update2 */
			ebp->FoundState = ebx;
			ST_FREQ(ebx) += 4;
			CTX_SUMMFREQ(ebp->MinContext) += 4;
			if (ST_FREQ(ebx) > MAX_FREQ) rescale(ebp);
			ebp->EscCount++;
			ebp->RunLength = ebp->InitRL;
			break;
		}
		/* .ds5: an escape again */
		RangeDecoder_Decode(ebp, hiCnt, total - hiCnt);
		for (u32 k = 0; k < n; k++) ebp->CharMask[ST_SYMBOL(ps[k])] = ebp->EscCount;
		ebp->NumMasked = CTX_NUMSTATS(esi);
		see->Summ += total;
	}
dsfnd:;
	u8 al = ST_SYMBOL(ebp->FoundState);
	NextContext(ebp);
	return al;
}

/* ppmd_decoder.fillBuf */
static void ppmd_fillBuf(streamInfo *s, u8 *edi, u32 ecx)
{
	ppmd_decoder *ebp = (ppmd_decoder *)s;
	ebp->outSize = ecx;
	if (!ebp->bInited) {
		/* .init: the range coder's first five bytes, the model */
		streamInfo *eax = ebp->inStream;
		fillBuf(eax);
		if (eax->bufDataLen < 5) return_err();
		ebp->inLen = eax->bufDataLen - 5;
		const u8 *esi = eax->bufPtr + 1;
		ebp->code = (u32)esi[0] << 24 | esi[1] << 16 | esi[2] << 8 | esi[3];
		ebp->inPtr = esi + 4;
		ebp->range = (u32)-1;
		ebp->bInited = 1;
		StartModelRare(ebp);
	}
	/* .mainloop */
	for (; ebp->outSize; ebp->outSize--) *edi++ = DecodeSymbol(ebp);
}

/* ppmd.inc: ppmd_get_buf_size: the properties: db order, dd memory */
void ppmd_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx)
{
	if (params_size < 5) return_err();
	u32 mem = esi[1] | esi[2] << 8 | esi[3] << 16 | (u32)esi[4] << 24;
	if (mem > kMaxMemBlockSize - sizeof(ppmd_decoder) - 4) return_err();
	*eax = mem + sizeof(ppmd_decoder) + kExtraSize + 4;    /* (+4: HeapStart's alignment) */
	*edx = 0x4000;
}

/* ppmd.inc: ppmd_init_decoder: CDecoder::SetDecoderProperties2,
 * CSubAllocator::CSubAllocator, CSubAllocator::StartSubAllocator */
void ppmd_init_decoder(streamInfo *s, const u8 *esi, u32 params_size, int ebx)
{
	ppmd_decoder *ebp = (ppmd_decoder *)s;
	ebp->bInited = 0;
	ebp->order = esi[0];
	ebp->usedMemorySize = esi[1] | esi[2] << 8 | esi[3] << 16 | (u32)esi[4] << 24;
	ebp->GlueCount = 0;
	ebp->LoUnit = ebp->HiUnit = ebp->pText = ebp->UnitsStart = 0;
	memset(ebp->FreeList, 0, sizeof(ebp->FreeList));
	/* (the heap's end aligned to 4 bytes, so that the contexts are: as
	 * 7-Zip's AlignOffset; the units' places from the end are the same) */
	ebp->HeapStart = ebp->Base + UNIT_SIZE + (-ebp->usedMemorySize & 3);
	s->fillBuf = ppmd_fillBuf;
}
