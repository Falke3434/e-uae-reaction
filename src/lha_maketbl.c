/* ------------------------------------------------------------------------ */
/* LHa for UNIX    															*/
/*				maketbl.c -- makes decoding table							*/
/*																			*/
/*		Modified          		Nobutaka Watazaki							*/
/*																			*/
/*	Ver. 1.14 	Source All chagned				1995.01.14	N.Watazaki		*/
/* ------------------------------------------------------------------------ */
#include "lha.h"

/* v17 (E-UAE): set by a bad (corrupt) table, decode() stops */
int lha_bad_table;

static void bad_table (unsigned short table[], short tablebits)
{
	int i;
	if (!lha_bad_table)
		write_log ("LHA: corrupt data (bad Huffman table)\n");
	lha_bad_table = 1;
	for (i = 0; i < (1 << tablebits); i++)
		table[i] = 0;
}

void lha_make_table(short nchar, unsigned char bitlen[], short tablebits, unsigned short table[])
{
	unsigned short  count[17];	/* count of bitlen */
	unsigned short  weight[17];	/* 0x10000ul >> bitlen */
	unsigned short  start[17];	/* first code of bitlen */
	unsigned short  total;
	unsigned int    total32;
	unsigned int    i, l;
	int             j, k, m, n, avail;
	unsigned short *p;

	avail = nchar;

	/* initialize */
	for (i = 1; i <= 16; i++) {
		count[i] = 0;
		weight[i] = 1 << (16 - i);
	}

	/* count; v17: a code length > 16 (corrupt data) overflowed count[] */
	for (i = 0; i < nchar; i++) {
		if (bitlen[i] > 16) {
			bad_table (table, tablebits);
			return;
		}
		count[bitlen[i]]++;
	}

	/* calculate first code; v17: the sum in 32 bits - it must be a
	 * complete code (0x10000) or empty, a sum that wrapped to a multiple
	 * of 0x10000 filled the table past its end */
	total = 0;
	total32 = 0;
	for (i = 1; i <= 16; i++) {
		start[i] = total;
		total += weight[i] * count[i];
		total32 += (unsigned int) weight[i] * count[i];
	}
	if (total32 != 0x10000 && total32 != 0) {
		/* v17: the table would be filled past its end */
		bad_table (table, tablebits);
		return;
	}

	/* shift data for make table. */
	m = 16 - tablebits;
	for (i = 1; i <= tablebits; i++) {
		start[i] >>= m;
		weight[i] >>= m;
	}

	/* initialize */
	j = start[tablebits + 1] >> m;
	k = 1 << tablebits;
	if (j != 0)
		for (i = j; i < k; i++)
			table[i] = 0;

	/* create table and tree */
	for (j = 0; j < nchar; j++) {
		k = bitlen[j];
		if (k == 0)
			continue;
		l = start[k] + weight[k];
		if (k <= tablebits) {
			/* code in table */
			for (i = start[k]; i < l; i++)
				table[i] = j;
		}
		else {
			/* code not in table */
			p = &table[(i = start[k]) >> m];
			i <<= tablebits;
			n = k - tablebits;
			/* make tree (n length) */
			while (--n >= 0) {
				if (*p == 0) {
					if (avail >= 2 * NC - 1) {      /* v17 */
						bad_table (table, tablebits);
						return;
					}
					h_right[avail] = h_left[avail] = 0;
					*p = avail++;
				}
				if (i & 0x8000)
					p = &h_right[*p];
				else
					p = &h_left[*p];
				i <<= 1;
			}
			*p = j;
		}
		start[k] = l;
	}
}

/* Local Variables: */
/* mode:c */
/* tab-width:4 */
/* End: */
