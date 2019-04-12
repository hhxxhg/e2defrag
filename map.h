/* map.h: new block translation matrix tracking system for the
 * Linux file system defragmenter.
 *
 * Copyright (C) 2010 Phillip Susi <psusi@cfl.rr.com>
 *
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 */


#ifndef MAP_H

#include "rbtree.h"

void map_init(void);
Block map_forward_get (Block b);
Block map_reverse_get (Block b);
void map_forward_set (Block old, Block new);
void map_identity_add (Block start, Block count);
void dump_extents (void);

struct map_extent {
	Block old, new;
	int count;
	struct rb_node rb_forward_node;
	struct rb_node rb_reverse_node;
};

struct map_extent *map_reverse_first (void);
struct map_extent *map_reverse_next (struct map_extent *e);
struct map_extent *map_reverse_get_extent (Block b);
struct map_extent *map_reverse_get_extent_next(Block b);

#endif
