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

#include <sys/avl.h>

#define	CMP_DATE(x, y)\
	(x->tm_year == y->tm_year &&\
	x->tm_mon == y->tm_mon &&\
	x->tm_mday == y->tm_mday)

#define	LS_IS_ACT(f)	(f & 1)
#define	LS_IS_TODO(f)	(f & 2)
#define	LS_IS_PRDAY(f)	(f & 4)
#define	LS_IS_PRBOTH(f)	(f & 8)
#define LS_IS_DESC(f)	(f & 16)

typedef enum day {
	SUN,
	MON,
	TUES,
	WED,
	THUR,
	FRI,
	SAT,
} day_t;

typedef struct tm tm_t;

typedef enum err {
	SUCCESS,
	DUR_EEXIST,
	DUR_ELENGTH,
	DUR_ECHUNKS,
	TIME_EEXIST,
	TIME_ENODUR,
	TIME_ELENGTH,
	TIME_TD,
	RA_EFIT,
	RA_EARRA,
	/* AWAKE_, */
	CREATE_EEXIST,
	CREATE_TD_EEXIST,
	DESTROY_EEXIST,
	DESTROY_TD_EEXIST,
	RN_ENEWEXIST,
	RN_TD_ENEWEXIST,
} err_t;

typedef struct todo {
	size_t		td_name_len;
	char		*td_name;
	int		td_time;
	char		*td_det;
	struct todo	*td_next;
} todo_t;


typedef struct act {
	size_t		act_name_len;
	char		act_dyn;
	day_t		act_day;
	tm_t		act_date;
	char		*act_name;
	int		act_time;
	size_t		act_dur;
	char		*act_det;
	char		*act_vmmin;
	char		*act_vmmax;
	char		*act_loc;
	int		act_fd_dur;
	int		act_fd_time;
} act_t;

#define	RAE_CODE_SUCCESS	0
#define	RAE_CODE_FIT		1
#define	RAE_CODE_ARRANGE 	2
typedef struct ra_err {
	int		rae_code;
	act_t		*rae_act;
} ra_err_t;
