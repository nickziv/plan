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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <umem.h>
#include <sys/vmem.h>
#include <strings.h>
#include <unistd.h>
#include <stdio.h>
#include "plan_impl.h"
#include "plan_probes.h"
#define	MEM2TIME(p) ((int)(p - 1))
#define	ALLRWX (S_IRWXU | S_IRWXG | S_IRWXO)

char *daystr[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
	"Friday", "Saturday"};
/*
 * We can, at most, have 1440 actions in a day. Here, we queue up the
 * actions as we reallocate them in the loop in read_act_dir.
 */
static act_t *a[1440];
static todo_t **t;
static size_t tsz;
static size_t t_elems;
static size_t a_elems;

extern int write_dur;
extern int days_fd;
extern int dates_fd;
extern int exdates_fd;
extern int todos_fd;
extern vmem_t *vmday;

extern short month_day_tbl[];

extern umem_cache_t *act_cache;
extern umem_cache_t *todo_cache;

extern void atomic_read(int, void*, size_t);
extern void atomic_write(int, void*, size_t);

/*
 * Here we compare two activities. But since this function is to be used on an
 * array of _pointer_ and NOT on an array of activities, we pass pointers to
 * the pointers. So, we have to dereference them twice to get at the data.
 * More importantly, activities are sorted by their start-time, and not their
 * name.
 */
static int
comp_act_ptrs(const void *a1, const void *a2)
{
	act_t *a1p = *(act_t **)a1;
	act_t *a2p = *(act_t **)a2;

	if (a1p->act_time < a2p->act_time) {
		return (-1);
	}

	if (a1p->act_time > a2p->act_time) {
		return (1);
	}

	return (0);
}

static int
comp_todo_ptrs(const void *t1, const void *t2)
{
	todo_t *t1p = *(todo_t **)t1;
	todo_t *t2p = *(todo_t **)t2;

	if (t1p->td_time < t2p->td_time) {
		return (-1);
	}

	if (t1p->td_time > t2p->td_time) {
		return (1);
	}

	return (0);
}

static size_t
get_total_usage()
{
	int i = 0;
	size_t usage = 0;
	while (i < a_elems && a_elems != 0) {
		usage += a[i]->act_dur;
		i++;
	}
	return (usage);
}

static int
openday(day_t day)
{
	char *path;
	switch (day) {

	case MON:
		path = "mon";
		break;
	case TUES:
		path = "tues";
		break;
	case WED:
		path = "wed";
		break;
	case THUR:
		path = "thur";
		break;
	case FRI:
		path = "fri";
		break;
	case SAT:
		path = "sat";
		break;
	case SUN:
		path = "sun";
		break;

	default:
		return (-1);
	}

	mkdirat(days_fd, path, ALLRWX);
	return (openat(days_fd, path, O_RDONLY));
}

static int
havedate(tm_t *date)
{
	char ypath[] = {0, 0, 0, 0, 0};
	char mpath[] = {0, 0, 0};
	char dpath[] = {0, 0, 0};


	strftime(ypath, sizeof (ypath), "%Y", date);
	strftime(mpath, sizeof (ypath), "%m", date);
	strftime(dpath, sizeof (ypath), "%d", date);
	int yfd = openat(dates_fd, &ypath[0], O_RDONLY);
	int mfd = openat(yfd, &mpath[0], O_RDONLY);
	int dfd = openat(mfd, &dpath[0], O_RDONLY);
	close(yfd);
	close(mfd);
	close(dfd);

	if (yfd == -1 || mfd == -1 || dfd == -1) {
		return (0);
	}

	return (1);
}

static int
opendate(tm_t *date)
{
	char ypath[] = {0, 0, 0, 0, 0};
	char mpath[] = {0, 0, 0};
	char dpath[] = {0, 0, 0};


	strftime(ypath, sizeof (ypath), "%Y", date);
	strftime(mpath, sizeof (mpath), "%m", date);
	strftime(dpath, sizeof (dpath), "%d", date);

	mkdirat(dates_fd, ypath, ALLRWX);

	int yfd = openat(dates_fd, &ypath[0], O_RDONLY);

	mkdirat(yfd, &mpath[0], ALLRWX);

	int mfd = openat(yfd, &mpath[0], O_RDONLY);

	mkdirat(mfd, dpath, ALLRWX);

	int dfd = openat(mfd, &dpath[0], O_RDONLY);

	close(yfd);
	close(mfd);
	return (dfd);
}

static int
openacts(int dfd)
{
	mkdirat(dfd, "acts", ALLRWX);
	return (openat(dfd, "acts", O_RDONLY));
}

static int
opentodos(int dfd)
{
	if (dfd != -1) {
		mkdirat(dfd, "todos", ALLRWX);
		return (openat(dfd, "todos", O_RDONLY));
	}
	return (todos_fd);
}


static int
get_awake_range(int day, tm_t *date, size_t *s, size_t *off)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}
	int awake_xattr = openat(dfd, "awake", O_XATTR | O_RDONLY);
	int dur_xattr = openat(dfd, "dur", O_XATTR | O_RDONLY);
	/*
	 * If this day has no awake or dur attr's we apparently have full day
	 * on our hands.
	 */
	if (awake_xattr == -1 || dur_xattr == -1) {
		*s = 0;
		*off = 1440;
	}
	atomic_read(awake_xattr, s, sizeof (size_t));
	atomic_read(awake_xattr, off, sizeof (size_t));
	close(awake_xattr);
	close(dur_xattr);
	close(dfd);
	return (0);
}


int
create_act(char *n, day_t day, tm_t *date)
{
	int dfd;

	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	int adfd = openacts(dfd);
	int afd = openat(adfd, n, O_RDWR, ALLRWX);
	if (afd != -1) {
		return (CREATE_EEXIST);
	}
	size_t dval = 0;
	char yval = 1;
	int tval = -1;
	afd = openat(adfd, n, O_CREAT | O_RDWR, ALLRWX);
	int time_xattr = openat(afd, "time",
		O_XATTR | O_CREAT | O_RDWR, ALLRWX);
	int dur_xattr = openat(afd, "dur",
		O_XATTR | O_CREAT | O_RDWR, ALLRWX);
	int dyn_xattr = openat(afd, "dyn",
		O_XATTR | O_CREAT | O_RDWR, ALLRWX);
	atomic_write(time_xattr, &tval, sizeof (int));
	atomic_write(dur_xattr, &dval, sizeof (size_t));
	atomic_write(dyn_xattr, &yval, sizeof (char));
	close(dfd);
	close(adfd);
	close(afd);
	close(time_xattr);
	close(dur_xattr);
	close(dyn_xattr);
	return (0);
}

int
create_todo(char *n, int day, tm_t *date)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}


	int tdfd;
	tdfd = opentodos(dfd);

	int tfd = openat(tdfd, n, O_RDWR, ALLRWX);
	if (tfd != -1) {
		return (CREATE_TD_EEXIST);
	}
	int tval = 0;
	tfd = openat(tdfd, n, O_CREAT | O_RDWR, ALLRWX);
	if (tfd == -1) {
		perror("ctodo");
		exit(0);
	}
	int time_xattr = openat(tfd, "time",
		O_XATTR | O_CREAT | O_RDWR, ALLRWX);
	if (time_xattr == -1) {
		perror("ctodo");
		exit(0);
	}
	atomic_write(time_xattr, &tval, sizeof (int));
	close(dfd);
	close(tdfd);
	close(tfd);
	close(time_xattr);
	return (0);

}


int
destroy_act(char *n, day_t day, tm_t *date)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}
	int adfd = openacts(dfd);
	int uaret = unlinkat(adfd, n, 0);
	if (uaret == -1) {
		return (DESTROY_EEXIST);
	}
	close(dfd);
	close(adfd);
	return (0);
}

int
destroy_todo(char *n, day_t day, tm_t *date)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}
	int tdfd = opentodos(dfd);
	int uaret = unlinkat(tdfd, n, 0);
	if (uaret == -1) {
		return (DESTROY_TD_EEXIST);
	}
	close(dfd);
	close(tdfd);
	return (0);
}

int
rename_act(char *old, char *new, day_t day, tm_t *date)
{
	int dfd;

	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	int adfd = openacts(dfd);
	int nfd = openat(adfd, new, O_RDONLY);
	if (nfd >= 0) {
		return (RN_ENEWEXIST);
	}
	renameat(adfd, old, adfd, new);
	close(dfd);
	close(adfd);
	return (0);
}


int
rename_todo(char *old, char *new, day_t day, tm_t *date)
{
	int dfd;

	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	int adfd = opentodos(dfd);
	int nfd = openat(adfd, new, O_RDONLY);
	if (nfd >= 0) {
		return (RN_TD_ENEWEXIST);
	}
	renameat(adfd, old, adfd, new);
	close(dfd);
	close(adfd);
	return (0);
}



static ra_err_t realloc_err;

/*
 * The below macro was used
 */
static void
read_todo_dir(int tfd, int det)
{

	struct dirent *de = NULL;
	DIR *todos_dir = fdopendir(tfd);
	size_t i = 0;
	size_t tsz2 = 0;
	tsz = 0;
	size_t psz = sizeof (todo_t *);
	int dotdirs = 1;
	while ((de = readdir(todos_dir)) != NULL) {
		/*
		 * The first 2 dirents are always '.' and '..'
		 * We skip those.
		 */
		if (dotdirs < 3) {
			dotdirs++;
			continue;
		}
		if ((i*psz) == tsz) {
			PLAN_GOTHERE(0);
			if (i == 0) {
			PLAN_GOTHERE(1);
				tsz = 100*psz;
				tsz2 = tsz;
			} else {
			PLAN_GOTHERE(2);
				tsz2 = tsz * 2;
			}
			todo_t **t2 = umem_alloc(tsz2, UMEM_NOFAIL);
			PLAN_GOTHERE(3);
			if (i) {
				bcopy(t, t2, tsz);
			}
			PLAN_GOTHERE(4);
			umem_free(t, tsz);
			PLAN_GOTHERE(5);
			t = t2;
			PLAN_GOTHERE(6);
			tsz = tsz2;
			PLAN_GOTHERE(7);
		}
		int sl = strnlen(de->d_name, 255);
			PLAN_GOTHERE(8);
		t[i] = umem_cache_alloc(todo_cache, UMEM_NOFAIL);
			PLAN_GOTHERE(9);
		char *name_str = umem_zalloc((sl+1), UMEM_NOFAIL);
			PLAN_GOTHERE(10);
		bcopy(de->d_name, name_str, (sl+1));
			PLAN_GOTHERE(11);
		t[i]->td_name_len = sl;
			PLAN_GOTHERE(12);
		t[i]->td_name = name_str;
			PLAN_GOTHERE(13);
		int todo_fd = openat(tfd, de->d_name, O_RDWR);
			PLAN_GOTHERE(14);
		int time_xattr = openat(todo_fd, "time",
			O_XATTR | O_RDWR | O_CREAT, ALLRWX);
			PLAN_GOTHERE(15);

		PLAN_READ_TODO(t[i]->td_name, t[i]->td_time);

		atomic_read(time_xattr, &t[i]->td_time,
			sizeof (int));
		
		struct stat det_stat;
		size_t len;
		if (det) {
			fstat(todo_fd, &det_stat);
			len = det_stat.st_size + 1; /* +1 for \0 */
			if (len - 1) {
				t[i]->td_det = umem_zalloc(len, UMEM_NOFAIL);
				atomic_read(todo_fd, t[i]->td_det, len);
			}
		}

		PLAN_READ_TODO(t[i]->td_name, t[i]->td_time);
			PLAN_GOTHERE(16);

		close(time_xattr);
			PLAN_GOTHERE(17);

		close(todo_fd);
		i++;
	}
	t_elems = i;
}

static void
mk_copy_act(act_t *src, act_t **des)
{
	*des = umem_cache_alloc(act_cache, UMEM_NOFAIL);
	bcopy(src, *des, sizeof (act_t));
	(*des)->act_name_len = 0;
	(*des)->act_name = src->act_name;
	/* *des->act_dyn = src->act_dyn; */
}

/*
 * loop
 *   open act-name attrs
 *     open the attrs of those names
 */
static void
read_act_dir(int afd, size_t base, size_t off, int det)
{
	struct dirent *de = NULL;
	int time_xattr;
	int dur_xattr;
	int dyn_xattr;
	int i = 0;
	DIR *acts_dir = fdopendir(afd);
	int dotdirs = 1;
	while ((de = readdir(acts_dir)) != NULL) {
		PLAN_GOTHERE(de->d_name);
		/*
		 * The first 2 dirents are always '.' and '..'
		 * We skip those.
		 */
		if (dotdirs < 3) {
			dotdirs++;
			continue;
		}

		PLAN_GOTHERE(de->d_name);
		int sl = strnlen(de->d_name, 255);
		a[i] = umem_cache_alloc(act_cache, UMEM_NOFAIL);
		PLAN_ACT_PTR(a[i]);
		char *name_str = umem_zalloc((sl+1), UMEM_NOFAIL);
		bcopy(de->d_name, name_str, sl);
		a[i]->act_name_len = sl;
		a[i]->act_name = name_str;
		int act_fd = openat(afd, de->d_name, O_RDWR);
		if (act_fd == -1) {
			perror("act_fd");
			exit(0);
		}
		time_xattr =
			openat(act_fd, "time",
				O_CREAT | O_XATTR | O_RDWR, ALLRWX);
		a[i]->act_fd_time = time_xattr;
		if (time_xattr == -1) {
			perror("time_xattr - open");
			exit(0);
		}
		dur_xattr =
			openat(act_fd, "dur",
				O_CREAT | O_XATTR | O_RDWR, ALLRWX);
		a[i]->act_fd_dur = dur_xattr;
		dyn_xattr =
			openat(act_fd, "dyn",
				O_CREAT | O_XATTR | O_RDWR, ALLRWX);


		int txr;
		struct stat time_stat;
		fstat(time_xattr, &time_stat);
		int ntimes = (time_stat.st_size)/sizeof (int);
		atomic_read(dur_xattr, &(a[i]->act_dur),
			sizeof (size_t));
		atomic_read(dyn_xattr, &(a[i]->act_dyn),
			sizeof (char));

		struct stat det_stat;
		size_t len;
		if (det) {
			fstat(act_fd, &det_stat);
			len = det_stat.st_size + 1; /* +1 for \0 */
			if (len-1) {
				a[i]->act_det = umem_zalloc(len, UMEM_NOFAIL);
				atomic_read(act_fd, a[i]->act_det, len);
			}
		}

		// lseek(time_xattr, 0, SEEK_SET);
		// lseek(dyn_xattr, 0, SEEK_SET);
		lseek(dur_xattr, 0, SEEK_SET);

		atomic_read(time_xattr, &(a[i]->act_time),
			sizeof (int));
		PLAN_READ_ACT(a[i]->act_name, a[i]->act_time, a[i]->act_dur);
		ntimes--;


		/*
		 * If we know that the action is dynamic, we can place
		 * it anywhere within the start and end of the day.
		 * If it is not dynamic, we have to fit it within it's
		 * starting time and the end of the day.
		 */
vm_set:;
		if (a[i]->act_dyn) {
			a[i]->act_vmmin = (void *) (1 + base);
			a[i]->act_vmmax = a[i]->act_vmmin + off;
		} else {
			a[i]->act_vmmin = (void *)(1 + a[i]->act_time);
			a[i]->act_vmmax =
				a[i]->act_vmmin + a[i]->act_dur;
			/*
			 * If the min boundary is lower than when the
			 * user starts the day, or the max boundary is
			 * greater than when the user ends the day, we
			 * can't fit this particular action into the
			 * day. And so, we must bail, informing the
			 * user of the inconsistency.
			 */
			char *dstart = (void *) (1 + base);
			char *dend = dstart + off;
			if ((a[i]->act_vmmin) < dstart ||
			    (a[i]->act_vmmax > dend)) {
				realloc_err.rae_code = RAE_CODE_FIT;
				realloc_err.rae_act = a[i];
			}
		}

		/*
		 * We know that there are no more than 1 chunks, so we seek the
		 * time_xattr to zero, so that we can overwrite the initial
		 * data.
		 */
		if (!ntimes) {
			lseek(time_xattr, 0, SEEK_SET);
		}

		/*
		 * If we are allocating an activity in chunks, we read through
		 * the rest of the time-xattr file, creating new activities,
		 * which are clones of the initial a[i], but differ only in the
		 * starting time of the activity, and the vmem related values.
		 * The reason we do this and, and don't have some nested
		 * structure (like a tree or a list) is because we want to be
		 * able to list all activities in chronological order, using
		 * the qsort routine to sort them in this order.  Ultimately,
		 * any kind of nesting would save a slim amount of memory, but
		 * spike our CPU consumption.
		 */
		while (ntimes > 0) {
			PLAN_NTIMES(ntimes);
			mk_copy_act(a[i], &(a[(i+1)]));
			i++;
			atomic_read(time_xattr, &(a[i]->act_time),
				sizeof (int));
			PLAN_READ_ACT(a[i]->act_name, a[i]->act_time,
				a[i]->act_dur);
			if (txr == -1) {
				perror("plan - time_xattr RD");
				exit(0);
			}
			ntimes--;
			goto vm_set;
		}
		close(dyn_xattr);

		PLAN_GOTHERE(a[i]->act_dur);
		i++;
	}
	closedir(acts_dir);
	a_elems = i;
}

/*
 * realloc_acts, given a day-start and day-duration, reallocates all of the
 * previously allocated actions into the new constraints. External variables
 * used are days_fd, a (the array of act_t's), vmday.
 * We return the file descriptor to the date or day file.
 * XXX: add support for chunked durations.
 */
#define	RA_DYN		1
#define	RA_STA		2
#define	RA_ALL		3
#define	RA_ISDYN(x)	(x & 0x0001)
#define	RA_ISSTA(x)	(x & 0x0002)
static ra_err_t *
realloc_acts(day_t day, tm_t *date, size_t base, size_t off)
{
	/*
	 * We have (re)set realloc_err so that we don't bail when bulk
	 * processing the entire week.
	 */
	realloc_err.rae_code = RAE_CODE_SUCCESS;
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}
	int afd = openacts(dfd);

	read_act_dir(afd, base, off, 0);

	/*
	 * We now take all of the data we have about the actions, and try to
	 * reallocate them in the day, within the specified awake times. This
	 * loop runs twice. The first time it allocates all of the activities
	 * that have a starting time (static activities), and then it loops
	 * again to allocate all of the activities that have no starting time,
	 * but have a duration (dynamic activities).
	 * XXX: When reading the acts, we should store them in 2 arrays, so
	 * that we loop over each array once, instead of the same array twice.
	 * This way, we get O(n) performance instead of O(2n).
	 */
	int j = 0;
	int k = 1;
alloc_again:;
	while (j <= (a_elems-1) && a_elems != 0) {
		PLAN_REALLOC_LOOP(a[j]);

		if (!date) {
			a[j]->act_day = day;
		} else {
			a[j]->act_day = -1;
			a[j]->act_date.tm_mon = date->tm_mon;
			a[j]->act_date.tm_mday = date->tm_mday;
			a[j]->act_date.tm_year = date->tm_year;
		}

		if ((a[j]->act_dyn == 0 && !k)) {
			j++;
			continue;
		}

		if ((a[j]->act_dyn == 1 && k)) {
			j++;
			continue;
		}

		/*
		 * When activities are first created, they have a duration of
		 * zero. We can't allocate a zero-sized block of time. How can
		 * you allocate nothing? So we skip activities that don't have
		 * a duration set.
		 */
		if ((a[j]->act_dur == 0)) {
			j++;
			continue;
		}

		PLAN_VMEM_XALLOC(NULL, (a[j]->act_dur), (a[j]->act_vmmin),
			(a[j]->act_vmmax));
		char *r = vmem_xalloc(vmday, (a[j]->act_dur), 0, 0, 0,
				(a[j]->act_vmmin), (a[j]->act_vmmax),
				VM_BESTFIT | VM_NOSLEEP);

		PLAN_VMEM_XALLOC(r, (a[j]->act_dur), (a[j]->act_vmmin),
			(a[j]->act_vmmax));

		if (!r) {
			realloc_err.rae_code = RAE_CODE_ARRANGE;
			realloc_err.rae_act = a[j];
		}
		PLAN_GOTHERE(0);
		a[j]->act_loc = r;
		PLAN_GOTHERE(1);

		/*
		 * And now, we have modify the time and dur members.
		 */
		a[j]->act_time = MEM2TIME(r);
		PLAN_GOTHERE(2);
		j++;
		PLAN_GOTHERE(3);
	}
	if (k) {
		j = 0;
		k--;
		PLAN_GOTHERE(4);
		goto alloc_again;
	}
	PLAN_GOTHERE(5);
	close(dfd);
	PLAN_GOTHERE(6);
	close(afd);
	PLAN_GOTHERE(7);
	return (&realloc_err);
}

/*
 * We commit the information in a[] to disk. We take the filedes of the acts/
 * directory as an argument.
 */
static void
commit_act_arr(int afd)
{
	int j = 0;
	while (j < a_elems) {
		PLAN_COMMIT_ACTS_LOOP(a[j]);
		int time_xattr = a[j]->act_fd_time;
		int dur_xattr = a[j]->act_fd_dur;

		atomic_write(time_xattr, &a[j]->act_time, sizeof (int));

		/*
		 * In some cases (like modifying time xattr), we don't need to
		 * write the dur as it doesn't change during the set_time_act
		 * function.
		 */
		if (write_dur) {
			atomic_write(dur_xattr, &a[j]->act_dur, sizeof (size_t));
		}

		PLAN_COMMIT_ACT(a[j]->act_name, a[j]->act_time, a[j]->act_dur);
		j++;
	}

}

static void
free_todo_arr()
{
	int j = 0;
	while (j < (t_elems) && t_elems != 0) {
		/*
		 * Here we specify the buffer size as the name length + 1 due
		 * to the trailing NULL.
		 */
		umem_free(t[j]->td_name, (t[j]->td_name_len + 1));
		umem_cache_free(todo_cache, t[j]);
		j++;
	}
	if (tsz) {
		umem_free(t, tsz);
		tsz = 0;
	}
	t_elems = 0;
}
/*
 * Here we free all of the memory in a[].
 */
static void
free_act_arr()
{
	int j = 0;
	while (j < (a_elems) && a_elems != 0) {
		if (a[j]->act_loc) {
			vmem_xfree(vmday, a[j]->act_loc, a[j]->act_dur);
		}
		/*
		 * Here we specify the buffer size as the name length + 1 due
		 * to the trailing NULL.
		 */
		if (a[j]->act_name_len != 0) {
			umem_free(a[j]->act_name, (a[j]->act_name_len + 1));
		}
		close(a[j]->act_fd_time);
		close(a[j]->act_fd_dur);
		umem_cache_free(act_cache, a[j]);
		j++;
	}
	a_elems = 0;
}

#define	FIT_ERR "%s: Activity %s can't fit in the alotted time\n"
static void
rae_code_print(ra_err_t *re)
{

	char strbuf[30];
	char *str = NULL;

	if (re->rae_act->act_day == -1) {
		strftime(strbuf, 30, "%Y-%m-%d", &(re->rae_act->act_date));
		str = strbuf;
	} else {
		str = daystr[(re->rae_act->act_day)];
	}

	if (re->rae_code == RAE_CODE_FIT) {
		fprintf(stderr, FIT_ERR, str, re->rae_act->act_name);
	}

	if (re->rae_code == RAE_CODE_ARRANGE) {
		fprintf(stderr, "%s: Couldn't rearrange activity %s\n",
			str, re->rae_act->act_name);
	}
}


/*
 * XXX: We have to check for conflicts because we are growing/shrinking the #
 * of minutes.  If there are dynamic conflicts, we try to rearrange them. If
 * there are static conflicts we report them. If we can't rearrange dynamic
 * actions, we report that to.
 */
int
set_awake(day_t day, tm_t *date, size_t base, size_t off)
{
	ra_err_t *re = realloc_acts(day, date, base, off);
	rae_code_print(re);

	int dfd;

	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	/*
	 * Now that all of our reallocation has worked out, we write these
	 * changes out to the file system, to make them persistent.
	 */
	int awake_xattr = openat(dfd, "awake",
				O_XATTR | O_CREAT | O_RDWR, ALLRWX);

	int afd = openacts(dfd);


	atomic_write(awake_xattr, &base, sizeof (size_t));

	atomic_write(awake_xattr, &off, sizeof (size_t));

	close(awake_xattr);

	commit_act_arr(afd);
	close(afd);
	close(dfd);

	/*
	 * Now we free all of the memory we used (in realloc_acts):
	 * 	act_name member in act_t structures.
	 * 	act_t structures.
	 */
	free_act_arr();
	return (0);
}

/*
 * The set dur function is interesting because, it not only modifies the
 * duration, but can also modify the time, if we have a chunked duration.
 */
int
set_dur(char *n, int day, tm_t *date, size_t dur, size_t chunks)
{
	if (dur > 1440) {
		return (DUR_ELENGTH);
	}

	if (chunks == 0) {
		return (DUR_ECHUNKS);
	}

	PLAN_SET_DUR(n, dur);


	size_t base;
	size_t off;
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	get_awake_range(day, date, &base, &off);

	int adfd = openacts(dfd);

	int afd = openat(adfd, n, O_RDWR);
	if (afd == -1) {
		return (DUR_EEXIST);
	}

	size_t prev_dur = 0;
	size_t prev_chunks = 1;

	/*
	 * We write the new duration to disk and do a reallocation. If the
	 * reallocation succeeds, we commit all of the new data. If it fails,
	 * we don't commit anything, and rollback the duration to the old
	 * value.
	 */
	int dur_xattr = openat(afd, "dur", O_XATTR | O_CREAT | O_RDWR, ALLRWX);
	if (dur_xattr == -1) {
		perror("set_dur_open_dur_xattr");
		exit(0);
	}
	atomic_read(dur_xattr, &prev_dur, sizeof (size_t));

	int time_xattr;
	struct stat time_st;
	int nfill = -1;
	time_xattr = openat(afd, "time",
			O_XATTR | O_CREAT | O_TRUNC | O_RDWR, ALLRWX);
	fstat(time_xattr, &time_st);
	prev_chunks = time_st.st_size/(sizeof (int));

	/*
	 * No need to recalculate a solution that we already have.
	 */
	if (prev_dur == dur && chunks == prev_chunks) {
		return (SUCCESS);
	}

	lseek(dur_xattr, 0, SEEK_SET);

	PLAN_PRECOMMIT_DUR(n, dur);

	atomic_write(dur_xattr, &dur, sizeof (size_t));
	close(dur_xattr);
	int *time_prev_buf;

	/*
	 * We save the data in "time", so that we can restore it in case
	 * allocation fails.
	 */
	time_prev_buf = umem_alloc(time_st.st_size, UMEM_NOFAIL);
	atomic_read(time_xattr, time_prev_buf, time_st.st_size);
	lseek(time_xattr, 0, SEEK_SET);

	/*
	 * If we are setting a chunked duration to a non-chunked duration, we
	 * need to make sure that it's size is adjusted to the size of an int.
	 */
	if (chunks == 1) {
		atomic_write(time_xattr, &nfill, sizeof (int));
		lseek(time_xattr, 0, SEEK_SET);
	}


	/*
	 * XXX: We also need to save the prev time and prev dyn, so that we
	 * could rollback lower down.
	 */
	int dyn_xattr;
	char prev_dyn;
	char yes_dyn = 1;
	int time_fill = -1;
	size_t i = 0;
	/*
	 * If we are setting a chunked duration, we modify the time attr, by
	 * setting it to the size of the chunks * sizeof(int).
	 */
	if (chunks > 1) {
		dyn_xattr = openat(afd, "dyn", O_XATTR | O_CREAT | O_RDWR,
			ALLRWX);

		atomic_read(dyn_xattr, &prev_dyn, sizeof (char));
		lseek(dyn_xattr, 0, SEEK_SET);
		atomic_write(dyn_xattr, &yes_dyn, sizeof (char));
		lseek(dyn_xattr, 0, SEEK_SET);

		while (i < chunks) {
			atomic_write(time_xattr, &time_fill, sizeof (int));
			i++;
		}

		lseek(time_xattr, 0, SEEK_SET);
	}




	ra_err_t *ret = realloc_acts(day, date, base, off);

	/*
	 * If reallocation fails, revert to the old duration, old time, old
	 * dyn, and bail.
	 */
	if (ret->rae_code != RAE_CODE_SUCCESS) {

		/* rollback duration */
		dur_xattr =
			openat(afd, "dur", O_XATTR | O_CREAT | O_RDWR, ALLRWX);
		atomic_write(dur_xattr, &prev_dur, sizeof (size_t));

		/* rollback time */
		atomic_write(time_xattr, &time_prev_buf, time_st.st_size);

		/* rollback dyn */
		atomic_write(dyn_xattr, &prev_dyn, sizeof (char));

		goto skip_commit;
	}

	/* here we write new profiles out to disk */
	commit_act_arr(adfd);

skip_commit:;
	close(dfd);
	close(adfd);
	close(afd);
	close(dur_xattr);
	close(time_xattr);
	close(dyn_xattr);
	rae_code_print(ret);
	free_act_arr();

	return (0);
}


/*
 * The set time functions does two things. It changes an action's type to
 * static and gives an absolute starting time. It can also change an action's
 * type to dynamic. Using this function on an action of chunked duration, will
 * reset the duration to non-chunked. In future, if I deem the feature useful,
 * I'll modify this function to coalesce the durations into a single chunk
 * [(dur=00h30m*2) becomes (dur=01h00m)].
 */
int
set_time_act(char *n, int day, tm_t *date, int time, char dyn)
{
	PLAN_GOTHERE(dyn);
	int dfd;
	int adfd;
	int afd;
	int time_xattr;
	int dyn_xattr;
	int dur_xattr;
	size_t base;
	size_t off;
	size_t dur;
	int old_time = -1;
	ra_err_t *re;

	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	PLAN_GOTHERE(dyn);
	get_awake_range(day, date, &base, &off);

	adfd = openacts(dfd);
	afd = openat(adfd, n, O_RDWR);
	if (afd == -1) {
		return (TIME_EEXIST);
	}
	time_xattr = openat(afd, "time", O_RDWR | O_XATTR | O_CREAT, ALLRWX);
	dur_xattr = openat(afd, "dur", O_RDWR | O_XATTR | O_CREAT, ALLRWX);
	dyn_xattr = openat(afd, "dyn", O_RDWR | O_XATTR | O_CREAT, ALLRWX);
	PLAN_GOTHERE(dyn);


	atomic_read(time_xattr, &old_time, sizeof (int));
	lseek(time_xattr, 0, SEEK_SET);

	atomic_read(dur_xattr, &dur, sizeof (size_t));
	lseek(dur_xattr, 0, SEEK_SET);

	PLAN_GOTHERE(dyn);

	if (dur == 0) {
		return (TIME_ENODUR);
	}

	PLAN_GOTHERE(dyn);

	if ((time + (int)dur) > 1440) {
		return (TIME_ELENGTH);
	}

	PLAN_GOTHERE(dyn);

	atomic_write(dyn_xattr, &dyn, sizeof (char));
	PLAN_GOTHERE(dyn);

	if (!dyn) {
		atomic_write(time_xattr, &time, sizeof (int));
		lseek(time_xattr, 0, SEEK_SET);
	}
	PLAN_GOTHERE(dyn);

	close(time_xattr);
	close(dyn_xattr);
	close(dur_xattr);

	re = realloc_acts(day, date, base, off);

	if (re->rae_code == RAE_CODE_SUCCESS) {
		commit_act_arr(adfd);
	} else {
		rae_code_print(re);
		time_xattr = openat(afd, "time", O_RDWR | O_XATTR | O_CREAT,
			ALLRWX);
		dyn_xattr = openat(afd, "dyn", O_RDWR | O_XATTR | O_CREAT,
			ALLRWX);
		atomic_write(time_xattr, &old_time, sizeof (int));
		close(time_xattr);
	}

	close(dfd);
	close(adfd);
	close(afd);
	free_act_arr();
	return (0);
}

int
set_time_todo(char *n, int day, tm_t *date, uint64_t time)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	int tdfd = opentodos(dfd);
	int tfd = openat(tdfd, n, O_RDWR);
	if (tfd == -1) {
		printf("Can't set time on todo %s. Doesn't exist.\n",
			n);
		exit(0);
	}
	int time_xattr = openat(tfd, "time", O_XATTR | O_CREAT, ALLRWX);
	atomic_write(time_xattr, &time, sizeof (uint64_t));
	close(dfd);
	close(tfd);
	close(time_xattr);
	return (0);
}


int
set_details_act(char *n, int day, tm_t *date, char *det)
{
	int dfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	int adfd = openacts(dfd);
	int afd = openat(adfd, n, O_RDWR);
	size_t strsz = strlen(det);
	/*
	 * Details are stored in the activity.
	 */
	atomic_write(afd, det, strsz);
	close(afd);
	close(adfd);
	close(dfd);
	return (0);
}


int
set_details_todo(char *n, int day, tm_t *date, char *det)
{
	int dfd;
	int tdfd;
	if (date) {
		dfd = opendate(date);
	} else {
		dfd = openday(day);
	}

	tdfd = opentodos(dfd);
	int tfd = openat(tdfd, n, O_RDWR);
	size_t strsz = strlen(det);
	atomic_write(tfd, det, strsz);
	close(tfd);
	close(tdfd);
	close(dfd);
	return (0);
}

/*
 * This function prints a list all of the activities and/or todos in a given
 * day or date, to stdout. It sets the integer pointed to by `no_print`, to 1
 * if anything was printed, or 0 if not.
 */
#define	POST_NL 1
#define NO_NL	0
#define	PRE_NL	-1
void
list(day_t d, tm_t *date, int flag, int nl)
{
	char time_fmt[10];
	char dur_fmt[10];
	char *dyn_fmt;
	char *dyn_true = "true";
	char *dyn_false = "false";
	size_t cur_usage;
	size_t hrs;
	size_t mins;
	int thrs;
	int tmins;
	int act = LS_IS_ACT(flag);
	int todo = LS_IS_TODO(flag);
	int is_prday = LS_IS_PRDAY(flag);
	int is_prboth = LS_IS_PRBOTH(flag);
	int pr_desc = LS_IS_DESC(flag);
	int dfd;
	int have_date;

	if (!date && d <= -1) {
		exit(0);
	}

	if (!date) {
		dfd = openday(d);
	} else {
		have_date = havedate(date);

		if (have_date) {
			dfd = opendate(date);
		} else {
try_day:;
			if (d > -1) {
				dfd = openday(date->tm_wday);
				goto skip_exit;
			}
			exit(0);
skip_exit:;
		}
	}

	int afd;
	int tfd;
	size_t base;
	size_t off;
	if (have_date) {
		get_awake_range(d, date, &base, &off);
	} else {
		get_awake_range(d, 0, &base, &off);
	}

	if (act) {

		int acnt = 0;

		afd = openacts(dfd);

		read_act_dir(afd, base, off, pr_desc);


		/*
		 * Even though we have a date directory, it's empty, so we
		 * might as well close dfd, and open the corresponding day
		 * instead.
		 */
		if (a_elems == 0 && have_date) {
			have_date = 0;
			close(dfd);
			close(afd);
			goto try_day;
		}

		if (a_elems == 0) {
			goto noprint_acts;
		}


		if (!is_prboth && is_prday && date == NULL) {
			printf("%s\n", daystr[d]);
		}

		char str[30];

		if (!is_prboth && is_prday && date) {
			strftime(str, 30, "%Y-%m-%d", date);
			printf("%s\n", str);
		}

		if (is_prboth) {
			strftime(str, 30, "%Y-%m-%d", date);
			printf("%s (%s)\n", daystr[d], str);
		}

		qsort(a, (a_elems), sizeof (act_t *), comp_act_ptrs);

		cur_usage = get_total_usage();

		if (nl == PRE_NL) {
			printf("\n");
		}

		printf("(%d/%d)\n", cur_usage, off);

		printf("%-20s %6s %7s %7s\n",
			"NAME", "DYN", "TIME", "DUR");


		while (acnt < a_elems) {

			if (a[acnt]->act_dyn) {
				dyn_fmt = dyn_true;
			} else {
				dyn_fmt = dyn_false;
			}

			hrs = 0;
			mins = 0;
			thrs = 0;
			tmins = 0;

			int seq_acts = 1;
			int total_dur = 0;
			total_dur += a[acnt]->act_dur;

			while ((acnt+seq_acts) < a_elems &&
				(strcmp((a[acnt]->act_name),
				    (a[(acnt+seq_acts)]->act_name)) == 0)) {

				total_dur += a[(acnt+seq_acts)]->act_dur;
				seq_acts++;
			}

			hrs = (total_dur)/60;
			mins = (total_dur) - (hrs*60);
			sprintf((char *)&dur_fmt, "%.2dh%.2dm", hrs, mins);

			if (a[acnt]->act_time == -1) {
				sprintf((char *)&time_fmt, "N/A");
				goto not_assigned_time;
			}


			thrs = (a[acnt]->act_time)/60;
			tmins = (a[acnt]->act_time) - (thrs*60);
			sprintf((char *)&time_fmt, "%.2d:%.2d", thrs, tmins);

not_assigned_time:;
			printf("%-20s %6s %7s %7s\n",
				a[acnt]->act_name,
				dyn_fmt,
				&(time_fmt[0]),
				&(dur_fmt[0]));

			if (pr_desc && a[acnt]->act_det) {
				printf("  | %s\n", a[acnt]->act_det);
			}

			acnt += seq_acts;
		}

		if (nl == POST_NL) {
			printf("\n");
		}

		free_act_arr();
noprint_acts:;
		close(afd);
	}

	if (todo) {
		tfd = opentodos(dfd);
		read_todo_dir(tfd, pr_desc);
		if (t_elems == 0) {
			goto noprint_todos;
		}


		if (!is_prboth && is_prday && date == NULL) {
			printf("%s\n", daystr[d]);
		}

		char str[30];

		if (!is_prboth && is_prday && date) {
			strftime(str, 30, "%Y-%m-%d", date);
			printf("%s\n", str);
		}

		if (is_prboth) {
			strftime(str, 30, "%Y-%m-%d", date);
			printf("%s (%s)\n", daystr[d], str);
		}

		qsort(t, (t_elems-1), sizeof (todo_t *), comp_todo_ptrs);
		int tcnt = 0;
		if (nl == PRE_NL) {
			printf("\n");
		}

		printf("%-20s %6s \n",
			"NAME", "TIME");
		while (tcnt < t_elems) {
			thrs = 0;
			tmins = 0;
			thrs = (t[tcnt]->td_time)/60;
			tmins = (t[tcnt]->td_time) - (thrs*60);
			sprintf((char *)&time_fmt, "%.2d:%.2d", thrs, tmins);
			printf("%-20s %6s\n",
				t[tcnt]->td_name,
				&(time_fmt[0]));
			if (pr_desc && t[tcnt]->td_det) {
				printf("%s\n", t[tcnt]->td_det);
			}
			tcnt++;
		}

		if (nl == POST_NL) {
			printf("\n");
		}

noprint_todos:;
		close(tfd);
	}
	close(dfd);
}

/*
 * Lot's of redundant code here;
 * TODO: Should roll this into list().
 */
void
list_gen_todo(flag)
{
	char time_fmt[10];
	int tfd = opentodos(-1);
	int det = LS_IS_DESC(flag);
	read_todo_dir(tfd, det);
	if (t_elems == 0) {
		goto noprint_todos;
	}


	qsort(t, (t_elems-1), sizeof (todo_t *), comp_todo_ptrs);
	int tcnt = 0;
	printf("%-20s %6s \n",
		"NAME", "TIME");
	while (tcnt < t_elems) {
		int thrs = 0;
		int tmins = 0;
		thrs = (t[tcnt]->td_time)/60;
		tmins = (t[tcnt]->td_time) - (thrs*60);
		sprintf((char *)&time_fmt, "%.2d:%.2d", thrs, tmins);
		printf("%-20s %6s\n",
			t[tcnt]->td_name,
			&(time_fmt[0]));
		tcnt++;
	}

noprint_todos:;

	close(tfd);
}


void
list_week(int flag, int week_type)
{
	int i = 0;
	tm_t *t = NULL;
	int fl = flag;

	if (week_type != GEN) {
		time_t ct = time(NULL);
		t = localtime(&ct);

		if (t->tm_wday != 0) {
			ct -= (1440*60*t->tm_wday);
		}

		if (week_type == NEXT) {
			ct += (1440*60*7);
		}
		fl = fl ^ 8;
	} else {
		fl = fl ^ 4;
	}

	while (i < 7) {
		if (i != 6) {
			list(i, t, fl, POST_NL);
		} else {
			list(i, t, fl, NO_NL);
		}

		if (LS_IS_ACT(flag)) {
			free_act_arr();
		}

		if (LS_IS_TODO(flag)) {
			free_todo_arr();
		}

		i++;
	}
}

void
list_today(int flag)
{
	time_t cur_time = time(NULL);
	tm_t *t = localtime(&cur_time);
	list(t->tm_wday, NULL, (flag ^ 4), NO_NL);
	list(-1, t, (flag ^ 4), PRE_NL);
}
