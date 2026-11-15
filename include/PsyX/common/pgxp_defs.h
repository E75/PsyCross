#ifndef PGXP_DEFS_H
#define PGXP_DEFS_H

#include "PsyX/PsyX_config.h"

#if USE_PGXP
#include "PsyX/common/half_float.h"

// Helpful macro

// vertices of one frame: a 10x draw distance with many cars transforms more than 65536, and with a 16 bit index the
// cache wrapped and the last drawn models (the cars) lost their precise positions; the index is 24 bit in the prim tag
#define PGXP_CACHE_SIZE		(1 << 20)	// a frame across a river sees the whole city: more than 262144 vertices wrapped the cache and the world drawn first lost its positions
#define PGXP_INVALID_INDEX	0xFFFFFF
#define PGXP_LOOKUP_VALUE(x, y) (*(u_short*)&(x) | (*(u_short*)&(y) << 16))

//-------------------------------------

// in C++, VERTTYPE can be declared as half
// use `_HF` to convert to half whenever you in C mode.
#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
typedef half VERTTYPE;
#define _HF(x) x
#else
typedef short VERTTYPE;
#define _HF(x) to_half_float(x)
#endif

typedef struct
{
	float px, py, pz;	// 32 bit values
	VERTTYPE x, y, z;	// 16 bit values (for lookup and backwards compat if not found in cache)
} PGXPVector3D;

typedef struct
{
	uint lookup;
	float px, py, pz;
	float scr_h, ofx, ofy;
} PGXPVData;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern PGXPVector3D g_FP_SXYZ0; // direct access PGXP without table lookup
extern PGXPVector3D g_FP_SXYZ1;
extern PGXPVector3D g_FP_SXYZ2;

/* clears PGXP vertex buffer */
void	PGXP_ClearCache();

/* emits new PGXP vertex */
uint	PGXP_EmitCacheData(PGXPVData* newData);

/* sets Z offset (works like Z bias) */
void	PGXP_SetZOffsetScale(float offset, float scale);

/* searches for vertex with given lookup value */
int		PGXP_GetCacheData(PGXPVData* out, uint lookup, uint indexhint /* = PGXP_INVALID_INDEX */);

/* used by primitive setup */
uint	PGXP_GetIndex(int checkTransform);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

// special PGXP type
typedef struct {		/* 2D short vector */
	half vx, vy;
	ushort pgxp_index;
} DVECTORF;

#else
typedef short VERTTYPE;

#define _HF(x) x

#endif


#endif // PGXP_DEFS