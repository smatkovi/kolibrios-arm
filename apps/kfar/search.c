/* kfar for KolibriOS for ARM: search.inc: the finite automaton for the
 * search of a string (the viewer's, the editor's, the file search's).
 *
 * Translated to C from kfar's search.inc, diamond (Evgeny Grechnikov)
 * and the KolibriOS team, GPL-2.0.
 *
 * The original keeps its temporary tables on the stack (the lists of the
 * raw bytes for each character as nodes pushed one after another); here
 * they are arrays. The automaton's fallback rows are KMP's (see
 * .fsmcalc): the original's miss some matches. */
#include "kfar.h"

/* a raw byte of the file whose character (through the encoding and the
 * translation table) is the list's */
struct revnode {
	u8 raw;
	struct revnode *next;
};

/* search.inc: search_string_pre: in: ebx -> the pattern (a non-empty
 * zero-terminated string), esi -> the translation table, the encoding,
 * whole words only; out: the automaton (edx, to be pgfree'd; NULL when
 * there is no memory), *ecx its final state. The automaton has a row of
 * 256 next states per state; the prefix function follows it. For whole
 * words the pattern is framed by two "spaces" (isspace_table). The
 * number of states is limited to 256, so the pattern length must be at
 * most 255. */
u8 *search_string_pre(const char *ebx, const u8 *esi, int encoding, int whole, u32 *ecx_out)
{
	const u8 *pattern = (const u8 *)ebx;
	/* get pattern length, m */
	u32 ecx = strlen(ebx);
	if (whole) ecx += 2;
	/* allocate m*257 bytes for FSM and prefix function */
	u8 *edx = xpgalloc(ecx * 257);
	if (!edx) return 0;
	u8 *eax = edx + (ecx << 8);            /* the prefix function */
#define TR(c) esi[(u8)(c)]
	/* calculate prefix function */
	ecx = 0;
	eax[0] = 0;
	u32 edi = 0;
	if (!whole) {
		for (;;) {                      /* .prefixcalc */
			edi++;
			u8 dl = TR(pattern[edi]);
			if (!dl) break;         /* .prefixdone */
			for (;;) {
				if (dl == TR(pattern[ecx])) {
					ecx++;
					break;
				}
				if (!ecx) break;        /* .prefixint */
				ecx = eax[ecx - 1];
			}
			eax[edi] = ecx;
		}
	} else {
		for (;;) {                      /* .whole.prefixcalc */
			edi++;
			u8 dl = TR(pattern[edi - 1]);
			if (!dl) break;         /* .whole.prefixdone */
			for (;;) {              /* .whole.prefixloop */
				if (!ecx) {     /* .whole.testfirst */
					if (isspace_table[dl]) ecx++;
					break;
				}
				if (dl == TR(pattern[ecx - 1])) {
					ecx++;
					break;
				}
				ecx = eax[ecx - 1];
			}
			eax[edi] = ecx;         /* .whole.prefixint */
		}
		/* .whole.prefixdone: the final "space" */
		while (ecx && !isspace_table[TR(pattern[ecx - 1])])
			ecx = eax[ecx - 1];
		ecx++;
		eax[edi] = ecx;
	}
	/* .prefixdone: create reverse table for encoding+translation */
	if (encoding >= encodings_unicode)
		encoding = 0;   /* no translations for Unicode encodings, they must be handled separately by caller */
	struct revnode *T[256], nodes[256];
	memset(T, 0, sizeof(T));
	for (int cl = 255; cl >= 0; cl--) {     /* .createrev */
		u8 al = cl < 0x80 ? cl : encodings_tables[encoding][cl - 0x80];
		al = TR(al);
		nodes[cl].raw = cl;
		nodes[cl].next = T[al];
		T[al] = &nodes[cl];
	}
	for (int cl = 255; cl >= 0; cl--)       /* the lists by the pattern's characters */
		T[cl] = T[TR(cl)];
	/* create FSM */
	ecx = 0;
	u8 *row = edx;
	if (whole) {
		memcpy(row, isspace_table, 256);
		row += 256;
		ecx = 1;
	}
	do {
		/* .fsmcalc: the state's row starts as the row of its fallback,
		 * the state of the prefix function of the previous one (KMP).
		 * The original copies row pi[s]-1 (zeros when pi[s] = 0) and so
		 * misses matches after a partial one, e.g. "ab" in "xaab"; here
		 * row pi[s-1], and zeros (.fsmzero) for the first state */
		if (ecx)
			memcpy(row, edx + (eax[ecx - 1] << 8), 256);
		else
			memset(row, 0, 256);
		row += 256;
		/* .fsmnext */
		ecx++;
		for (struct revnode *n = T[*pattern]; n; n = n->next)
			row[-256 + n->raw] = ecx;
		pattern++;
	} while (*pattern);
	if (whole) {
		/* the final "space": its fallback as above (the original: row
		 * pi[s]-1) */
		memcpy(row, edx + (eax[ecx - 1] << 8), 256);
		row += 256;
		ecx++;
		for (int a = 0; a < 256; a++)   /* .whole.fsmfin */
			if (isspace_table[a]) row[-256 + a] = ecx;
	}
	/* .nowholefin: ok, now edx -> FSM, cl = final state */
#undef TR
	*ecx_out = ecx;
	return edx;
}
