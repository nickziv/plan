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

#include <stdint.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <umem.h>
#include <sys/vmem.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <time.h>
#include <sys/time.h>
#include <tzfile.h>
#include "plan_impl.h"
#include "plan_probes.h"

extern char *daystr[];

/*
 * The program name.
 */
static char *pn = NULL;

static int pdb_fd;
int days_fd;
int dates_fd;
int todos_fd;
int write_dur;
static int cur_cmd = 0;
vmem_t *vmday;
umem_cache_t *act_cache;
umem_cache_t *todo_cache;

/*
 * Declarations from plan_manip.c
 */
extern int create_todo(char *, day_t, tm_t *);
extern int create_act(char *, day_t, tm_t *);
extern int destroy_todo(char *, day_t, tm_t *);
extern int destroy_act(char *, day_t, tm_t *);
extern int rename_todo(char *, char *, day_t, tm_t *);
extern int rename_act(char *, char *, day_t, tm_t *);
extern int set_awake(day_t, tm_t *, size_t, size_t);
extern int set_dur(char *, int, tm_t *, size_t, size_t);
extern int set_time_act(char *, int, tm_t *, int, char);
extern int set_details_act(char *, int, tm_t *, char *);
extern int set_details_todo(char *, int, tm_t *, char *);
extern void list(day_t, tm_t *, int, int);
extern void list_week(int, int);
extern void list_today(int);
extern void list_this_week(int);
extern void list_next_week(int);
extern void list_gen_todo(int);


/*
 * Forward declaration.
 */
static void usage(int ix, int usage_bool);
static void print_usage_all(void);

typedef enum plan_help {
	HELP_CREATE,
	HELP_DESTROY,
	HELP_RENAME,
	HELP_DESCRIBE,
	HELP_SET,
	HELP_SET_TIME,
	HELP_SET_DURATION,
	HELP_SET_DETAILS,
	HELP_SET_AWAKE,
	HELP_LIST,
} plan_help_t;

typedef struct plan_cmd {
	const char 	*name;
	int		(*func)(int argc, char **argv);
	plan_help_t	usage;
} plan_cmd_t;

size_t set_cmd_len[] = {5, 8, 4, 7};


static int
act_ctor(void *buf, void *ignored, int flags)
{
	act_t *act = buf;
	bzero(act, sizeof (act_t));
	/* actions are dynamic by default */
	act->act_dyn = 1;
	return (0);
}

static void
act_dtor(void *buf, void *ignored)
{
	act_t *act = buf;
	bzero(act, sizeof (act_t));
	act->act_dyn = 1;
}

static int
todo_ctor(void *buf, void *ignored, int flags)
{
	act_t *act = buf;
	bzero(act, sizeof (todo_t));
	return (0);
}

static void
todo_dtor(void *buf, void *ignored)
{
	act_t *act = buf;
	bzero(act, sizeof (todo_t));
}

/*
 * This function parses time in 24 hour "hh:mm" format, into the integer
 * pointed to by `*time`, which counts the number of minutes since 00:00.
 */
static int
parse_time(char *t, int *time)
{
	/* time is 24hr format hh:mm */
	char *c = t;
	PLAN_GOT_HERE((int)c);
	if (*c > '2') {
		printf("Hours are a 2-digit value\n.");
		printf("The first digit can't be greater than '2'\n");
		exit(0);
	}

	int hrs = 0;
	hrs = (*c - 48) * 10;
	c++;
	hrs += (*c - 48);
	PLAN_GOT_HERE((int)hrs);
	if (hrs > 23) {
		printf("The max hour value is 23. You speicified %d\n",
			hrs);
		exit(0);
	}
	PLAN_GOT_HERE((int)0);

	c++;
	if (*c != ':') {
		printf("Error, missing colon ':'. Format is hh:mm\n");
		exit(0);
	}
	c++;
	int mins = 0;

	mins = (*c - 48) * 10;
	PLAN_GOT_HERE((int)mins);
	c++;

	mins += (*c - 48);
	PLAN_GOT_HERE((int)mins);

	if (mins > 59) {
		printf("The max minute value is 59. You speicified %d\n",
			hrs);
		exit(0);
	}
	PLAN_GOT_HERE((int)mins);
	mins += (hrs * 60);
	PLAN_GOT_HERE((int)mins);
	*time = mins;
	return (0);
}

static char *date_fmt[] = {
	"%y-%m-%d",
	"%Y-%m-%d",
	"%y-%h-%d",
	"%Y-%h-%d"
};

/*
 * This function parses a string that represents a date in YYMMDD format, and
 * stores the result as a struct tm in *tm.
 */
#define _STRPTIME_DONTZERO
static void
parse_date(const char *d, tm_t **tm)
{
	int i = 0;
	char *ret = NULL;

	while (i < (sizeof (date_fmt)/sizeof (char *)) && ret == NULL) {
		ret = strptime(d, date_fmt[i], *tm);
		i++;
	}

	if (!ret) {
		*tm = NULL;
	}

}

/*
 * Here we parse a textual representation of the day into an integer.  The
 * meanings of the integer mappings can be found in the day_t enum in
 * plan_impl.h
 */
static day_t
parse_day(char *d)
{
	tm_t t;
	char *ret = strptime(d, "%a", &t);
	if (ret) {
		return (t.tm_wday);
	}

	return (-1);
}

static void
day_err()
{
	printf("Valid inputs for days:\n");
	printf("\tMonday\n");
	printf("\tTuesday\n");
	printf("\tWednesday\n");
	printf("\tThursday\n");
	printf("\tFriday\n");
	printf("\tSaturday\n");
	printf("\tSunday\n");
}

static void
handle_err(err_t e, char *n, day_t d, tm_t *date)
{

	char path[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	if (e != SUCCESS) {
		if (date) {
			strftime(path, 12, "%Y-%m-%d", date);
			printf("%s: ", path);
		} else {
			if (d >= 0) {
				printf("%s: ", daystr[d]);
			}
		}
	}

	switch (e) {

	case TIME_EEXIST:
		printf("Can't set time on activity %s.", n);
		printf(" Activity doesn't exist.\n");
		break;

	case TIME_ENODUR:
		printf("Can't set time on activity %s.", n);
		printf(" Activity doesn't have duration.\n");
		break;

	case TIME_ELENGTH:
		printf("Can't set time on activity %s.", n);
		printf(" Activity won't fit within the day\n");
		break;

	case TIME_ECHUNK:
		printf("Can't set time on activity %s.", n);
		printf(" Activity has more than one chunk.\n");
		break;

	case DUR_EEXIST:
		printf("Can't set duration on activity %s.", n);
		printf(" Activity doesn't exist.\n");
		break;

	case DUR_ELENGTH:
		printf("Can't set duration on activity %s.", n);
		printf(" Duration exceeds 24 hours\n");
		break;

	case DUR_ECHUNKS:
		printf("Can't set duration on activity %s.", n);
		printf(" Duration has zero (0) chunks.\n");
		break;

	case CREATE_EEXIST:
		printf("Can't create activity %s.", n);
		printf(" Activity already exists.\n");
		break;

	case CREATE_TD_EEXIST:
		printf("Can't create todo %s.", n);
		printf(" Todo already exists.\n");
		break;

	case DESTROY_EEXIST:
		printf("Can't destroy activity %s.", n);
		printf(" Activity doesn't exist.\n");
		break;

	case DESTROY_TD_EEXIST:
		printf("Can't destroy todo %s.", n);
		printf(" Todo doesn't exist.\n");
		break;

	case RN_ENEWEXIST:
		printf("Can't rename activity %s.", n);
		printf(" An activity with the new name already exists.\n");
		break;

	case RN_TD_ENEWEXIST:
		printf("Can't rename activity %s.", n);
		printf(" A todo with the new name already exists.\n");
		break;
	}
}

/*
 * Here we parse the command line and create a new activity or todo.
 */
static int
do_create(int ac, char *av[])
{
	int week = 0;
	int wi = 0;
	tm_t tm;
	tm_t *D = &tm;
	parse_date(av[1], &D);
	day_t d = parse_day(av[1]);
	char *c = NULL;
	c = strchr(av[1], '/');
	if (D == NULL && d == -1 && *av[1] != '*' && *av[1] != '@') {
		printf("\"%s\" is not a valid day or date\n", av[1]);
		exit(0);
	}


	if (c == NULL && *av[1] != '@') {
		printf("A '/' must separate the date or day from the name\n");
		exit(0);
	}

	int cerr;
	if (c == NULL && *av[1] == '@') {
		c = av[1] + 1;
		cerr = create_todo(c, -1, NULL);
		handle_err(cerr, (c+2), -1, NULL);
		return (0);
	}



	if (*(c+1) == '@') {
		if (week) {
			while (wi < 7) {
				cerr = create_todo((c+2), wi, D);
				handle_err(cerr, (c+2), wi, D);
				wi++;
			}
		} else {
			cerr = create_todo((c+2), d, D);
			handle_err(cerr, (c+2), d, D);
		}
	} else {
		if (week) {
			while (wi < 7) {
				cerr = create_act((c+1), wi, D);
				handle_err(cerr, (c+1), wi, D);
				wi++;
			}
		} else {
			cerr = create_act((c+1), d, D);
			handle_err(cerr, (c+1), d, D);
		}
	}


	return (0);
}

/*
 * Here we parse the command line and destroy a new activity or todo.
 */
static int
do_destroy(int ac, char *av[])
{
	int wi = 0;
	int week = 0;
	tm_t t;
	tm_t *D = &t;
	parse_date(av[1], &D);
	day_t d = parse_day(av[1]);
	char *c = NULL;
	c = strchr(av[1], '/');


	if (c == NULL && *av[1] != '@') {
		printf("A '/' must separate the date or day from the name\n");
		exit(0);
	}


	if (D == 0 && d == -1 && !week && *av[1] != '@') {
		printf("\"%s\" is not a valid day or date\n", av[1]);
		exit(0);
	}

	int dret;
	if (c == NULL && *av[1] == '@') {
		c = av[1] + 1;
		dret = destroy_todo(c, -1, NULL);
		handle_err(dret, c, -1, NULL);
		return (0);
	}


	if (*(c+1) == '@') {
		if (week) {
			while (wi < 7) {
				dret = destroy_todo((c+2), wi, D);
				handle_err(dret, (c+2), wi, D);
				wi++;
			}
		} else {
			dret = destroy_todo((c+2), d, D);
			handle_err(dret, (c+2), d, D);
		}
	} else {
		if (week) {
			while (wi < 7) {
				dret = destroy_act((c+1), wi, D);
				handle_err(dret, (c+1), wi, D);
				wi++;
			}
		} else {
			dret = destroy_act((c+1), d, D);
			handle_err(dret, (c+1), d, D);
		}
		destroy_act((c+1), d, D);
	}

	return (1);
}

static int
do_rename(int ac, char *av[])
{
	int at = 0;
	char *new = NULL;
	char *old = NULL;
	if (ac < 3) {
		usage(cur_cmd, 1);
		exit(0);
	}

	old = strchr(av[1], '/');


	new = strchr(av[2], '/');

	if (old == NULL || new == NULL) {
		printf("A '/' must separate the date or day from the name\n");
		exit(0);
	}

	old++;
	new++;

	if ((at = (*old == '@' && *new == '@')) ||
	    (*old != '@' && *new != '@')) {
		goto skip_ampersand_errors;
	}

	if (*old == '@' && *new != '@') {
		printf("Can't rename a todo to an activity.\n");
		exit(0);
	}

	if (*old != '@' && *new == '@') {
		printf("Can't rename an activity to a todo.\n");
		exit(0);
	}


skip_ampersand_errors:;
	tm_t t;
	tm_t tt;
	tm_t *D = &t;
	tm_t *DD = &tt;
	int ret;
	day_t d = parse_day(av[1]);
	day_t dd = parse_day(av[2]);
	parse_date(av[1], &D);
	parse_date(av[2], &DD);
	if ((d != -1 && dd != -1) || (D != 0 && DD != 0)) {
		if ((d == dd) || CMP_DATE(D, DD)) {
			if (!at) {
				ret = rename_act(old, new, d, D);
			} else {
				ret = rename_todo(old, new, d, D);
			}
			handle_err(ret, old, d, D);
			return (0);
		}
	}

	if (D) {
		printf("When renaming, dates must be identical.\n");
	} else {
		printf("When renaming, days must be identical.\n");
	}

	exit(0);

	return (0);
}

static int
do_describe(int ac, char *av[])
{
	if (ac < 2) {
		return (-1);
	}

	tm_t tm;
	tm_t *D = &tm;
	char *d = av[1];
	day_t day;
	char *target = av[1];
	char *desc = NULL;
	if (ac >= 3) {
		desc = av[2];
	}

	if (*target == '@') {
		target++;
		set_details_todo(target, -1, NULL, desc);
		return (0);
	}

	char *slash = strchr(target, '/');

	if (slash == target) {
		printf("Please enter a day or date before the slash\n");
		exit(0);
	}

	if (slash) {
		target = (slash + 1);
		if ((*target) == '\0') {
			printf("Please enter an activity"
				" or todo after the slash\n");
			exit(0);
		}
	}

	parse_date(d, &D);
	day = parse_day(d);

	if (*target == '@') {
		set_details_todo((target + 1), day, D, desc);
	} else {
		set_details_act(target, day, D, desc);
	}

	return (0);
}

/*
 * Here we parse a duration formated as <hrs>h<mins>m
 * We write the result into the variable dur points to.
 * dur's quantum of currency is minutes.
 */
#define	DTSTL\
	"Duration too short to be chunked, and too long to be non-chunked.\n"
static int
parse_dur(char *d, size_t *dur, size_t *chunks)
{

	size_t hrs;
	size_t mins;
	char *c = d;
	size_t slen = strlen(d);
	int chunked = 0;
	if (slen < 6) {
		printf("Duration too short.\n");
		exit(0);
	}

	if (slen > 6 && slen < 7) {
		printf(DTSTL);
		exit(0);
	}

	if (slen >= 8) {
		chunked++;
	}

	if (slen == 6) {
		chunked = 0;
	}


	/*
	 * We can only have 24 hour-long durs
	 */
	if (*(d+2) != 'h' || *(d+5) != 'm') {
		return (-1);
	}

	PLAN_GOT_HERE((int)c);

	hrs = *c - 48;
	hrs *= 10;
	c++;
	PLAN_GOT_HERE((int)c);
	hrs += *c - 48;
	c += 2; /* skip the 'h' */
	mins = *c - 48;
	mins *= 10;
	c++;
	PLAN_GOT_HERE((int)c);
	mins += *c - 48;

	if (mins > 59 || hrs > 24) {
		printf("Duration has more than 24 hrs or more than 59 mins.");
		exit(1);
		return (-1);
	}

	*dur = (hrs * 60) + mins;

	PLAN_PARSE_DUR(*dur);

	if (*dur > 1440) {
		return (-1);
	}

	if (!chunked) {
		return (0);
	}

	c++;	/* Now we're at 'm' */
	PLAN_GOT_HERE((int)c);
	c++;	/* and /now/ we're at '*' */
	PLAN_GOT_HERE((int)c);

	if (*c != '*') {
		printf("Expected '*', found '%c' instead, in \"%s\"\n",
			*c, d);
		exit(0);
	}

	c++;
	PLAN_GOT_HERE((int)c);
	char *invch;
	*chunks = (size_t)strtol(c, &invch, 0);

	if (*invch != '\0') {
		printf("Expected a digit, found '%c' instead in \"%s\"\n",
			(*invch), d);
		exit(0);
	}

	return (0);
}


/*
 * Here we parse the command line and try to modify the time the user wakes up
 * on a particular day, and for how long he is awake.
 */
static int
do_awake(int ac, char *av[])
{
	int week = 0;
	size_t base;
	size_t off;
	int ret;
	tm_t t;
	tm_t *date = &t;
	char *c = av[1]+6;
	char *comma = strchr(c, ',');
	day_t day = -1;

	if (comma == NULL) {
		usage(cur_cmd, 1);
		exit(0);
	}

	if (*av[2] >= 48 && *av[2] <= 57) {
		parse_date(av[2], &date);
	} else {
		day = parse_day(av[2]);
		date = NULL;
	}

	/*
	 * Yes, strictly speaking the 2nd arg to parse_time has the wrong type,
	 * but we're never going to notice the discrepancy in overflow-max,
	 * because we never take an invalid input.
	 */
	ret = parse_time(c, (int*)&base);
	if (ret < 0) {
		return (-1);
	}
	ret = parse_dur((comma+1), &off, NULL);

	int wi = 0;

	ret = set_awake(day, date, base, off);
	/* handle_err(ret, n, day, date); */
	return (ret);

}

/*
 * Here we parse the command line and try to modify the time an activity
 * starts, or the time that a todo is due, on a particular day.
 */
static int
do_time(int ac, char *av[])
{
	int week = 0;
	day_t day = -1;
	tm_t t;
	tm_t *date = &t;
	char *n;
	int time;
	char *at = strchr(av[2], '@');
	n = strchr(av[2], '/');
	n += 1;

	parse_date(av[2], &date);
	day = parse_day(av[2]);

	if (day == -1 && date == 0) {
		day_err();
		exit(0);
	}

	/*
	 * When modifying time, we don't need to write duration data back to
	 * the dur xattr (as the dur doesn't change).
	 */
	write_dur = 0;
	/*
	 * We are auto fitting.
	 */
	int wi = 0;
	int str = 0;
	if (*(av[1]+5) == 'a') {
		if (at) {
			usage(cur_cmd, 1);
		}
		str = set_time_act(n, day, date, 0, 1);
		handle_err(str, n, day, date);
		return (0);
	}


	if (*(av[1]+5) >= 48 && *(av[1]+5) <= 57) {
		int r = parse_time((av[1]+5), &time);
		PLAN_GOT_HERE((int)time);
		if (r == -1) {
			usage(cur_cmd, 1);
		}

		str = set_time_act(n, day, date, time, 0);
		handle_err(str, n, day, date);

		return (0);
	}

	return (0);
}


/*
 * Here we parse the command line and try to modify the duration of an
 * activity.
 */
static int
do_dur(int ac, char *av[])
{
	day_t day = -1;
	tm_t t;
	tm_t *date = &t;
	size_t dur;
	size_t chunks = 1;
	char *slash = strchr(av[2], '/');
	char *d = av[1];
	char *str_date = av[2];

	day = parse_day(av[2]);
	parse_date(str_date, &date);

	if (day == -1 && date == 0) {
		day_err();
		exit(0);
	}

	if (slash == NULL && date) {
		printf("A '/' must separate the date from the activity\n");
	}

	if (slash == NULL && !date) {
		printf("A '/' must separate the day from the activity\n");
	}

	if (*(slash+1) == '@') {
		printf("Only actions can have a duration\n");
		exit(0);
	}

	char *n = (slash+1);

	PLAN_DO_DUR(&dur, dur);

	int r = parse_dur((d+9), &dur, &chunks);

	PLAN_DO_DUR(&dur, dur);

	if (r == -1) {
		printf("Valid duration format\n");
		printf("\t<integer-0..24>h<integer-0..59>m\n");
		exit(0);
	}

	PLAN_DO_DUR(&dur, dur);

	int sdr;
	int wi = 0;

	sdr = set_dur(n, day, date, dur, chunks);
	handle_err(sdr, n, day, date);

	PLAN_DO_DUR(&dur, dur);

	return (sdr);
}

#define	SNCMD	(sizeof (set_cmd_tbl) / sizeof (set_cmd_tbl[0]))

static plan_cmd_t set_cmd_tbl[] = {
	{"awake", do_awake, HELP_SET_AWAKE},
	{NULL, NULL, NULL},
	{"duration", do_dur, HELP_SET_DURATION},
	{NULL, NULL, NULL},
	{"time", do_time, HELP_SET_TIME},
	{NULL, NULL, NULL},
};

static int
do_set(int ac, char *av[])
{
	if (ac < 3) {
		return (-1);
	}

	char *eq = strchr(av[1], '=');

	if (*eq == NULL) {
		printf(
		    "An '=' must separate the property name from the value\n");
		exit(0);
	}

	size_t eqlen = eq - av[1];
	int do_ret = -1;
	int i = 0;
	while (i < SNCMD) {
		if (set_cmd_tbl[i].name == NULL) {
			i++;
			continue;
		}

		if (strncmp(av[1], set_cmd_tbl[i].name, eqlen) == 0) {
			cur_cmd = i;
			do_ret = set_cmd_tbl[i].func(ac, av);
			if (do_ret < 0) {
				return (-1);
			}
			break;
		}


		i++;
	}

	if (do_ret == -1) {
		printf("Can't set an invalid property.\n");
		exit(0);
	}

	return (0);
}

/*
 * We parse the command line and list activities and todos.
 */
static int
do_list(int ac, char *av[])
{
	int flag = 0;
	tm_t t;
	tm_t *date = &t;
	day_t day = -1;
	int cc;
	char *ls_target;
	extern char *optarg;

	while ((cc = getopt(ac, av, ":t:a:d")) != -1) {
		switch (cc) {

		case 't':
			flag = flag ^ 2;

			if (LS_IS_ACT(flag)) {
				usage(cur_cmd, 1);
				exit(0);
			}

			ls_target = optarg;
			break;

		case 'a':
			flag = flag ^ 1;
			if (LS_IS_TODO(flag)) {
				usage(cur_cmd, 1);
				exit(0);
			}
			ls_target = optarg;
			break;

		case 'd':
			flag = flag ^ 16;
			break;

		/* fallthrough */
		case ':':
		case '?':
			usage(cur_cmd, 1);
			exit(0);
			break;
		}
	}

	if (strcmp("today", ls_target) == 0) {
		list_today(flag);
		return (0);
	}

	if (strcmp("general", ls_target) == 0) {
		list_gen_todo(flag);
		return (0);
	}

	if (strcmp("week", ls_target) == 0) {
		list_week(flag, GEN);
		return (0);
	}

	if (strcmp("this_week", ls_target) == 0) {
		list_week(flag, THIS);
		return (0);
	}

	if (strcmp("next_week", ls_target) == 0) {
		list_week(flag, NEXT);
		return (0);
	}

	day = parse_day(ls_target);
	parse_date(ls_target, &date);
	if (day != -1 || date) {
		list(day, date, flag, 0);
		return (SUCCESS);
	}



	printf("Either the day or date has been mis-specified.\n");
	exit(0);
}


static plan_cmd_t cmd_tbl[] = {
	{"create", do_create, HELP_CREATE},
	{NULL, NULL, NULL},
	{"destroy", do_destroy, HELP_DESTROY},
	{NULL, NULL, NULL},
	{"rename", do_rename, HELP_RENAME},
	{NULL, NULL, NULL},
	{"describe", do_describe, HELP_DESCRIBE},
	{NULL, NULL, NULL},
	{"set", do_set, HELP_SET},
	{NULL, NULL, NULL},
	{"list", do_list, HELP_LIST},
	{NULL, NULL, NULL},
};

#define	NCMD	(sizeof (cmd_tbl) / sizeof (cmd_tbl[0]))

#define	LIST_USAGE\
	"\tlist -a | -t today | <day> | <date> | week | this_week | next_week\n"\
	"\tlist -t general\n"

static void
usage(int ix, int usage_bool)
{
	if (usage_bool) {
		printf("usage:\n");
	}

	if (cmd_tbl[ix].name == NULL) {
		printf("\n");
		return;
	}

	int cmd = cmd_tbl[ix].usage;

	switch (cmd) {

	case HELP_CREATE:
		printf("\tcreate <day|date>/<activity>\n");
		printf("\tcreate <day|date>/@<todo>\n");
		break;

	case HELP_DESTROY:
		printf("\tdestroy <day|date>/<activity>\n");
		printf("\tdestroy <day|date>/@<todo>\n");
		break;

	case HELP_RENAME:
		printf(
		    "\trename <day|date>/<activity> <day|date>/<activity>\n");
		printf("\trename <day|date>/@<todo> <day|date>/@<todo>\n");
		break;

	case HELP_DESCRIBE:
		printf("\tdescribe <day|date>/<activity> \"<description>\"\n");
		printf("\tdescribe <day|date>/@<todo> \"<description>\"\n");
		break;

	case HELP_SET:
		printf("\tset awake=<24-hr-time>,<hrs>h<mins>m <day|date>\n");
		printf("\tset duration=<hrs>h<mins>m <day|date>/<activity>\n");
		printf("\tset time=autofit <day|date>/<activity>\n");
		printf("\tset time=<24-hr-time> <day|date>/<activity>\n");
		printf("\tset time=<24-hr-time> <day|date>/@<todo>\n");
		break;

	case HELP_LIST:
		printf(LIST_USAGE);
		break;

	}


}

static void
print_usage_all()
{
	int i = 0;
	printf("usage: %s command args ...\n", pn);
	printf("where 'command args' is one of the following:\n\n");
	while (i < NCMD) {
		usage(i, 0);
		i++;
	}
}

static int
my_umem_retry()
{
	return (UMEM_CALLBACK_RETRY);
}

#define	ALLRWX (S_IRWXU | S_IRWXG | S_IRWXO)
int
main(int ac, char *av[])
{
	/*
	 * This sleep is here, to allow devs enough time to grab the pid for
	 * use with dtrace. Using the dtrace -c 'plan ...' command results in
	 * running the command as root, which creates a '/root/.plandb'
	 * directory, instead of using the user's directory.
	 */
	if (0) {
		sleep(7);
	}

	pn = basename(av[0]);

	if (ac < 3) {
		print_usage_all();
		return (0);
	}

	/*
	 * We use umem caches because, in addition to being fast, they allow us
	 * to construct objects upon allocation (less house keeping involved).
	 */
	act_cache = umem_cache_create("act_cache",
			sizeof (act_t),
			0,
			act_ctor,
			act_dtor,
			NULL,
			NULL,
			NULL,
			0);

	todo_cache = umem_cache_create("todo_cache",
			sizeof (todo_t),
			0,
			todo_ctor,
			todo_dtor,
			NULL,
			NULL,
			NULL,
			0);

	umem_nofail_callback(my_umem_retry);

	/*
	 * By default, we update all xattr's.
	 */
	write_dur = 1;

	/*
	 * We always place .plandb in the current user's home directory. We
	 * find the home directory, concatenate the two paths, and open the
	 * .plandb directory. We created any needed sub-directories along the
	 * way. The reason the mkdirats are not in conditional, is because this
	 * way, we can peruse an existing .plandb directory, that might not
	 * have any subdirectories. This allows the user to create ~/.plandb
	 * before using `plan`. This way they could, for example dedicate a ZFS
	 * datasetfor .plandb if they should desire this.
	 */
	uid_t uid = getuid();
	struct passwd *pwd = getpwuid(uid);
	char *home = pwd->pw_dir;
	size_t hl = strlen(home);
	size_t pdbl = hl+9;
	/* the db root */
	char *pdb_path = umem_alloc(pdbl, UMEM_NOFAIL);
	strcpy(pdb_path, home);
	strcat(pdb_path, "/.plandb");
	mkdir(pdb_path, ALLRWX);
	DIR *pdb_dir = opendir(pdb_path);
	pdb_fd = dirfd(pdb_dir);
	mkdirat(pdb_fd, "days", ALLRWX);
	mkdirat(pdb_fd, "dates", ALLRWX);
	mkdirat(pdb_fd, "todos", ALLRWX);
	days_fd = openat(pdb_fd, "days", O_RDONLY);
	if (days_fd == -1) {
		perror("days_fd");
		exit(0);
	}

	/*
	 * These are opened, so that we can use openat() in other parts of the
	 * code, making for far, far less strcat's. openat is my new favourite
	 * system call.
	 */
	dates_fd = openat(pdb_fd, "dates", O_RDONLY);
	todos_fd = openat(pdb_fd, "todos", O_RDONLY);


	/*
	 * While vmem is a rather sexy resource allocator, a horrible, horrible
	 * flaw in the 2001 umem/vmem paper is that the authors neglected to
	 * mention that we can't allocate integer resources that have an
	 * initial span starting at 0 AND a non-zero size (but an initial span
	 * with size 0 _can_ start at 0). This situation is further exacerbated
	 * by the psuedo-code examples that show the creation of hypothetical
	 * arenas that do indeed have a span based at zero, with a non-zero
	 * size.  This comment is here so that the next poor soul who looks at
	 * this code doesn't interpret the `(void*)1` as a careless error on my
	 * part. Trying to "fix" it with a `(void*)0` will result in a
	 * umem_panic (which raises a SIGABRT), which will in turn result in
	 * threats of violence directed at the dev machine, raised
	 * blood-pressure, lowered life-expectancy, significantly less hair,
	 * and possibly a restraining order from your room mate directed at you
	 * (or vice-versa). You're welcome.
	 */
	vmday = vmem_create(
			"vmday",
			(void*)1,
			1440,
			1,
			NULL,
			NULL,
			NULL,
			1,
			VM_NOSLEEP);

	/*
	 * Used while debugging, may be useful in the future.
	 */
	PLAN_VMEM_CREATE(vmday);

	int do_ret = -1;
	int i = 0;
	while (i < NCMD) {
		if (cmd_tbl[i].name == NULL) {
			i++;
			continue;
		}

		if (strcmp(av[1], cmd_tbl[i].name) == 0) {
			cur_cmd = i;
			do_ret = cmd_tbl[i].func((ac-1), (av+1));
			if (do_ret < 0) {
				usage(i, 1);
			}
			break;
		}
		i++;
	}

	if (do_ret < 0) {
		//print_usage_all();
	}

	umem_free(pdb_path, pdbl);
	return (0);
}
