/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Ricardo Correia.
 * Use is subject to license terms.
 */

#include <sys/types.h>
#include <sys/debug.h>

#include <stdio.h>
#include <unistd.h>
#include <strings.h>

#include "vfs.h"

int ncpus;
uint64_t physmem;
unsigned long _pagesize;
unsigned int _pageshift;

void libsolkerncompat_init()
{
	/* LINUX */
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	physmem = sysconf(_SC_PHYS_PAGES);
	_pagesize = sysconf(_SC_PAGESIZE);
	_pageshift = ffs(_pagesize) - 1;

	VERIFY(ncpus > 0 && physmem > 0);

#ifdef DEBUG
	printf("ncpus = %i\n", ncpus);
	printf("physmem = %llu pages (%.2f GB)\n", (unsigned long long) physmem, (double) physmem * sysconf(_SC_PAGE_SIZE) / (1ULL << 30));
	printf("pagesize = %li, pageshift: %i\n", _pagesize, _pageshift);
#endif

	vfs_init();
}

void libsolkerncompat_exit()
{
	vfs_exit();
}