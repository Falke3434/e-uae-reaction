/*
 * E-UAE - LZX archives (v17)
 *
 * Decrunch engine from WinUAE archivers/lzx/unlzx.cpp ("minimal UAE
 * specific version" by Toni Wilen of unlzx.c 1.0 by Oliver Gantert / Erik
 * Meusel), original credits below. The archive directory and extraction
 * glue is new: E-UAE has no zvolume layer, archive_lzx_get() returns the
 * index-th file of the archive as a memory zfile.
 */
/* $VER: unlzx.c 1.0 (22.2.98) */
/* Created: 11.2.98 */
/* Added Pipe support to read from stdin (03.4.01, Erik Meusel)           */

/* LZX Extract in (supposedly) portable C.                                */

/* ********************* WINDOWS PORTING NOTES STARTS *********************/

/* Ported to Windows Platform by Stefano Coletta on 22.5.02               */
/* creator@mindcreations.com or visit http://creator.mindcreations.com    */

/* Changes to original code include:                                      */
/* - Using #include <direct.h> to substitute the mkdir() function call    */
/* - Added #include "getopt.h" to support getopt() function               */

/* Compile with:                                                          */
/* Microsoft Visual Studio 6.0 SP5 using supplied project file            */

/* ********************** WINDOWS PORTING NOTES END ***********************/


/* Thanks to Dan Fraser for decoding the coredumps and helping me track   */
/* down some HIDEOUSLY ANNOYING bugs.                                     */

/* Everything is accessed as unsigned char's to try and avoid problems    */
/* with byte order and alignment. Most of the decrunch functions          */
/* encourage overruns in the buffers to make things as fast as possible.  */
/* All the time is taken up in crc_calc() and decrunch() so they are      */
/* pretty damn optimized. Don't try to understand this program.           */


#include "sysconfig.h"
#include "sysdeps.h"

#include <string.h>

#include "zfile.h"

/* the decrunch code reads and writes past its buffers ("encourage
 * overruns"), and corrupt data can point before the output: padding */
#define LZX_PAD 65536

/* ---------------------------------------------------------------------- */


struct lzxdata
{
  unsigned char *source;
  unsigned char *destination;
  unsigned char *source_end;
  unsigned char *destination_end;

  unsigned short decrunch_method;
  unsigned int decrunch_length;
  unsigned int last_offset;
  unsigned int global_control;
  short global_shift;
  unsigned int sum;

  unsigned char offset_len[8];
  unsigned short offset_table[128];
  unsigned char huffman20_len[20];
  unsigned short huffman20_table[96];
  unsigned char literal_len[768];
  unsigned short literal_table[5120];
};

/* ---------------------------------------------------------------------- */

static const unsigned int crc_table[256]=
{
 0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,
 0xE963A535,0x9E6495A3,0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,
 0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,0x1DB71064,0x6AB020F2,
 0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
 0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,
 0xFA0F3D63,0x8D080DF5,0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,
 0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,0x35B5A8FA,0x42B2986C,
 0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
 0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,
 0xCFBA9599,0xB8BDA50F,0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,
 0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,0x76DC4190,0x01DB7106,
 0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
 0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,
 0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,
 0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,0x65B0D9C6,0x12B7E950,
 0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
 0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,
 0xA4D1C46D,0xD3D6F4FB,0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,
 0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,0x5005713C,0x270241AA,
 0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
 0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,
 0xB7BD5C3B,0xC0BA6CAD,0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,
 0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,0xE3630B12,0x94643B84,
 0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
 0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,
 0x196C3671,0x6E6B06E7,0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,
 0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,0xD6D6A3E8,0xA1D1937E,
 0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
 0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,
 0x316E8EEF,0x4669BE79,0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,
 0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,0xC5BA3BBE,0xB2BD0B28,
 0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
 0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,
 0x72076785,0x05005713,0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,
 0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,0x86D3D2D4,0xF1D4E242,
 0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
 0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,
 0x616BFFD3,0x166CCF45,0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,
 0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,0xAED16A4A,0xD9D65ADC,
 0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
 0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,
 0x54DE5729,0x23D967BF,0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,
 0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
};

/* ---------------------------------------------------------------------- */

static const unsigned char table_one[32]=
{
 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14
};

static const unsigned int table_two[32]=
{
 0,1,2,3,4,6,8,12,16,24,32,48,64,96,128,192,256,384,512,768,1024,
 1536,2048,3072,4096,6144,8192,12288,16384,24576,32768,49152
};

static const unsigned int table_three[16]=
{
 0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767
};

static const unsigned char table_four[34]=
{
 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16
};

/* ---------------------------------------------------------------------- */

/* Possible problems with 64 bit machines here. It kept giving warnings   */
/* for people so I changed back to ~.                                     */

static void crc_calc(struct lzxdata *d, unsigned char *memory, unsigned int length)
{
 unsigned int temp;

 if(length)
 {
  temp = ~d->sum; /* was (sum ^ 4294967295) */
  do
  {
   temp = crc_table[(*memory++ ^ temp) & 255] ^ (temp >> 8);
  } while(--length);
  d->sum = ~temp; /* was (temp ^ 4294967295) */
 }
}

/* ---------------------------------------------------------------------- */

/* Build a fast huffman decode table from the symbol bit lengths.         */
/* There is an alternate algorithm which is faster but also more complex. */

static int make_decode_table(struct lzxdata *d, short number_symbols, short table_size,
		      unsigned char *length, unsigned short *table)
{
 unsigned char bit_num = 0;
 short symbol;
 unsigned short leaf; /* could be a register */
 unsigned short bit_mask, fill, next_symbol, reverse;
 unsigned int table_mask, pos;
#if LZX_ERROR_CHECK
 short abort = 0;
#endif

 pos = 0; /* consistantly used as the current position in the decode table */

 bit_mask = table_mask = 1 << table_size;

 bit_mask >>= 1; /* don't do the first number */
 bit_num++;

 while(bit_num <= table_size)
 {
  for(symbol = 0; symbol < number_symbols; symbol++)
  {
   if(length[symbol] == bit_num)
   {
    reverse = pos; /* reverse the order of the position's bits */
    leaf = 0;
    fill = table_size;
    do /* reverse the position */
    {
     leaf = (leaf << 1) + (reverse & 1);
     reverse >>= 1;
    } while(--fill);
	pos += bit_mask;
#if LZX_ERROR_CHECK
	if(pos > table_mask)
    {
	 /* we will overrun the table! abort! */
	 return 1;
    }
#endif
	fill = bit_mask;
    next_symbol = 1 << bit_num;
    do
    {
     table[leaf] = symbol;
     leaf += next_symbol;
    } while(--fill);
   }
  }
  bit_mask >>= 1;
  bit_num++;
 }

 if(pos != table_mask)
 {
  for(symbol = pos; symbol < table_mask; symbol++) /* clear the rest of the table */
  {
   reverse = symbol; /* reverse the order of the position's bits */
   leaf = 0;
   fill = table_size;
   do /* reverse the position */
   {
    leaf = (leaf << 1) + (reverse & 1);
    reverse >>= 1;
   } while(--fill);
   table[leaf] = 0;
  }
  next_symbol = table_mask >> 1;
  pos <<= 16;
  table_mask <<= 16;
  bit_mask = 32768;

  while(bit_num <= 16)
  {
   for(symbol = 0; symbol < number_symbols; symbol++)
   {
    if(length[symbol] == bit_num)
    {
     reverse = pos >> 16; /* reverse the order of the position's bits */
     leaf = 0;
     fill = table_size;
     do /* reverse the position */
     {
      leaf = (leaf << 1) + (reverse & 1);
      reverse >>= 1;
     } while(--fill);
     for(fill = 0; fill < bit_num - table_size; fill++)
     {
      if(table[leaf] == 0)
      {
       table[(next_symbol << 1)] = 0;
       table[(next_symbol << 1) + 1] = 0;
       table[leaf] = next_symbol++;
      }
      leaf = table[leaf] << 1;
      leaf += (pos >> (15 - fill)) & 1;
     }
     table[leaf] = symbol;
	 pos += bit_mask;
#if LZX_ERROR_CHECK
	 if(pos > table_mask)
     {
      /* we will overrun the table! abort! */
	  return 1;
     }
#endif
    }
   }
   bit_mask >>= 1;
   bit_num++;
  }
 }
#if LZX_ERROR_CHECK
 if(pos != table_mask) return 1; /* the table is incomplete! */
#endif
 return 0;
}

/* ---------------------------------------------------------------------- */

/* Read and build the decrunch tables. There better be enough data in the */
/* source buffer or it's stuffed. */

static int read_literal_table(struct lzxdata *d)
{
 unsigned int control;
 short shift;
 unsigned short temp; /* could be a register */
 unsigned short symbol, pos, count, fix, max_symbol;
#if LZX_ERROR_CHECK
 short abort = 0;
#endif

 control = d->global_control;
 shift = d->global_shift;

 if(shift < 0) /* fix the control word if necessary */
 {
  shift += 16;
  control += (unsigned int) *d->source++ << (8 + shift);
  control += (unsigned int) *d->source++ << shift;
 }

/* read the decrunch method */

 d->decrunch_method = control & 7;
 control >>= 3;
 if((shift -= 3) < 0)
 {
  shift += 16;
  control += (unsigned int) *d->source++ << (8 + shift);
  control += (unsigned int) *d->source++ << shift;
 }

/* Read and build the offset huffman table */

 if(d->decrunch_method == 3)
 {
  for(temp = 0; temp < 8; temp++)
  {
   d->offset_len[temp] = control & 7;
   control >>= 3;
   if((shift -= 3) < 0)
   {
    shift += 16;
    control += (unsigned int) *d->source++ << (8 + shift);
    control += (unsigned int) *d->source++ << shift;
   }
  }
#if LZX_ERROR_CHECK
  abort = make_decode_table(d, 8, 7, d->offset_len, d->offset_table);
  if (abort)
	  return abort;
#else
  make_decode_table(d, 8, 7, d->offset_len, d->offset_table);
#endif
 }

/* read decrunch length */

  d->decrunch_length = (control & 255) << 16;
  control >>= 8;
  if((shift -= 8) < 0)
  {
   shift += 16;
   control += (unsigned int) *d->source++ << (8 + shift);
   control += (unsigned int) *d->source++ << shift;
  }
  d->decrunch_length += (control & 255) << 8;
  control >>= 8;
  if((shift -= 8) < 0)
  {
   shift += 16;
   control += (unsigned int) *d->source++ << (8 + shift);
   control += (unsigned int) *d->source++ << shift;
  }
  d->decrunch_length += (control & 255);
  control >>= 8;
  if((shift -= 8) < 0)
  {
   shift += 16;
   control += (unsigned int) *d->source++ << (8 + shift);
   control += (unsigned int) *d->source++ << shift;
  }

/* read and build the huffman literal table */

 if(d->decrunch_method != 1)
 {
  pos = 0;
  fix = 1;
  max_symbol = 256;

  do
  {
   for(temp = 0; temp < 20; temp++)
   {
    d->huffman20_len[temp] = control & 15;
    control >>= 4;
    if((shift -= 4) < 0)
    {
     shift += 16;
     control += (unsigned int) *d->source++ << (8 + shift);
     control += (unsigned int) *d->source++ << shift;
    }
   }
#if LZX_ERROR_CHECK
   abort = make_decode_table(d, 20, 6, d->huffman20_len, d->huffman20_table);
   if (abort)
	   return abort;
#else
   make_decode_table(d, 20, 6, d->huffman20_len, d->huffman20_table);
#endif

   do
   {
    if((symbol = d->huffman20_table[control & 63]) >= 20)
    {
     do /* symbol is longer than 6 bits */
     {
      symbol = d->huffman20_table[((control >> 6) & 1) + (symbol << 1)];
      if(!shift--)
      {
       shift += 16;
       control += (unsigned int) *d->source++ << 24;
       control += (unsigned int) *d->source++ << 16;
      }
      control >>= 1;
     } while(symbol >= 20);
     temp = 6;
    }
    else
    {
     temp = d->huffman20_len[symbol];
    }
    control >>= temp;
    if((shift -= temp) < 0)
    {
     shift += 16;
     control += (unsigned int) *d->source++ << (8 + shift);
     control += (unsigned int) *d->source++ << shift;
    }
    switch(symbol)
    {
     case 17:
     case 18:
     {
      if(symbol == 17)
      {
       temp = 4;
       count = 3;
      }
      else /* symbol == 18 */
      {
       temp = 6 - fix;
       count = 19;
      }
      count += (control & table_three[temp]) + fix;
      control >>= temp;
      if((shift -= temp) < 0)
      {
       shift += 16;
       control += (unsigned int) *d->source++ << (8 + shift);
       control += (unsigned int) *d->source++ << shift;
      }
      while((pos < max_symbol) && (count--))
       d->literal_len[pos++] = 0;
      break;
     }
     case 19:
     {
      count = (control & 1) + 3 + fix;
      if(!shift--)
      {
       shift += 16;
       control += (unsigned int) *d->source++ << 24;
       control += (unsigned int) *d->source++ << 16;
      }
      control >>= 1;
      if((symbol = d->huffman20_table[control & 63]) >= 20)
      {
       do /* symbol is longer than 6 bits */
       {
	symbol = d->huffman20_table[((control >> 6) & 1) + (symbol << 1)];
	if(!shift--)
	{
	 shift += 16;
	 control += (unsigned int) *d->source++ << 24;
	 control += (unsigned int) *d->source++ << 16;
	}
	control >>= 1;
       } while(symbol >= 20);
       temp = 6;
      }
      else
      {
       temp = d->huffman20_len[symbol];
      }
      control >>= temp;
      if((shift -= temp) < 0)
      {
       shift += 16;
       control += (unsigned int) *d->source++ << (8 + shift);
       control += (unsigned int) *d->source++ << shift;
      }
      if (symbol > 16)        /* v17: corrupt data, table_four[] underflow */
       return 1;
      symbol = table_four[d->literal_len[pos] + 17 - symbol];
      while((pos < max_symbol) && (count--))
       d->literal_len[pos++] = (unsigned char)symbol;
      break;
     }
     default:
     {
      symbol = table_four[d->literal_len[pos] + 17 - symbol];
      d->literal_len[pos++] = (unsigned char)symbol;
      break;
     }
    }
   } while(pos < max_symbol);
   fix--;
   max_symbol += 512;
  } while(max_symbol == 768);

#if LZX_ERROR_CHECK
   abort = make_decode_table(d, 768, 12, d->literal_len, d->literal_table);
   if (abort)
	   return abort;
#else
   make_decode_table(d, 768, 12, d->literal_len, d->literal_table);
#endif
 }

 d->global_control = control;
 d->global_shift = shift;

 return 0;
}

/* ---------------------------------------------------------------------- */

/* Fill up the decrunch buffer. Needs lots of overrun for both destination */
/* and source buffers. Most of the time is spent in this routine so it's  */
/* pretty damn optimized. */

static void decrunch(struct lzxdata *d)
{
 unsigned int control;
 short shift;
 unsigned short temp; /* could be a register */
 unsigned short symbol, count;
 unsigned char *string;

 control = d->global_control;
 shift = d->global_shift;

 do
 {
  if((symbol = d->literal_table[control & 4095]) >= 768)
  {
   control >>= 12;
   if((shift -= 12) < 0)
   {
    shift += 16;
    control |= (unsigned int) *d->source++ << (8 + shift);
    control |= (unsigned int) *d->source++ << shift;
   }
   do /* literal is longer than 12 bits */
   {
    symbol = d->literal_table[(control & 1) + (symbol << 1)];
    if(!shift--)
    {
     shift += 16;
     control |= (unsigned int) *d->source++ << 24;
     control |= (unsigned int) *d->source++ << 16;
    }
    control >>= 1;
   } while(symbol >= 768);
  }
  else
  {
   temp = d->literal_len[symbol];
   control >>= temp;
   if((shift -= temp) < 0)
   {
    shift += 16;
    control |= (unsigned int) *d->source++ << (8 + shift);
    control |= (unsigned int) *d->source++ << shift;
   }
  }
  if(symbol < 256)
  {
   *d->destination++ = (unsigned char)symbol;
  }
  else
  {
   symbol -= 256;
   count = table_two[temp = symbol & 31];
   temp = table_one[temp];
   if((temp >= 3) && (d->decrunch_method == 3))
   {
    temp -= 3;
    count += ((control & table_three[temp]) << 3);
    control >>= temp;
    if((shift -= temp) < 0)
    {
     shift += 16;
     control |= (unsigned int) *d->source++ << (8 + shift);
     control |= (unsigned int) *d->source++ << shift;
    }
    count += (temp = d->offset_table[control & 127]);
    temp = d->offset_len[temp];
   }
   else
   {
    count += control & table_three[temp];
    if(!count) count = d->last_offset;
   }
   control >>= temp;
   if((shift -= temp) < 0)
   {
    shift += 16;
    control |= (unsigned int) *d->source++ << (8 + shift);
    control |= (unsigned int) *d->source++ << shift;
   }
   d->last_offset = count;

   count = table_two[temp = (symbol >> 5) & 15] + 3;
   temp = table_one[temp];
   count += (control & table_three[temp]);
   control >>= temp;
   if((shift -= temp) < 0)
   {
    shift += 16;
    control |= (unsigned int) *d->source++ << (8 + shift);
    control |= (unsigned int) *d->source++ << shift;
   }

   string = d->destination - d->last_offset;
   do
   {
    *d->destination++ = *string++;
   } while(--count);
  }
 } while((d->destination < d->destination_end) && (d->source < d->source_end));

 d->global_control = control;
 d->global_shift = shift;
}


/* ---------------------------------------------------------------------- */
/* v17: directory + extraction (E-UAE)                                    */

#define LZX_MAX_ENTRIES 1024

struct lzx_entry {
    char name[256];
    unsigned int size;        /* unpacked size                              */
    unsigned int offset;      /* != 0: last file of a merged block, file
				 position of the packed data               */
    unsigned int packedsize;  /* packed size of that block                  */
    unsigned int offset2;     /* position in the unpacked merged block      */
};

static int lzx_directory (struct zfile *in_file, struct lzx_entry *e, int max)
{
    unsigned char archive_header[31];
    char header_filename[256];
    char header_comment[256];
    unsigned int merge_size = 0;
    int n = 0;
    struct lzxdata d;

    memset (&d, 0, sizeof d);
    zfile_fseek (in_file, 0, SEEK_SET);
    if (zfile_fread (archive_header, 1, 10, in_file) != 10)
	return -1;
    if (memcmp (archive_header, "LZX", 3))
	return -1;
    for (;;) {
	unsigned int crc, temp, pack_size, unpack_size;

	if (zfile_fread (archive_header, 1, 31, in_file) != 31)
	    break;                                   /* end of the archive */
	d.sum = 0;
	crc = (archive_header[29] << 24) + (archive_header[28] << 16) + (archive_header[27] << 8) + archive_header[26];
	archive_header[29] = archive_header[28] = archive_header[27] = archive_header[26] = 0;
	crc_calc (&d, archive_header, 31);
	temp = archive_header[30];                   /* filename length */
	if (zfile_fread (header_filename, 1, temp, in_file) != temp)
	    break;
	header_filename[temp] = 0;
	crc_calc (&d, (unsigned char *) header_filename, temp);
	temp = archive_header[14];                   /* comment length */
	if (zfile_fread (header_comment, 1, temp, in_file) != temp)
	    break;
	header_comment[temp] = 0;
	crc_calc (&d, (unsigned char *) header_comment, temp);
	if (d.sum != crc) {
	    write_log ("LZX: header CRC error\n");
	    break;
	}
	unpack_size = (archive_header[5] << 24) + (archive_header[4] << 16) + (archive_header[3] << 8) + archive_header[2];
	pack_size = (archive_header[9] << 24) + (archive_header[8] << 16) + (archive_header[7] << 8) + archive_header[6];
	if (n < max) {
	    strncpy (e[n].name, header_filename, sizeof e[n].name - 1);
	    e[n].name[sizeof e[n].name - 1] = 0;
	    e[n].size = unpack_size;
	    e[n].offset = 0;
	    e[n].packedsize = 0;
	    e[n].offset2 = merge_size;
	}
	merge_size += unpack_size;
	if (pack_size) {                             /* end of a merged block */
	    merge_size = 0;
	    if (n < max) {
		e[n].offset = (unsigned int) zfile_ftell (in_file);
		e[n].packedsize = pack_size;
	    }
	    zfile_fseek (in_file, pack_size, SEEK_CUR);
	}
	n++;
    }
    return n < max ? n : max;
}

/* The index-th file (0 = first) of an LZX archive as a memory zfile, NULL
 * if there is no such file or it can't be unpacked. */
struct zfile *archive_lzx_get (struct zfile *zf, int index)
{
    struct lzx_entry *e;
    int n, first, last, i;
    unsigned int startpos, compsize, unpsize = 0;
    unsigned char *buf = NULL, *dbase = NULL, *dbuf = NULL, *dbufend;
    struct zfile *out = NULL;
    struct lzxdata *d;

    e = malloc (LZX_MAX_ENTRIES * sizeof *e);
    d = calloc (1, sizeof *d);
    if (!e || !d)
	goto end;
    n = lzx_directory (zf, e, LZX_MAX_ENTRIES);
    if (index < 0 || index >= n)
	goto end;
    /* the merged block that holds the file: from the first file after
     * the previous block end to the file that ends the block */
    for (first = index; first > 0 && e[first - 1].offset == 0; first--)
	;
    for (last = index; last < n && e[last].offset == 0; last++)
	;
    if (last >= n)
	goto end;
    for (i = first; i <= last; i++)
	unpsize += e[i].size;
    startpos = e[last].offset;
    compsize = e[last].packedsize;
    if (e[index].offset2 + e[index].size > unpsize)
	goto end;

    buf = calloc (1, compsize + LZX_PAD);
    dbase = calloc (1, LZX_PAD + unpsize + LZX_PAD);
    if (!buf || !dbase)
	goto end;
    dbuf = dbase + LZX_PAD;
    zfile_fseek (zf, startpos, SEEK_SET);
    if (zfile_fread (buf, 1, compsize, zf) != compsize)
	goto end;

    d->source = buf;
    d->source_end = buf + compsize + 1;
    d->global_control = 0;
    d->global_shift = -16;
    d->last_offset = 1;
    d->destination = dbuf;
    dbufend = dbuf + unpsize;
    if (compsize == unpsize) {
	memcpy (dbuf, buf, unpsize);
    } else {
	unsigned int left = unpsize;
	while (left > 0) {
	    unsigned char *pdest = d->destination;
	    if (read_literal_table (d)) {
		write_log ("LZX: corrupt compressed data in '%s'\n", e[index].name);
		goto end;
	    }
	    d->destination_end = d->destination + d->decrunch_length;
	    if (d->destination_end > dbufend || d->decrunch_length == 0) {
		write_log ("LZX: corrupt compressed data in '%s'\n", e[index].name);
		goto end;
	    }
	    decrunch (d);
	    left -= d->decrunch_length;
	    crc_calc (d, pdest, d->decrunch_length);
	}
    }
    out = zfile_fopen_empty (e[index].name, e[index].size);
    if (out && e[index].size)
	zfile_fwrite (dbuf + e[index].offset2, e[index].size, 1, out);
    if (out)
	zfile_fseek (out, 0, SEEK_SET);
end:
    free (buf);
    free (dbase);
    free (d);
    free (e);
    return out;
}
