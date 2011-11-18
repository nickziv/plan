#include <stdlib.h>
#include <unistd.h>
#include "plan_probes.h"

/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the Common Development
 * and Distribution License (the "License").  You may not use this file except
 * in compliance with the License.
 *
 * You can obtain a copy of the license at src/PLAN.LICENSE.  See the License
 * for the specific language governing permissions and limitations under the
 * License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each file and
 * include the License file at src/PLAN.LICENSE.  If applicable, add the
 * following below this CDDL HEADER, with the fields enclosed by brackets "[]"
 * replaced with your own identifying information:
 * Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2011, Nick Zivkovic. All rights reserved.
 */


/*
 * My build of illumos doesn't have the new "dynamic tracemem" feature, so I
 * have use this probing method below, to see what is about to be
 * read/written.
 */

void
atomic_read(int fd, void *buf, size_t sz)
{
	int total_read = 0;
	int red = 0;
	int probe_ix = 0;
	int probe_sz = (sz/8) ? (sz/8) : 1;
	probe_sz += (sz%8) ? 1 : 0;

	while (PLAN_ATOMIC_READ_ENABLED() && probe_ix < probe_sz && buf && sz) {
		PLAN_ATOMIC_READ(fd, ((uint64_t*)buf)[probe_ix], sz);
		probe_ix++;
	}

	do {
		red = read(fd, buf, (sz-red));
		total_read += red;
	} while (total_read < sz && red != -1);
}


void 
atomic_write(int fd, void *buf, size_t sz)
{

	int total_written = 0;
	int written = 0;
	int probe_ix = 0;
	int probe_sz = (sz/8) ? (sz/8) : 1;
	probe_sz += (sz%8) ? 1 : 0;

	while (PLAN_ATOMIC_WRITE_ENABLED() && probe_ix < probe_sz && buf
	    && sz) {
		PLAN_ATOMIC_WRITE(fd, ((uint64_t*)buf)[probe_ix], sz);
		probe_ix++;
	}

	do {
		written = write(fd, buf, (sz-written));
		total_written += written;
	} while (total_written < sz && written != -1);
}
