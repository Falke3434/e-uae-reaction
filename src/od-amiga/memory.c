 /*
  * UAE - The Un*x Amiga Emulator
  *
  * OS-specific memory support functions
  *
  * Copyright 2004 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include "include/memory.h"

#ifdef JIT
#ifdef __amigaos4__
//We need the memory handling functions and flags for AmigaOS4
#include <proto/exec.h>
#include <exec/exec.h>
#include <exec/exectags.h>
#endif

#ifdef __MORPHOS__
//We need the memory handling functions for MorphOS
#include <proto/exec.h>
#include <exec/system.h>
#endif

/*
 * Allocate executable memory for JIT cache
 */
void * cache_alloc (int size)
{
#ifdef __amigaos4__
	//For AmigaOS4 we must allocate the memory with executable flag
	//also we must align to the size of the cache line
	//(v18: AllocVecTags() instead of the obsolete AllocVec() with
	//MEMF_HWALIGNED; 64 byte alignment covers the cache lines of the PA6T
	//and the e5500, the G4 has 32 byte lines)
	return AllocVecTags(size,
			AVT_Type,      MEMF_EXECUTABLE,
			AVT_Alignment, 64,
			TAG_DONE);
#else
#ifdef __MORPHOS__
	//For MorphOS we will try to figure out the cache line size for the aligned memory allocation
	ULONG cachelinesize;
	if (!NewGetSystemAttrsA(&cachelinesize, sizeof(cachelinesize), SYSTEMINFOTYPE_PPC_ICACHEL1LINESIZE, NULL))
	{
		//Failed: let's send a warning and use 32 byte alignment
		write_log("Warning: failed to read cache alignment requirement, 32 bytes alignment is used");
		cachelinesize = 32;
	}

	//Allocate memory
	return AllocVecAligned(size, MEMF_ANY, cachelinesize, 0);
#else
	//Unknown system: use generic memory allocation
   return malloc (size);
#endif
#endif
}

/*
 * v18: may the JIT emit the mcrxr instruction? Only on the 60x/G3/G4
 * family (Pegasos, AmigaOne XE/SE/Micro), where it is part of the core. The
 * newer architecture versions dropped mcrxr (PA6T of the X1000, e5500 of the
 * X5000); on all cores except the 60x/G3/G4 the JIT does not rely on it and
 * copies the XER bits with mfxer/mtcrf, which works on every PowerPC.
 */
int ppc_cpu_native_mcrxr (void)
{
#ifdef __amigaos4__
	uint32 family = CPUFAMILY_UNKNOWN;
	int native;

	GetCPUInfoTags(GCIT_Family, &family, TAG_DONE);
	native = (family == CPUFAMILY_60X || family == CPUFAMILY_7X0 || family == CPUFAMILY_74XX);
	write_log("JIT: CPU family %lu, mcrxr %s\n", (unsigned long) family,
			native ? "native" : "replaced by mfxer/mtcrf");
	return native;
#else
	return 0;
#endif
}

/*
 * v18 (TODO 7.3): has the PowerPC a classic FPU? Not the e500 core of the
 * A1222 (P1022, family 8 = CPUFAMILY_E500 in exec/exectags.h): it has the
 * SPE unit instead, every FPU instruction would be emulated by the kernel.
 * The JIT translates the 68k FPU instructions only with a classic FPU,
 * otherwise they stay in the interpreter.
 */
int ppc_cpu_has_fpu (void)
{
#ifdef __amigaos4__
	uint32 family = CPUFAMILY_UNKNOWN;

	GetCPUInfoTags(GCIT_Family, &family, TAG_DONE);
	if (family == 8)
	{
		write_log("JIT: CPU family %lu has no classic FPU, FPU instructions are not translated\n", (unsigned long) family);
		return 0;
	}
#endif
	return 1;
}

/*
 * PowerPC instruction cache flush function
 */
void ppc_cacheflush(void* start, int length)
{
#ifdef __amigaos4__
	//For AmigaOS4
	CacheClearE(start, length, CACRF_ClearI|CACRF_ClearD);
#else
#ifdef __MORPHOS__
	//For MorphOS
	CacheFlushDataInstArea(start, length);
#else
	write_log("Error: processor cache flush function is not implemented for your system");
#endif
#endif
}

/**
 * Free JIT cache
 */
void cache_free (void *cache)
{
#if defined __amigaos4__ || defined __MORPHOS__
	//For AmigaOS4 and MorphOS
	FreeVec(cache);
#else
	//For unknown system
	free (cache);
#endif
}

#endif
