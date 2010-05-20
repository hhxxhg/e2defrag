/* map.c: new block translation matrix tracking system for the
 * Linux file system defragmenter.
 *
 * Copyright (C) 2010 Phillip Susi <psusi@cfl.rr.com>
 *
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 */
#include <glib.h>
#include <malloc.h>
#include <assert.h>
#include <stdio.h>
#include "map.h"

struct map_extent {
	Block old, new;
	int count;
};

static GTree *forward_map, *reverse_map;

static int forward_compare (struct map_extent *a, struct map_extent *b)
{
	if (a->old > b->old)
		return 1;
	else if (a->old < b->old)
		return -1;
	else return 0;
}

static int reverse_compare (struct map_extent *a, struct map_extent *b)
{
	if (a->new > b->new)
		return 1;
	else if (a->new < b->new)
		return -1;
	else return 0;
}

static int forward_search (struct map_extent *e, Block *b)
{
	if (e->old <= *b && (e->old + e->count) >= *b)
		return 0;
	else if (e->old > *b)
		return -1;
	else if (e->old < *b)
		return 1;
}

static int forward_search_add (struct map_extent *e, Block *b)
{
 	if (e->old-1 <= *b && (e->old + e->count) >= *b)
		return 0;
	else if ((e->old-1) <= *b && (e->old + e->count + 1) >= *b)
		return 0;
	else if (e->old > *b)
		return -1;
	else if (e->old < *b)
		return 1;
}

static int reverse_search (struct map_extent *e, Block *b)
{
	if (e->new <= *b && (e->new + e->count) >= *b)
		return 0;
	else if (e->new > *b)
		return -1;
	else if (e->new < *b)
		return 1;
}

static int reverse_search_add (struct map_extent *e, Block *b)
{
	if (e->new <= *b && (e->new + e->count) >= *b)
		return 0;
	else if ((e->new-1) <= *b && (e->new + e->count + 1) >= *b)
		return 0;
	else if (e->new > *b)
		return -1;
	else if (e->new < *b)
		return 1;
}

void map_init()
{
	forward_map = g_tree_new_full ((GCompareDataFunc)forward_compare,
				       NULL,
				       NULL,
				       NULL);
	assert (forward_map);
	reverse_map = g_tree_new_full ((GCompareDataFunc)reverse_compare,
				       NULL,
				       NULL,
				       NULL);
	assert (reverse_map);
}

/* lookup destination block given original location */

Block map_forward_get (Block b)
{
	struct map_extent *e;

	e = g_tree_search (forward_map, (GCompareFunc)forward_search, &b);
	if (e)
		return b - e->old + e->new;
	else return 0;
}

/* lookup source block given destination */

Block map_reverse_get (Block b)
{
	struct map_extent *e;

	e = g_tree_search (reverse_map, (GCompareFunc)reverse_search, &b);
	if (e)
		return b - e->new + e->old;
	else return 0;
}

/* set new destination given old original location */

void map_forward_set (Block old, Block new)
{
	struct map_extent *e;

	e = g_tree_search (forward_map, (GCompareFunc)forward_search, &old);
	if (!e)
		e = g_tree_search (forward_map, (GCompareFunc)forward_search_add, &old);
	if (e) {
		if (e->old <= old && (e->old + e->count) >= old) {
			/* exact match, may have to split */
			if (old - e->old + e->new == new)
				return; /* already has new value, nothing to do */
			if (e->count == 0) {
				g_tree_remove (reverse_map, e);
				if (new == 0) {
					/* delete entry */
					g_tree_remove (forward_map, e);
					free (e);
					return;
				}
				e->new = new;
				g_tree_insert (reverse_map, e, e);
			}
			else if (e->old == old) {
				/* head split */
				g_tree_remove (forward_map, e);
				g_tree_remove (reverse_map, e);
				e->old++;
				e->new++;
				e->count--;
				g_tree_insert (forward_map, e, e);
				g_tree_insert (reverse_map, e, e);
				goto add;
			} else if (e->old+e->count == old) {
				/* tail split */
				e->count--;
			} else {
				/* center split */
				/* add new extent for blocks after */
				struct map_extent *ne = malloc (sizeof(struct map_extent));
				assert (ne);
				ne->count = e->count - old;
				/* truncate existing extent to blocks before */
				e->count = old - e->old - 1;
				ne->new = e->new + e->count + 2;
				ne->old = e->old + e->count + 2;
				g_tree_insert (forward_map, ne, ne);
				g_tree_insert (reverse_map, ne, ne);
				/* now add the new extent */
				goto add;
			}
		} else {
			/* got an extent just before or after we may
			   be able to merge with */
			if (e->old-1 == old && e->new-1 == new) {
				/* head merge */
				g_tree_remove (forward_map, e);
				g_tree_remove (reverse_map, e);
				e->old--;
				e->new--;
				e->count++;
				g_tree_insert (forward_map, e, e);
				g_tree_insert (reverse_map, e, e);
			} else if (e->old+e->count+1 == old && e->new+e->count+1 == new) {
				/* tail merge */
				e->count++;
			} else goto add; /* can't merge */
		}
	} else {
	add:
		if (new == 0) /* 0 = no entry, so do nothing */
			return;
		/* simple case, no existing extent so add one */
		e = malloc (sizeof(struct map_extent));
		assert (e);
		e->old = old;
		e->new = new;
		e->count = 0;
		g_tree_insert (forward_map, e, e);
		g_tree_insert (reverse_map, e, e);
	}
}

static gboolean dump_extent (struct map_extent *e, void *unused1, void *unused2)
{
	printf ("old = %d, new = %d, count = %d\n", e->old, e->new, e->count);
	return 0;
}

#if 0
int main()
{
	map_init();
	map_forward_set (5, 6);
	map_forward_set (3, 4);
	map_forward_set (1, 2);
	map_forward_set (2, 3);
	map_forward_set (4, 5);
	map_forward_set (12, 57);
	map_forward_set (2, 19);
	map_forward_set (3, 0);
	g_tree_foreach (forward_map, (GTraverseFunc)dump_extent, NULL);
	printf ("2 moves to %d\n", map_forward_get (2));
	printf ("3 moves to %d\n", map_forward_get (3));
	printf ("12 moves to %d\n", map_forward_get (12));
	printf ("19 comes from %d\n", map_reverse_get (19));
	printf ("57 comes from %d\n", map_reverse_get (57));
	return 0;
}
#endif
