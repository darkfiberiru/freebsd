/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * Copyright (c) 2013-2016 Mellanox Technologies, Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */
#ifndef	_LINUX_PAGE_H_
#define _LINUX_PAGE_H_

#include <linux/types.h>

#include <sys/param.h>
#include <sys/eventhandler.h>

#include <machine/atomic.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>

#define page	vm_page

typedef unsigned long pte_t;
typedef unsigned long pmd_t;
typedef unsigned long pgd_t;
typedef unsigned long pgprot_t;


#define cachemode2protval(attr) (((pgprot_t)attr) << 3)
#define pgprot2cachemode(prot) (((vm_memattr_t)((prot) >> 3) == 0) ? VM_MEMATTR_DEFAULT  : ((vm_memattr_t)((prot) >> 3)))

#define	virt_to_page(x)		PHYS_TO_VM_PAGE(vtophys((x)))
#define	page_to_pfn(pp)		(VM_PAGE_TO_PHYS((pp)) >> PAGE_SHIFT)
#define	pfn_to_page(pfn)	(PHYS_TO_VM_PAGE((pfn) << PAGE_SHIFT))
#define	nth_page(page,n)	pfn_to_page(page_to_pfn((page)) + (n))

#define	clear_page(page)		memset((page), 0, PAGE_SIZE)
#define	pgprot_noncached(prot)		((prot) | cachemode2protval(VM_MEMATTR_UNCACHEABLE))
#define	pgprot_writecombine(prot)	((prot) | cachemode2protval(VM_MEMATTR_WRITE_COMBINING))

#undef	PAGE_MASK
#define	PAGE_MASK	(~(PAGE_SIZE-1))
/*
 * Modifying PAGE_MASK in the above way breaks trunc_page, round_page, and btoc
 * macros.  Therefore, redefine them in a way that makes sense so linuxkpi
 * consumers don't get totally broken behavior.
 */
#undef	btoc
#define	btoc(x)	(((vm_offset_t)(x)+PAGE_SIZE-1)>>PAGE_SHIFT)
#undef	round_page
#define	round_page(x)	((((uintptr_t)(x)) + PAGE_SIZE-1) & ~(PAGE_SIZE-1))
#undef	trunc_page
#define	trunc_page(x)	((uintptr_t)(x) & ~(PAGE_SIZE-1))

struct io_mapping {
	vm_paddr_t base;
	unsigned long size;
	vm_prot_t prot;
	struct resource *r;
};
/* XXX note that this is incomplete */
void *kmap(vm_page_t page);
void *kmap_atomic(vm_page_t page);
void *kmap_atomic_prot(vm_page_t page, pgprot_t prot);
void kunmap(vm_page_t page);
void kunmap_atomic(void *vaddr);
void page_cache_release(vm_page_t page);

void *acpi_os_ioremap(vm_paddr_t pa, vm_size_t size);


extern struct io_mapping *io_mapping_create_wc(vm_paddr_t base, unsigned long size);
extern void unmap_mapping_range(void *obj,
				loff_t const holebegin, loff_t const holelen, int even_cows);
extern void io_mapping_free(struct io_mapping *mapping);
extern void * iomap_atomic_prot_pfn(unsigned long pfn, vm_prot_t prot);

void iounmap_atomic(void *vaddr);


static inline void
io_mapping_unmap_atomic(void *vaddr)
{
	iounmap_atomic(vaddr);
}


extern void *io_mapping_map_atomic_wc(struct io_mapping *mapping, unsigned long offset);
extern void *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset);
extern int set_memory_wc(unsigned long addr, int numpages);

static inline int
set_memory_wb(unsigned long addr, int numpages)
{

	return (pmap_change_attr(addr, numpages, PAT_WRITE_BACK));
}

static inline int
set_pages_wb(vm_page_t page, int numpages)
{
	unsigned long addr = (unsigned long)VM_PAGE_TO_PHYS(page);

	return set_memory_wb(addr, numpages);
}

vm_page_t alloc_page(gfp_t gfp);
void __free_page(vm_page_t page);

/* bump refcount */
int set_pages_uc(vm_page_t page, int numpages);
int set_memory_uc(unsigned long addr, int numpages);
vm_paddr_t page_to_phys(vm_page_t page);

void *acpi_os_ioremap(vm_paddr_t pa, vm_size_t size);

void unmap_mapping_range(void *obj,
			 loff_t const holebegin, loff_t const holelen, int even_cows);

#define cpu_has_pat pat_works

extern void linux_clflushopt(u_long addr);
#endif	/* _LINUX_PAGE_H_ */
