/* map.h: new block translation matrix tracking system for the
 * Linux file system defragmenter.
 *
 * Copyright (C) 2010 Phillip Susi <psusi@cfl.rr.com>
 *
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 */


#ifndef MAP_H

void map_init(void);
Block map_forward_get (Block b);
Block map_reverse_get (Block b);
void map_forward_set (Block old, Block new);

#endif
