/*
 * E-UAE - LHA / LZH archives (v17)
 *
 * Decoder: LHa for UNIX (Y. Tagawa, N. Watazaki, t. okamoto, see the
 * lha_*.c files) as used by WinUAE (archivers/lha). The glue is new and
 * replaces WinUAE's uae_lha.cpp (no zvolume layer in E-UAE):
 * archive_lha_get() returns the index-th file of the archive as a memory
 * zfile.
 */

#define LHA_MAIN_SRC            /* the LHa globals are defined here */
#include "lha.h"

static const char *methods[] =
{
    LZHUFF0_METHOD, LZHUFF1_METHOD, LZHUFF2_METHOD, LZHUFF3_METHOD,
    LZHUFF4_METHOD, LZHUFF5_METHOD, LZHUFF6_METHOD, LZHUFF7_METHOD,
    LARC_METHOD, LARC5_METHOD, LARC4_METHOD,
    LZHDIRS_METHOD,
    NULL
};

/* used by the WinUAE header code */
int uaestrlen (const char *s)
{
    return (int) strlen (s);
}

static struct zfile *lha_unpack (struct zfile *zf, LzHeader *hdr, int method)
{
    struct zfile *out;
    struct interfacing lhinterface;

    if (hdr->original_size < 0 || hdr->original_size > 64 * 1024 * 1024)
	return NULL;
    out = zfile_fopen_empty (hdr->name, (int) hdr->original_size);
    if (!out)
	return NULL;

    lhinterface.method = method;
    lhinterface.dicbit = 13;        /* method + 8; -lh5- */
    lhinterface.infile = zf;
    lhinterface.outfile = out;
    lhinterface.original = (unsigned long) hdr->original_size;
    lhinterface.packed = hdr->packed_size;

    switch (method) {
	case LZHUFF0_METHOD_NUM:
	case LARC4_METHOD_NUM:
	    {
		uae_u8 *tmp = malloc (hdr->original_size + 1);
		if (tmp) {
		    zfile_fread (tmp, 1, hdr->original_size, zf);
		    zfile_fwrite (tmp, 1, hdr->original_size, out);
		    free (tmp);
		}
	    }
	    break;
	case LARC_METHOD_NUM:           /* -lzs- */
	    lhinterface.dicbit = 11;
	    decode (&lhinterface);
	    break;
	case LZHUFF1_METHOD_NUM:        /* -lh1- */
	case LZHUFF4_METHOD_NUM:        /* -lh4- */
	case LARC5_METHOD_NUM:          /* -lz5- */
	    lhinterface.dicbit = 12;
	    decode (&lhinterface);
	    break;
	case LZHUFF6_METHOD_NUM:        /* -lh6- */
	case LZHUFF7_METHOD_NUM:        /* -lh7- */
	    lhinterface.dicbit = (method - LZHUFF6_METHOD_NUM) + 15;
	    decode (&lhinterface);
	    break;
	default:
	    decode (&lhinterface);
	    break;
    }
    zfile_fseek (out, 0, SEEK_SET);
    return out;
}

/* The index-th file (0 = first, directories don't count) of an LHA
 * archive as a memory zfile, NULL if there is none. */
struct zfile *archive_lha_get (struct zfile *zf, int index)
{
    LzHeader hdr;
    int n = 0;

    zfile_fseek (zf, 0, SEEK_SET);
    for (;;) {
	long pos;
	int i, method = -1;

	memset (&hdr, 0, sizeof hdr);
	if (!get_header (zf, &hdr))
	    break;
	for (i = 0; methods[i]; i++)
	    if (!strcmp (methods[i], hdr.method))
		method = i;
	pos = zfile_ftell (zf);
	if (method < 0) {
	    write_log ("LHA: unknown method '%.5s' of '%s'\n", hdr.method, hdr.name);
	} else if (method != LZHDIRS_METHOD_NUM) {
	    if (n == index)
		return lha_unpack (zf, &hdr, method);
	    n++;
	}
	zfile_fseek (zf, pos + hdr.packed_size, SEEK_SET);
    }
    return NULL;
}
