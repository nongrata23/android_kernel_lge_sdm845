/*
 * drivers/staging/android/ion/ion_page_pool.c
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2016, 2018 The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/llist.h>
#include <linux/vmalloc.h>
#include "ion_priv.h"

static void *ion_page_pool_alloc_pages(struct ion_page_pool *pool)
{
	struct page *page;

	page = alloc_pages(pool->gfp_mask & ~__GFP_ZERO, pool->order);

	if (!page)
		return NULL;

	if (pool->gfp_mask & __GFP_ZERO)
		if (msm_ion_heap_high_order_page_zero(pool->dev, page,
						      pool->order))
			goto error_free_pages;

	ion_page_pool_alloc_set_cache_policy(pool, page);
	mod_node_page_state(page_pgdat(page), NR_ION_HEAP,
				1 << pool->order);

	return page;
error_free_pages:
	__free_pages(page, pool->order);
	return NULL;
}

static void ion_page_pool_free_pages(struct ion_page_pool *pool,
				     struct page *page)
{
	ion_page_pool_free_set_cache_policy(pool, page);
	__free_pages(page, pool->order);
	mod_node_page_state(page_pgdat(page), NR_ION_HEAP,
				-(1 << pool->order));
}

static int ion_page_pool_add(struct ion_page_pool *pool, struct page *page)
{
	if (PageHighMem(page)) {
		llist_add((struct llist_node *)&page->lru, &pool->high_items);
		atomic_inc(&pool->high_count);
	} else {
		llist_add((struct llist_node *)&page->lru, &pool->low_items);
		atomic_inc(&pool->low_count);
	}

	mod_node_page_state(page_pgdat(page), NR_INDIRECTLY_RECLAIMABLE_BYTES,
			    (1 << (PAGE_SHIFT + pool->order)));
	return 0;
}

static struct page *ion_page_pool_remove(struct ion_page_pool *pool, bool high)
{
	struct page *page = NULL;
	struct llist_node *ret;

	if (high) {
		BUG_ON(!atomic_read(&pool->high_count));
		ret = llist_del_first(&pool->high_items);
		if (ret)
			page = llist_entry((struct list_head *)ret, struct page, lru);
		atomic_dec(&pool->high_count);
	} else {
		BUG_ON(!atomic_read(&pool->low_count));
		ret = llist_del_first(&pool->low_items);
		if (ret)
			page = llist_entry((struct list_head *)ret, struct page, lru);
		atomic_dec(&pool->low_count);
	}

	mod_node_page_state(page_pgdat(page), NR_INDIRECTLY_RECLAIMABLE_BYTES,
			    -(1 << (PAGE_SHIFT + pool->order)));
	return page;
}

void *ion_page_pool_alloc(struct ion_page_pool *pool, bool *from_pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	*from_pool = true;

	if (mutex_trylock(&pool->mutex)) {
		if (atomic_read(&pool->high_count))
			page = ion_page_pool_remove(pool, true);
		else if (atomic_read(&pool->low_count))
			page = ion_page_pool_remove(pool, false);
		mutex_unlock(&pool->mutex);
	}
	if (!page) {
#ifdef CONFIG_MIGRATE_HIGHORDER
		if (pool->order > 0 &&
				(global_page_state(NR_FREE_HIGHORDER_PAGES) < (1 << pool->order)))
			return page;
#endif
		page = ion_page_pool_alloc_pages(pool);
		*from_pool = false;
	}
	return page;
}

/*
 * Tries to allocate from only the specified Pool and returns NULL otherwise
 */
void *ion_page_pool_alloc_pool_only(struct ion_page_pool *pool)
{
	struct page *page = NULL;

	if (!pool)
		return NULL;

	if (mutex_trylock(&pool->mutex)) {
		if (atomic_read(&pool->high_count))
			page = ion_page_pool_remove(pool, true);
		else if (atomic_read(&pool->low_count))
			page = ion_page_pool_remove(pool, false);
		mutex_unlock(&pool->mutex);
	}

	return page;
}

void ion_page_pool_free(struct ion_page_pool *pool, struct page *page)
{
	int ret;

	ret = ion_page_pool_add(pool, page);
	if (ret)
		ion_page_pool_free_pages(pool, page);
}

void ion_page_pool_free_immediate(struct ion_page_pool *pool, struct page *page)
{
	ion_page_pool_free_pages(pool, page);
}

int ion_page_pool_total(struct ion_page_pool *pool, bool high)
{
	int count = atomic_read(&pool->low_count);

	if (high)
		count += atomic_read(&pool->high_count);

	return count << pool->order;
}

int ion_page_pool_shrink(struct ion_page_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	int freed = 0;
	bool high;

	if (current_is_kswapd())
		high = true;
	else
		high = !!(gfp_mask & __GFP_HIGHMEM);

	if (nr_to_scan == 0)
		return ion_page_pool_total(pool, high);

	while (freed < nr_to_scan) {
		struct page *page;

		mutex_lock(&pool->mutex);
		if (atomic_read(&pool->low_count)) {
			page = ion_page_pool_remove(pool, false);
		} else if (high && atomic_read(&pool->high_count)) {
			page = ion_page_pool_remove(pool, true);
		} else {
			mutex_unlock(&pool->mutex);
			break;
		}
		mutex_unlock(&pool->mutex);
		ion_page_pool_free_pages(pool, page);
		freed += (1 << pool->order);
	}

	return freed;
}

struct ion_page_pool *ion_page_pool_create(struct device *dev, gfp_t gfp_mask,
					   unsigned int order)
{
	struct ion_page_pool *pool = kmalloc(sizeof(*pool), GFP_KERNEL);

	if (!pool)
		return NULL;
	pool->dev = dev;
	atomic_set(&pool->high_count, 0);
	atomic_set(&pool->low_count, 0);
	init_llist_head(&pool->low_items);
	init_llist_head(&pool->high_items);
	pool->gfp_mask = gfp_mask;
	pool->order = order;
	mutex_init(&pool->mutex);
	plist_node_init(&pool->list, order);

	return pool;
}

void ion_page_pool_destroy(struct ion_page_pool *pool)
{
	kfree(pool);
}

static int __init ion_page_pool_init(void)
{
	return 0;
}
device_initcall(ion_page_pool_init);
