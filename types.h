#ifndef _DEFRAG_TYPES_H
#define _DEFRAG_TYPES_H

/*
 * types.h
 *
 * Common type definitions for the defrag and dump programs.  We define them
 * here to avoid having to include all of defrag.h in e2dump.c
 *
 * Copyright (c) Stephen Tweedie, 1997
 *
 */

#include <sys/types.h>
#include <linux/types.h>

/* Define modes for the walk_zone functions */
enum walk_zone_mode {WZ_SCAN, WZ_REMAP, WZ_FIXED_BLOCKS};

/* Block/buffer management prototypes */
enum BufferType { OUTPUT, RESCUE, FORCE };
typedef enum BufferType BufferType;
	
#endif /* !_DEFRAG_TYPES_H */
