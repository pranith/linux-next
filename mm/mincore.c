/*
 *	linux/mm/mincore.c
 *
 * Copyright (C) 1994-2006  Linus Torvalds
 */

/*
 * The mincore() system call.
 */
#include <linux/pagemap.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/hugetlb.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>

static int mincore_hugetlb(pte_t *pte, unsigned long addr,
			unsigned long end, struct mm_walk *walk)
{
	int err = 0;
#ifdef CONFIG_HUGETLB_PAGE
	unsigned char present;
	unsigned char *vec = walk->private;

	/*
	 * Hugepages under user process are always in RAM and never
	 * swapped out, but theoretically it needs to be checked.
	 */
	present = pte && !huge_pte_none(huge_ptep_get(pte));
	for (; addr != end; vec++, addr += PAGE_SIZE)
		*vec = present;
	cond_resched();
	walk->private += (end - addr) >> PAGE_SHIFT;
#else
	BUG();
#endif
	return err;
}

/*
 * Later we can get more picky about what "in core" means precisely.
 * For now, simply check to see if the page is in the page cache,
 * and is up to date; i.e. that no page-in operation would be required
 * at this time if an application were to map and access this page.
 */
static unsigned char mincore_page(struct address_space *mapping, pgoff_t pgoff)
{
	unsigned char present = 0;
	struct page *page;

	/*
	 * When tmpfs swaps out a page from a file, any process mapping that
	 * file will not get a swp_entry_t in its pte, but rather it is like
	 * any other file mapping (ie. marked !present and faulted in with
	 * tmpfs's .fault). So swapped out tmpfs mappings are tested here.
	 */
#ifdef CONFIG_SWAP
	if (shmem_mapping(mapping)) {
		page = find_get_entry(mapping, pgoff);
		/*
		 * shmem/tmpfs may return swap: account for swapcache
		 * page too.
		 */
		if (radix_tree_exceptional_entry(page)) {
			swp_entry_t swp = radix_to_swp_entry(page);
			page = find_get_page(swap_address_space(swp), swp.val);
		}
	} else
		page = find_get_page(mapping, pgoff);
#else
	page = find_get_page(mapping, pgoff);
#endif
	if (page) {
		present = PageUptodate(page);
		page_cache_release(page);
	}

	return present;
}

static int mincore_hole(unsigned long addr, unsigned long end,
				struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	unsigned char *vec = walk->private;
	unsigned long nr = (end - addr) >> PAGE_SHIFT;
	int i;

	if (vma->vm_file) {
		pgoff_t pgoff;

		pgoff = linear_page_index(vma, addr);
		for (i = 0; i < nr; i++, pgoff++)
			vec[i] = mincore_page(vma->vm_file->f_mapping, pgoff);
	} else {
		for (i = 0; i < nr; i++)
			vec[i] = 0;
	}
	walk->private += nr;
	return 0;
}

static int mincore_pte(pte_t *pte, unsigned long addr, unsigned long end,
			struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	unsigned char *vec = walk->private;
	pgoff_t pgoff;

	if (pte_present(*pte))
		*vec = 1;
	else if (pte_file(*pte)) {
		pgoff = pte_to_pgoff(*pte);
		*vec = mincore_page(vma->vm_file->f_mapping, pgoff);
	} else { /* pte is a swap entry */
		swp_entry_t entry = pte_to_swp_entry(*pte);

		if (is_migration_entry(entry)) {
			/* migration entries are always uptodate */
			*vec = 1;
		} else {
#ifdef CONFIG_SWAP
			pgoff = entry.val;
			*vec = mincore_page(swap_address_space(entry),
					    pgoff);
#else
			WARN_ON(1);
			*vec = 1;
#endif
		}
	}
	walk->private++;
	return 0;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static int mincore_pmd(pmd_t *pmd, unsigned long addr, unsigned long end,
			struct mm_walk *walk)
{
	memset(walk->private, 1, (end - addr) >> PAGE_SHIFT);
	walk->private += (end - addr) >> PAGE_SHIFT;
	return 0;
}
#endif

/*
 * Do a chunk of "sys_mincore()". We've already checked
 * all the arguments, we hold the mmap semaphore: we should
 * just return the amount of info we're asked for.
 */
static long do_mincore(unsigned long addr, unsigned long pages,
		       unsigned char *vec)
{
	struct vm_area_struct *vma;
	int err;
	struct mm_walk mincore_walk = {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		.pmd_entry = mincore_pmd,
#endif
		.pte_entry = mincore_pte,
		.pte_hole = mincore_hole,
		.hugetlb_entry = mincore_hugetlb,
		.private = vec,
	};

	vma = find_vma(current->mm, addr);
	if (!vma || addr < vma->vm_start)
		return -ENOMEM;
	mincore_walk.vma = vma;
	mincore_walk.mm = vma->vm_mm;

	err = walk_page_vma(vma, &mincore_walk);
	if (err < 0) {
		return err;
	} else {
		unsigned long end;

		end = min(vma->vm_end, addr + (pages << PAGE_SHIFT));
		return (end - addr) >> PAGE_SHIFT;
	}
}

/*
 * The mincore(2) system call.
 *
 * mincore() returns the memory residency status of the pages in the
 * current process's address space specified by [addr, addr + len).
 * The status is returned in a vector of bytes.  The least significant
 * bit of each byte is 1 if the referenced page is in memory, otherwise
 * it is zero.
 *
 * Because the status of a page can change after mincore() checks it
 * but before it returns to the application, the returned vector may
 * contain stale information.  Only locked pages are guaranteed to
 * remain in memory.
 *
 * return values:
 *  zero    - success
 *  -EFAULT - vec points to an illegal address
 *  -EINVAL - addr is not a multiple of PAGE_CACHE_SIZE
 *  -ENOMEM - Addresses in the range [addr, addr + len] are
 *		invalid for the address space of this process, or
 *		specify one or more pages which are not currently
 *		mapped
 *  -EAGAIN - A kernel resource was temporarily unavailable.
 */
SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
		unsigned char __user *, vec)
{
	long retval;
	unsigned long pages;
	unsigned char *tmp;

	/* Check the start address: needs to be page-aligned.. */
 	if (start & ~PAGE_CACHE_MASK)
		return -EINVAL;

	/* ..and we need to be passed a valid user-space range */
	if (!access_ok(VERIFY_READ, (void __user *) start, len))
		return -ENOMEM;

	/* This also avoids any overflows on PAGE_CACHE_ALIGN */
	pages = len >> PAGE_SHIFT;
	pages += (len & ~PAGE_MASK) != 0;

	if (!access_ok(VERIFY_WRITE, vec, pages))
		return -EFAULT;

	tmp = (void *) __get_free_page(GFP_USER);
	if (!tmp)
		return -EAGAIN;

	retval = 0;
	while (pages) {
		/*
		 * Do at most PAGE_SIZE entries per iteration, due to
		 * the temporary buffer size.
		 */
		down_read(&current->mm->mmap_sem);
		retval = do_mincore(start, min(pages, PAGE_SIZE), tmp);
		up_read(&current->mm->mmap_sem);

		if (retval <= 0)
			break;
		if (copy_to_user(vec, tmp, retval)) {
			retval = -EFAULT;
			break;
		}
		pages -= retval;
		vec += retval;
		start += retval << PAGE_SHIFT;
		retval = 0;
	}
	free_page((unsigned long) tmp);
	return retval;
}
