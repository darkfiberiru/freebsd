
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/queue.h>

#include <linux/slab.h>

STAILQ_HEAD(, cached_page) cached_page_list;

#define CACHED_PAGES_MAX 32
#define CACHED_PAGES_MIN 16

static int cached_page_count;
#define CACHED_PAGE_MAGIC 0xCAFECACA

static struct mtx kmalloc_cached_mtx;

struct cached_page {
	STAILQ_ENTRY(cached_page) page_entry;
	char pad[PAGE_SIZE - sizeof(void *) - sizeof(uint64_t)];
	uint64_t page_magic;
};

static void
kmalloc_cached_init(void *arg __unused)
{
	int i;
	struct cached_page *p;

	mtx_init(&kmalloc_cached_mtx, "kmalloc_cache", NULL, MTX_SPIN);
	STAILQ_INIT(&cached_page_list);
	for (i = 0; i < CACHED_PAGES_MAX; i++) {
		p = malloc(PAGE_SIZE, M_KMALLOC, M_ZERO|M_WAITOK);
		p->page_magic = CACHED_PAGE_MAGIC;
		STAILQ_INSERT_HEAD(&cached_page_list, p, page_entry);
	}
	cached_page_count = CACHED_PAGES_MAX;
}

SYSINIT(linux_kmalloc_cached, SI_SUB_KTHREAD_PAGE, SI_ORDER_SECOND, kmalloc_cached_init, NULL);

void *
kmalloc_cached(int size, gfp_t flags)
{
	struct cached_page *p;

	if (size > (PAGE_SIZE - 32))
		return (NULL);
	if (STAILQ_EMPTY(&cached_page_list))
		return (NULL);
	mtx_lock_spin(&kmalloc_cached_mtx);
	p = STAILQ_FIRST(&cached_page_list);
	if (p != NULL) {
		STAILQ_REMOVE_HEAD(&cached_page_list, page_entry);
		if (flags & M_ZERO)
			bzero(p, size);
		cached_page_count--;
	}
	mtx_unlock_spin(&kmalloc_cached_mtx);
	return (p);
}


void
kfree_cached(void *ptr)
{
	struct cached_page *p = ptr;

	if (p->page_magic == CACHED_PAGE_MAGIC) {
		mtx_lock_spin(&kmalloc_cached_mtx);
		STAILQ_INSERT_HEAD(&cached_page_list, p, page_entry);
		cached_page_count++;
		mtx_unlock_spin(&kmalloc_cached_mtx);
	}
	if (cached_page_count < CACHED_PAGES_MIN &&
	    (curthread->td_intr_nesting_level == 0 && curthread->td_critnest == 0)) {
		do {
			p = malloc(PAGE_SIZE, M_KMALLOC, M_NOWAIT);
			if (p == NULL)
				break;
			p->page_magic = CACHED_PAGE_MAGIC;
			mtx_lock_spin(&kmalloc_cached_mtx);
			STAILQ_INSERT_HEAD(&cached_page_list, p, page_entry);
			cached_page_count++;
			mtx_unlock_spin(&kmalloc_cached_mtx);
		} while (cached_page_count < CACHED_PAGES_MAX);
	}
}
