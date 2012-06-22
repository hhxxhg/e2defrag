/* map.c: new block translation matrix tracking system for the
 * Linux file system defragmenter.
 *
 * Copyright (C) 2010 Phillip Susi <psusi@cfl.rr.com>
 *
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 */
#include <malloc.h>
#include <assert.h>
#include <stdio.h>
#include "defrag.h"
#include "map.h"

struct rb_root forward_tree_root = RB_ROOT;
struct rb_root reverse_tree_root = RB_ROOT;

static inline struct map_extent * rb_search_map_forward(Block b)
{
	struct map_extent *extent;
	struct rb_node *n = forward_tree_root.rb_node;

	while (n)
	{
		extent = rb_entry(n, struct map_extent, rb_forward_node);

		if (b < extent->old)
			n = n->rb_left;
		else if (b <= (extent->old + extent->count) )
			return extent;
		else n = n->rb_right;
	}
	return NULL;
}

static inline struct map_extent * __rb_insert_map_forward(Block b,
							  struct rb_node * node)
{
	struct rb_node ** p = &forward_tree_root.rb_node;
	struct rb_node * parent = NULL;
	struct map_extent *extent;

	while (*p)
	{
		parent = *p;
		extent = rb_entry(parent, struct map_extent, rb_forward_node);

		if (b < extent->old)
			p = &(*p)->rb_left;
		else if (b > extent->old)
			p = &(*p)->rb_right;
		else
			return extent;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

static inline struct map_extent * rb_insert_map_forward(struct map_extent *extent)
{
	struct map_extent * ret;
	if ((ret = __rb_insert_map_forward(extent->old, &extent->rb_forward_node)))
		goto out;
	rb_insert_color(&extent->rb_forward_node, &forward_tree_root);
 out:
	return ret;
}

static inline struct map_extent * rb_search_map_reverse(Block b)
{
	struct map_extent *extent;
	struct rb_node *n = reverse_tree_root.rb_node;

	while (n)
	{
		extent = rb_entry(n, struct map_extent, rb_reverse_node);

		if (b < extent->new)
			n = n->rb_left;
		else if (b <= (extent->new + extent->count) )
			return extent;
		else n = n->rb_right;
	}
	return NULL;
}

static inline struct map_extent * __rb_insert_map_reverse(Block b,
							  struct rb_node * node)
{
	struct rb_node ** p = &reverse_tree_root.rb_node;
	struct rb_node * parent = NULL;
	struct map_extent *extent;

	while (*p)
	{
		parent = *p;
		extent = rb_entry(parent, struct map_extent, rb_reverse_node);

		if (b < extent->new)
			p = &(*p)->rb_left;
		else if (b > extent->new)
			p = &(*p)->rb_right;
		else
			return extent;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

static inline struct map_extent * rb_insert_map_reverse(struct map_extent *extent)
{
	struct map_extent * ret;
	if ((ret = __rb_insert_map_reverse(extent->new, &extent->rb_reverse_node)))
		goto out;
	rb_insert_color(&extent->rb_reverse_node, &reverse_tree_root);
 out:
	return ret;
}

void map_init()
{
}

/* lookup destination block given original location */

Block map_forward_get (Block b)
{
	struct map_extent *e;

	e = rb_search_map_forward (b);
	if (e)
		return b - e->old + e->new;
	else return 0;
}

/* lookup source block given destination */

Block map_reverse_get (Block b)
{
	struct map_extent *e;

	e = rb_search_map_reverse (b);
	if (e)
		return b - e->new + e->old;
	else return 0;
}

struct map_extent *map_reverse_get_extent (Block b)
{
	return rb_search_map_reverse (b);
}

struct map_extent *map_reverse_get_extent_next(Block b)
{
	struct map_extent *extent;
	struct rb_node *n = reverse_tree_root.rb_node;

	while (n)
	{
		extent = rb_entry(n, struct map_extent, rb_reverse_node);

		if (b < extent->new)
		{
			if (n->rb_left == NULL)
				return rb_entry (rb_next(n), struct map_extent, rb_reverse_node);
			n = n->rb_left;
		}
		else if (b <= (extent->new + extent->count) )
			return extent;
		else {
			if (n->rb_right == NULL)
				return rb_entry (rb_next(n), struct map_extent, rb_reverse_node);
			n = n->rb_right;
		}
	}
	return NULL;
}



struct map_extent *map_reverse_first ()
{
	struct rb_node *node = rb_first (&reverse_tree_root);
	if (node == NULL)
		return NULL;
	return rb_entry (node, struct map_extent, rb_reverse_node);
}

struct map_extent *map_reverse_next (struct map_extent *e)
{
	struct rb_node *node = rb_next (&e->rb_reverse_node);
	if (node == NULL)
		return NULL;
	return rb_entry (node, struct map_extent, rb_reverse_node);
}


/* set new destination given old original location */

void map_forward_set (Block old, Block new)
{
	struct map_extent *e, *es1 = NULL, *es2 = NULL;
	char add = new ? 1 : 0;

	if (old == 0)
		goto ret;
	e = rb_search_map_forward (old);
	if (e) {
		/* exact match, may have to split */
		if (old - e->old + e->new == new)
			goto ret; /* already has new value, nothing to do */
		rb_erase (&e->rb_reverse_node, &reverse_tree_root);
		rb_erase (&e->rb_forward_node, &forward_tree_root);
		if (e->count == 0) {
			/* delete entry */
			free (e);
		}
		else if (e->old == old) {
			/* head split */
			e->old++;
			e->new++;
			e->count--;
			assert (e->count >= 0);
			es1 = e;
		} else if (e->old+e->count == old) {
			/* tail split */
			e->count--;
			assert (e->count >=0);
			es1 = e;
		} else {
			/* center split */
			/* add new extent for blocks after */
			struct map_extent *ne = malloc (sizeof(struct map_extent));
			assert (ne);
			ne->count = e->count - (old - e->old) - 1;
			/* truncate existing extent to blocks before */
			e->count = old - e->old - 1;
			ne->new = e->new + e->count + 2;
			ne->old = e->old + e->count + 2;
			es1 = e;
			es2 = ne;
			assert (e->count >= 0);
			assert (ne->count >= 0);
			/* now add the new extent */
		}
	}
	/* first need to remove the reverse mapping for the destination */
	e = rb_search_map_reverse (new);
	if (e) {
		rb_erase (&e->rb_reverse_node, &reverse_tree_root);
		rb_erase (&e->rb_forward_node, &forward_tree_root);
	}
	if (!e) {
		if (es1 && es1->new <= new && (es1->new + es1->count) >= new) {
			e = es1;
			es1 = NULL;
		}
		else if (es2 && es2->new <= new && (es2->new + es2->count) >= new) {
			e = es2;
			es2 = NULL;
		}
	}
	if (e) {
		/* exact match, may have to split */
		if (e->count == 0) {
			free (e);
		}
		else if (e->new == new) {
			/* head split */
			e->old++;
			e->new++;
			e->count--;
			rb_insert_map_forward(e);
			rb_insert_map_reverse(e);
			assert (e->count >= 0);
		} else if (e->new+e->count == new) {
			/* tail split */
			e->count--;
			rb_insert_map_forward(e);
			rb_insert_map_reverse(e);
			assert (e->count >=0);
		} else {
			/* center split */
			/* add new extent for blocks after */
			struct map_extent *ne = malloc (sizeof(struct map_extent));
			assert (ne);
			ne->count = e->count - (new - e->new) - 1;
			/* truncate existing extent to blocks before */
			e->count = new - e->new - 1;
			ne->new = e->new + e->count + 2;
			ne->old = e->old + e->count + 2;
			rb_insert_map_forward(e);
			rb_insert_map_reverse(e);
			rb_insert_map_forward(ne);
			rb_insert_map_reverse(ne);
			assert (e->count >= 0);
			assert (ne->count >= 0);
			/* now add the new extent */
		}
	}
	if (es1) {
		rb_insert_map_forward(es1);
		rb_insert_map_reverse(es1);
	}
	if (es2) {
		rb_insert_map_forward(es2);
		rb_insert_map_reverse(es2);
	}
	if (!add)
		goto ret;
	old++;
	new++;
	e = rb_search_map_forward(old);
	if (e && e->new == new) {
		/* head merge */
		rb_erase (&e->rb_reverse_node, &reverse_tree_root);
		rb_erase (&e->rb_forward_node, &forward_tree_root);
		e->old--;
		e->new--;
		e->count++;
		rb_insert_map_forward(e);
		rb_insert_map_reverse(e);
		goto ret;
	}
	old -= 2;
	new -= 2;
	e = rb_search_map_forward (old);
	if (e && (e->new + e->count) == new) {
		/* tail merge */
		rb_erase (&e->rb_reverse_node, &reverse_tree_root);
		rb_erase (&e->rb_forward_node, &forward_tree_root);
		e->count++;
		rb_insert_map_forward(e);
		rb_insert_map_reverse(e);
		goto ret;
	}
	old++;
	new++;
	/* simple case, no existing extent so add one */
	e = malloc (sizeof(struct map_extent));
	assert (e);
	e->old = old;
	e->new = new;
	e->count = 0;
	rb_insert_map_forward(e);
	rb_insert_map_reverse(e);
 ret:
	return;
}

void map_identity_add (Block start, Block count)
{
	struct map_extent *e;
	e = malloc (sizeof(struct map_extent));
	e->old = start;
	e->new = start;
	e->count = count;
	rb_insert_map_forward(e);
	rb_insert_map_reverse(e);
}

void dump_extents (void)
{
	struct rb_node *node = rb_first (&reverse_tree_root);
	while (node)
	{
		struct map_extent *e = container_of (node, struct map_extent, rb_reverse_node);
		printf ("old = %d, new = %d, count = %d\n", e->old, e->new, e->count);
		node = rb_next (node);
	}
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
	dump_extents();
	printf ("2 moves to %d\n", map_forward_get (2));
	printf ("3 moves to %d\n", map_forward_get (3));
	printf ("12 moves to %d\n", map_forward_get (12));
	printf ("19 comes from %d\n", map_reverse_get (19));
	printf ("57 comes from %d\n", map_reverse_get (57));
	return 0;
}
#endif
