/*
 *  linux/mm/page_alloc.c(Part)
 *
 * Modified code for lab2（实验二选做）
*/

//add includePath for function pagt_to_pfn
#include <asm-generic/memory_model.h>

//define the struct of FIFO
#define FIFO_size 10
typedef struct {
    unsigned long array[FIFO_size];
    int front;
    int rear;
    int count;
	spinlock_t lock; 
} FIFO;

static FIFO fifo;//Define global array variables

static void initFIFO(FIFO *fifo) {
    fifo->front = 0;
    fifo->rear = -1;
    fifo->count = 0;
	spin_lock_init(&fifo->lock);//built-in init function in Linux
}

static bool isFull(FIFO *fifo) {
    return fifo->count == FIFO_size;
}

  //add pfn of the new_page to the tail of the queue
static void enqueue(FIFO *fifo,unsigned long pfn) {
	unsigned long flags;
    //enter critical
    spin_lock_irqsave(&fifo->lock, flags); 
    //check if the queue is full
    if (isFull(fifo)) {
        unsigned long removed_value = fifo->array[fifo->front];
        //print dequeue information
		printk(KERN_DEBUG "Dequeued FIFO_page,whose pfn is %lu\n", removed_value);
		fifo->front = (fifo->front + 1) % FIFO_size;
        fifo->count--;
    }
    //new page enqueue
    fifo->rear = (fifo->rear + 1) % FIFO_size;
    fifo->array[fifo->rear] = pfn;
    fifo->count++;
    //exit critical
	spin_unlock_irqrestore(&fifo->lock, flags);
    //print enqueue information
	printk(KERN_DEBUG "add FIFO_page ,whose pfn is %lu\n",pfn);
}

void __init page_alloc_init(void)
{
	int ret;
	initFIFO(&fifo);//Modified here.Init FIFO array
	ret = cpuhp_setup_state_nocalls(CPUHP_PAGE_ALLOC_DEAD,
					"mm/page_alloc:dead", NULL,
					page_alloc_cpu_dead);
	WARN_ON(ret < 0);
}

/*
 * get_page_from_freelist goes through the zonelist trying to allocate
 * a page.
 */
static struct page *
get_page_from_freelist(gfp_t gfp_mask, unsigned int order, int alloc_flags,
						const struct alloc_context *ac)
{
	struct zoneref *z = ac->preferred_zoneref;
	struct zone *zone;
	struct pglist_data *last_pgdat_dirty_limit = NULL;

	/*
	 * Scan zonelist, looking for a zone with enough free.
	 * See also __cpuset_node_allowed() comment in kernel/cpuset.c.
	 */
	for_next_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
								ac->nodemask) {
		struct page *page;
		unsigned long mark;

		/* skip non-movable zone for normal user tasks */
		if (skip_none_movable_zone(gfp_mask, z))
			continue;

		/*
		 * CDM nodes get skipped if the requested gfp flag
		 * does not have __GFP_THISNODE set or the nodemask
		 * does not have any CDM nodes in case the nodemask
		 * is non NULL (explicit allocation requests from
		 * kernel or user process MPOL_BIND policy which has
		 * CDM nodes).
		 */
		if (is_cdm_node(zone->zone_pgdat->node_id)) {
			if (!(gfp_mask & __GFP_THISNODE)) {
				if (!ac->nodemask)
					continue;
			}
		}

		if (cpusets_enabled() &&
			(alloc_flags & ALLOC_CPUSET) &&
			!__cpuset_zone_allowed(zone, gfp_mask)
#ifdef CONFIG_COHERENT_DEVICE
			&& !(alloc_flags & ALLOC_CDM)
#endif
		)
				continue;
		/*
		 * When allocating a page cache page for writing, we
		 * want to get it from a node that is within its dirty
		 * limit, such that no single node holds more than its
		 * proportional share of globally allowed dirty pages.
		 * The dirty limits take into account the node's
		 * lowmem reserves and high watermark so that kswapd
		 * should be able to balance it without having to
		 * write pages from its LRU list.
		 *
		 * XXX: For now, allow allocations to potentially
		 * exceed the per-node dirty limit in the slowpath
		 * (spread_dirty_pages unset) before going into reclaim,
		 * which is important when on a NUMA setup the allowed
		 * nodes are together not big enough to reach the
		 * global limit.  The proper fix for these situations
		 * will require awareness of nodes in the
		 * dirty-throttling and the flusher threads.
		 */
		if (ac->spread_dirty_pages) {
			if (last_pgdat_dirty_limit == zone->zone_pgdat)
				continue;

			if (!node_dirty_ok(zone->zone_pgdat)) {
				last_pgdat_dirty_limit = zone->zone_pgdat;
				continue;
			}
		}

		mark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
		if (!zone_watermark_fast(zone, order, mark,
				       ac_classzone_idx(ac), alloc_flags)) {
			int ret;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
			/*
			 * Watermark failed for this zone, but see if we can
			 * grow this zone if it contains deferred pages.
			 */
			if (static_branch_unlikely(&deferred_pages)) {
				if (_deferred_grow_zone(zone, order))
					goto try_this_zone;
			}
#endif
			/* Checked here to keep the fast path fast */
			BUILD_BUG_ON(ALLOC_NO_WATERMARKS < NR_WMARK);
			if (alloc_flags & ALLOC_NO_WATERMARKS)
				goto try_this_zone;

			if (node_reclaim_mode == 0 ||
			    !zone_allows_reclaim(ac->preferred_zoneref->zone, zone))
				continue;

			ret = node_reclaim(zone->zone_pgdat, gfp_mask, order);
			switch (ret) {
			case NODE_RECLAIM_NOSCAN:
				/* did not scan */
				continue;
			case NODE_RECLAIM_FULL:
				/* scanned but unreclaimable */
				continue;
			default:
				/* did we reclaim enough */
				if (zone_watermark_ok(zone, order, mark,
						ac_classzone_idx(ac), alloc_flags))
					goto try_this_zone;

				continue;
			}
		}

try_this_zone:
		page = rmqueue(ac->preferred_zoneref->zone, zone, order,
				gfp_mask, alloc_flags, ac->migratetype);
		if (page) {
			prep_new_page(page, order, gfp_mask, alloc_flags);

			/*
			 * If this is a high-order atomic allocation then check
			 * if the pageblock should be reserved for the future
			 */
			if (unlikely(order && (alloc_flags & ALLOC_HARDER)))
				reserve_highatomic_pageblock(page, zone, order);
            //Modified here:get pfn of the page and add pfn to array if getting page correctly
			if(page) enqueue(&fifo,page_to_pfn(page));
			return page;
		} else {
#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
			/* Try again if zone has deferred pages */
			if (static_branch_unlikely(&deferred_pages)) {
				if (_deferred_grow_zone(zone, order))
					goto try_this_zone;
			}
#endif
		}
	}

	return NULL;
}