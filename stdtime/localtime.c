/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Most of this code is in the public domain, so clarified as of
 * June 5, 1996 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * All modifications to this code to abstract timezones away from
 * the environment are by Tilghman Lesher, <tlesher@vcch.com>, with
 * the copyright assigned to Digium.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*
 *
 * Multi-timezone Localtime code
 * 
 */

/*
 * Asterisk defines
 *
 * Don't mess with these unless you're really sure you know what you're doing.
 */
#ifndef _THREAD_SAFE
#define _THREAD_SAFE
#endif
#define TZ_STRLEN_MAX	255
/* #define DEBUG */
#include "asterisk/lock.h"
#include "asterisk/localtime.h"


#ifndef lint
#ifndef NOID
static const char	elsieid[] = "@(#)localtime.c	7.57";
#endif /* !defined NOID */
#endif /* !defined lint */

/*
** Leap second handling from Bradley White (bww@k.gp.cs.cmu.edu).
** POSIX-style TZ environment variable handling from Guy Harris
** (guy@auspex.com).
*/

/*LINTLIBRARY*/

#include <sys/types.h>
#include <sys/stat.h>
#include "private.h"
#include "tzfile.h"
#include <fcntl.h>
#ifdef DEBUG
#include <stdio.h>
#endif

/*
** SunOS 4.1.1 headers lack O_BINARY.
*/

#ifdef O_BINARY
#define OPEN_MODE	(O_RDONLY | O_BINARY)
#endif /* defined O_BINARY */
#ifndef O_BINARY
#define OPEN_MODE	O_RDONLY
#endif /* !defined O_BINARY */

#ifndef WILDABBR
/*
** Someone might make incorrect use of a time zone abbreviation:
**	1.	They might reference tzname[0] before calling ast_tzset (explicitly
**		or implicitly).
**	2.	They might reference tzname[1] before calling ast_tzset (explicitly
**		or implicitly).
**	3.	They might reference tzname[1] after setting to a time zone
**		in which Daylight Saving Time is never observed.
**	4.	They might reference tzname[0] after setting to a time zone
**		in which Standard Time is never observed.
**	5.	They might reference tm.TM_ZONE after calling offtime.
** What's best to do in the above cases is open to debate;
** for now, we just set things up so that in any of the five cases
** WILDABBR is used.  Another possibility:  initialize tzname[0] to the
** string "tzname[0] used before set", and similarly for the other cases.
** And another:  initialize tzname[0] to "ERA", with an explanation in the
** manual page of what this "time zone abbreviation" means (doing this so
** that tzname[0] has the "normal" length of three characters).
*/
#define WILDABBR	"   "
#endif /* !defined WILDABBR */

static char		wildabbr[] = "WILDABBR";

/* FreeBSD defines 'zone' in 'struct tm' as non-const, so don't declare this
   string as const. */
static char		gmt[] = "GMT";

struct ttinfo {				/* time type information */
	long		tt_gmtoff;	/* GMT offset in seconds */
	int		tt_isdst;	/* used to set tm_isdst */
	int		tt_abbrind;	/* abbreviation list index */
	int		tt_ttisstd;	/* TRUE if transition is std time */
	int		tt_ttisgmt;	/* TRUE if transition is GMT */
};

struct lsinfo {				/* leap second information */
	time_t		ls_trans;	/* transition time */
	long		ls_corr;	/* correction to apply */
};

#define BIGGEST(a, b)	(((a) > (b)) ? (a) : (b))

#ifdef TZNAME_MAX
#define MY_TZNAME_MAX	TZNAME_MAX
#endif /* defined TZNAME_MAX */
#ifndef TZNAME_MAX
#define MY_TZNAME_MAX	255
#endif /* !defined TZNAME_MAX */

#ifdef SOLARIS
#undef TM_ZONE
#undef TM_GMTOFF 
#endif


struct state {
	char	name[TZ_STRLEN_MAX + 1];
	int		leapcnt;
	int		timecnt;
	int		typecnt;
	int		charcnt;
	time_t		ats[TZ_MAX_TIMES];
	unsigned char	types[TZ_MAX_TIMES];
	struct ttinfo	ttis[TZ_MAX_TYPES];
	char		chars[BIGGEST(BIGGEST(TZ_MAX_CHARS + 1, sizeof gmt),
				(2 * (MY_TZNAME_MAX + 1)))];
	struct lsinfo	lsis[TZ_MAX_LEAPS];
	struct state	*next;
};

struct rule {
	int		r_type;		/* type of rule--see below */
	int		r_day;		/* day number of rule */
	int		r_week;		/* week number of rule */
	int		r_mon;		/* month number of rule */
	long		r_time;		/* transition time of rule */
};

#define JULIAN_DAY		0	/* Jn - Julian day */
#define DAY_OF_YEAR		1	/* n - day of year */
#define MONTH_NTH_DAY_OF_WEEK	2	/* Mm.n.d - month, week, day of week */

/*
** Prototypes for static functions.
*/

static long		detzcode P((const char * codep));
static const char *	getnum P((const char * strp, int * nump, int min,
				int max));
static const char *	getsecs P((const char * strp, long * secsp));
static const char *	getoffset P((const char * strp, long * offsetp));
static const char *	getrule P((const char * strp, struct rule * rulep));
static void		gmtload P((struct state * sp));
static void		gmtsub P((const time_t * timep, long offset,
				struct tm * tmp, const char * zone));
static void		localsub P((const time_t * timep, long offset,
				struct tm * tmp, const char * zone));
static int		increment_overflow P((int * number, int delta));
static int		normalize_overflow P((int * tensptr, int * unitsptr,
				int base));
static time_t		time1 P((struct tm * tmp,
				void(*funcp) P((const time_t *,
				long, struct tm *, const char*)),
				long offset, const char * zone));
static time_t		time2 P((struct tm *tmp,
				void(*funcp) P((const time_t *,
				long, struct tm*, const char*)),
				long offset, int * okayp, const char * zone));
static void		timesub P((const time_t * timep, long offset,
				const struct state * sp, struct tm * tmp));
static int		tmcomp P((const struct tm * atmp,
				const struct tm * btmp));
static time_t		transtime P((time_t janfirst, int year,
				const struct rule * rulep, long offset));
static int		tzload P((const char * name, struct state * sp));
static int		tzparse P((const char * name, struct state * sp,
				int lastditch));

static struct state *	lclptr      = NULL;
static struct state *	last_lclptr = NULL;
static struct state *	gmtptr      = NULL;

#ifndef TZ_STRLEN_MAX
#define TZ_STRLEN_MAX 255
#endif /* !defined TZ_STRLEN_MAX */

static int		gmt_is_set;
#ifdef	_THREAD_SAFE
AST_MUTEX_DEFINE_STATIC(lcl_mutex);
AST_MUTEX_DEFINE_STATIC(tzset_mutex);
AST_MUTEX_DEFINE_STATIC(tzsetwall_mutex);
AST_MUTEX_DEFINE_STATIC(gmt_mutex);
#endif

/*
** Section 4.12.3 of X3.159-1989 requires that
**	Except for the strftime function, these functions [asctime,
**	ctime, gmtime, localtime] return values in one of two static
**	objects: a broken-down time structure and an array of char.
** Thanks to Paul Eggert (eggert@twinsun.com) for noting this.
*/

static long
detzcode(codep)
const char * const	codep;
{
	register long	result;
	register int	i;

	result = (codep[0] & 0x80) ? ~0L : 0L;
	for (i = 0; i < 4; ++i)
		result = (result << 8) | (codep[i] & 0xff);
	return result;
}

static int
tzload(name, sp)
register const char *		name;
register struct state * const	sp;
{
	register const char *	p;
	register int		i;
	register int		fid;

#ifdef DEBUG
	fprintf(stderr,"tzload called with name=%s, sp=%d\n", name, sp);
#endif
	if (name == NULL && (name = TZDEFAULT) == NULL)
		return -1;
	{
		register int	doaccess;
		struct stat	stab;
		/*
		** Section 4.9.1 of the C standard says that
		** "FILENAME_MAX expands to an integral constant expression
		** that is the size needed for an array of char large enough
		** to hold the longest file name string that the implementation
		** guarantees can be opened."
		*/
		char		fullname[FILENAME_MAX + 1] = "";

		if (name[0] == ':')
			++name;
		doaccess = name[0] == '/';
		if (!doaccess) {
			if ((p = TZDIR) == NULL)
				return -1;
			if ((strlen(p) + 1 + strlen(name) + 1) >= sizeof fullname)
				return -1;
			(void) strncpy(fullname, p, sizeof(fullname) - 1);
			(void) strncat(fullname, "/", sizeof(fullname) - strlen(fullname) - 1);
			(void) strncat(fullname, name, sizeof(fullname) - strlen(fullname) - 1);
			/*
			** Set doaccess if '.' (as in "../") shows up in name.
			*/
			if (strchr(name, '.') != NULL)
				doaccess = TRUE;
			name = fullname;
		}
		if (doaccess && access(name, R_OK) != 0)
		     	return -1;
		if ((fid = open(name, OPEN_MODE)) == -1)
			return -1;
		if ((fstat(fid, &stab) < 0) || !S_ISREG(stab.st_mode)) {
			close(fid);
			return -1;
		}
	}
	{
		struct tzhead *	tzhp;
		char		buf[sizeof *sp + sizeof *tzhp];
		int		ttisstdcnt;
		int		ttisgmtcnt;

		i = read(fid, buf, sizeof buf);
		if (close(fid) != 0)
			return -1;
		p = buf;
		p += (sizeof tzhp->tzh_magic) + (sizeof tzhp->tzh_reserved);
		ttisstdcnt = (int) detzcode(p);
		p += 4;
		ttisgmtcnt = (int) detzcode(p);
		p += 4;
		sp->leapcnt = (int) detzcode(p);
		p += 4;
		sp->timecnt = (int) detzcode(p);
		p += 4;
		sp->typecnt = (int) detzcode(p);
		p += 4;
		sp->charcnt = (int) detzcode(p);
		p += 4;
		if (sp->leapcnt < 0 || sp->leapcnt > TZ_MAX_LEAPS ||
			sp->typecnt <= 0 || sp->typecnt > TZ_MAX_TYPES ||
			sp->timecnt < 0 || sp->timecnt > TZ_MAX_TIMES ||
			sp->charcnt < 0 || sp->charcnt > TZ_MAX_CHARS ||
			(ttisstdcnt != sp->typecnt && ttisstdcnt != 0) ||
			(ttisgmtcnt != sp->typecnt && ttisgmtcnt != 0))
				return -1;
		if (i - (p - buf) < sp->timecnt * 4 +	/* ats */
			sp->timecnt +			/* types */
			sp->typecnt * (4 + 2) +		/* ttinfos */
			sp->charcnt +			/* chars */
			sp->leapcnt * (4 + 4) +		/* lsinfos */
			ttisstdcnt +			/* ttisstds */
			ttisgmtcnt)			/* ttisgmts */
				return -1;
		for (i = 0; i < sp->timecnt; ++i) {
			sp->ats[i] = detzcode(p);
			p += 4;
		}
		for (i = 0; i < sp->timecnt; ++i) {
			sp->types[i] = (unsigned char) *p++;
			if (sp->types[i] >= sp->typecnt)
				return -1;
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			ttisp->tt_gmtoff = detzcode(p);
			p += 4;
			ttisp->tt_isdst = (unsigned char) *p++;
			if (ttisp->tt_isdst != 0 && ttisp->tt_isdst != 1)
				return -1;
			ttisp->tt_abbrind = (unsigned char) *p++;
			if (ttisp->tt_abbrind < 0 ||
				ttisp->tt_abbrind > sp->charcnt)
					return -1;
		}
		for (i = 0; i < sp->charcnt; ++i)
			sp->chars[i] = *p++;
		sp->chars[i] = '\0';	/* ensure '\0' at end */
		for (i = 0; i < sp->leapcnt; ++i) {
			register struct lsinfo *	lsisp;

			lsisp = &sp->lsis[i];
			lsisp->ls_trans = detzcode(p);
			p += 4;
			lsisp->ls_corr = detzcode(p);
			p += 4;
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			if (ttisstdcnt == 0)
				ttisp->tt_ttisstd = FALSE;
			else {
				ttisp->tt_ttisstd = *p++;
				if (ttisp->tt_ttisstd != TRUE &&
					ttisp->tt_ttisstd != FALSE)
						return -1;
			}
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			if (ttisgmtcnt == 0)
				ttisp->tt_ttisgmt = FALSE;
			else {
				ttisp->tt_ttisgmt = *p++;
				if (ttisp->tt_ttisgmt != TRUE &&
					ttisp->tt_ttisgmt != FALSE)
						return -1;
			}
		}
	}
	return 0;
}

static const int	mon_lengths[2][MONSPERYEAR] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static const int	year_lengths[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

/*
** Given a pointer into a time zone string, extract a number from that string.
** Check that the number is within a specified range; if it is not, return
** NULL.
** Otherwise, return a pointer to the first character not part of the number.
*/

static const char *
getnum(strp, nump, min, max)
register const char *	strp;
int * const		nump;
const int		min;
const int		max;
{
	register char	c;
	register int	num;

	if (strp == NULL || !is_digit(c = *strp))
		return NULL;
	num = 0;
	do {
		num = num * 10 + (c - '0');
		if (num > max)
			return NULL;	/* illegal value */
		c = *++strp;
	} while (is_digit(c));
	if (num < min)
		return NULL;		/* illegal value */
	*nump = num;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a number of seconds,
** in hh[:mm[:ss]] form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the number
** of seconds.
*/

static const char *
getsecs(strp, secsp)
register const char *	strp;
long * const		secsp;
{
	int	num;

	/*
	** `HOURSPERDAY * DAYSPERWEEK - 1' allows quasi-Posix rules like
	** "M10.4.6/26", which does not conform to Posix,
	** but which specifies the equivalent of
	** ``02:00 on the first Sunday on or after 23 Oct''.
	*/
	strp = getnum(strp, &num, 0, HOURSPERDAY * DAYSPERWEEK - 1);
	if (strp == NULL)
		return NULL;
	*secsp = num * (long) SECSPERHOUR;
	if (*strp == ':') {
		++strp;
		strp = getnum(strp, &num, 0, MINSPERHOUR - 1);
		if (strp == NULL)
			return NULL;
		*secsp += num * SECSPERMIN;
		if (*strp == ':') {
			++strp;
			/* `SECSPERMIN' allows for leap seconds.  */
			strp = getnum(strp, &num, 0, SECSPERMIN);
			if (strp == NULL)
				return NULL;
			*secsp += num;
		}
	}
	return strp;
}

/*
** Given a pointer into a time zone string, extract an offset, in
** [+-]hh[:mm[:ss]] form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the time.
*/

static const char *
getoffset(strp, offsetp)
register const char *	strp;
long * const		offsetp;
{
	register int	neg = 0;

	if (*strp == '-') {
		neg = 1;
		++strp;
	} else if (*strp == '+')
		++strp;
	strp = getsecs(strp, offsetp);
	if (strp == NULL)
		return NULL;		/* illegal time */
	if (neg)
		*offsetp = -*offsetp;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a rule in the form
** date[/time].  See POSIX section 8 for the format of "date" and "time".
** If a valid rule is not found, return NULL.
** Otherwise, return a pointer to the first character not part of the rule.
*/

static const char *
getrule(strp, rulep)
const char *			strp;
register struct rule * const	rulep;
{
	if (*strp == 'J') {
		/*
		** Julian day.
		*/
		rulep->r_type = JULIAN_DAY;
		++strp;
		strp = getnum(strp, &rulep->r_day, 1, DAYSPERNYEAR);
	} else if (*strp == 'M') {
		/*
		** Month, week, day.
		*/
		rulep->r_type = MONTH_NTH_DAY_OF_WEEK;
		++strp;
		strp = getnum(strp, &rulep->r_mon, 1, MONSPERYEAR);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_week, 1, 5);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERWEEK - 1);
	} else if (is_digit(*strp)) {
		/*
		** Day of year.
		*/
		rulep->r_type = DAY_OF_YEAR;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERLYEAR - 1);
	} else	return NULL;		/* invalid format */
	if (strp == NULL)
		return NULL;
	if (*strp == '/') {
		/*
		** Time specified.
		*/
		++strp;
		strp = getsecs(strp, &rulep->r_time);
	} else	rulep->r_time = 2 * SECSPERHOUR;	/* default = 2:00:00 */
	return strp;
}

/*
** Given the Epoch-relative time of January 1, 00:00:00 GMT, in a year, the
** year, a rule, and the offset from GMT at the time that rule takes effect,
** calculate the Epoch-relative time that rule takes effect.
*/

static time_t
transtime(janfirst, year, rulep, offset)
const time_t				janfirst;
const int				year;
register const struct rule * const	rulep;
const long				offset;
{
	register int	leapyear;
	register time_t	value = 0;
	register int	i;
	int		d, m1, yy0, yy1, yy2, dow;

	leapyear = isleap(year);
	switch (rulep->r_type) {

	case JULIAN_DAY:
		/*
		** Jn - Julian day, 1 == January 1, 60 == March 1 even in leap
		** years.
		** In non-leap years, or if the day number is 59 or less, just
		** add SECSPERDAY times the day number-1 to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + (rulep->r_day - 1) * SECSPERDAY;
		if (leapyear && rulep->r_day >= 60)
			value += SECSPERDAY;
		break;

	case DAY_OF_YEAR:
		/*
		** n - day of year.
		** Just add SECSPERDAY times the day number to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + rulep->r_day * SECSPERDAY;
		break;

	case MONTH_NTH_DAY_OF_WEEK:
		/*
		** Mm.n.d - nth "dth day" of month m.
		*/
		value = janfirst;
		for (i = 0; i < rulep->r_mon - 1; ++i)
			value += mon_lengths[leapyear][i] * SECSPERDAY;

		/*
		** Use Zeller's Congruence to get day-of-week of first day of
		** month.
		*/
		m1 = (rulep->r_mon + 9) % 12 + 1;
		yy0 = (rulep->r_mon <= 2) ? (year - 1) : year;
		yy1 = yy0 / 100;
		yy2 = yy0 % 100;
		dow = ((26 * m1 - 2) / 10 +
			1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
		if (dow < 0)
			dow += DAYSPERWEEK;

		/*
		** "dow" is the day-of-week of the first day of the month.  Get
		** the day-of-month (zero-origin) of the first "dow" day of the
		** month.
		*/
		d = rulep->r_day - dow;
		if (d < 0)
			d += DAYSPERWEEK;
		for (i = 1; i < rulep->r_week; ++i) {
			if (d + DAYSPERWEEK >=
				mon_lengths[leapyear][rulep->r_mon - 1])
					break;
			d += DAYSPERWEEK;
		}

		/*
		** "d" is the day-of-month (zero-origin) of the day we want.
		*/
		value += d * SECSPERDAY;
		break;
	}

	/*
	** "value" is the Epoch-relative time of 00:00:00 GMT on the day in
	** question.  To get the Epoch-relative time of the specified local
	** time on that day, add the transition time and the current offset
	** from GMT.
	*/
	return value + rulep->r_time + offset;
}

/*
** Given a POSIX section 8-style TZ string, fill in the rule tables as
** appropriate.
*/

static int
tzparse(name, sp, lastditch)
const char *			name;
register struct state * const	sp;
const int			lastditch;
{
	const char *			stdname;
	const char *			dstname = NULL;
	size_t				stdlen = 0;
	size_t				dstlen = 0;
	long				stdoffset = 0L;
	long				dstoffset = 0L;
	register time_t *		atp;
	register unsigned char *	typep;
	register char *			cp;
	register int			load_result;

	stdname = name;
#ifdef DEBUG
	fprintf(stderr, "tzparse(): loading default rules\n");
#endif
	load_result = tzload(TZDEFRULES, sp);
	if (load_result != 0)
		sp->leapcnt = 0;		/* so, we're off a little */
	if (*name != '\0') {
		if (*name != '\0' && *name != ',' && *name != ';') {
			name = getoffset(name, &dstoffset);
			if (name == NULL)
				return -1;
		} else	dstoffset = stdoffset - SECSPERHOUR;
		if (*name == ',' || *name == ';') {
			struct rule	start;
			struct rule	end;
			register int	year;
			register time_t	janfirst;
			time_t		starttime;
			time_t		endtime;

			++name;
			if ((name = getrule(name, &start)) == NULL)
				return -1;
			if (*name++ != ',')
				return -1;
			if ((name = getrule(name, &end)) == NULL)
				return -1;
			if (*name != '\0')
				return -1;
			sp->typecnt = 2;	/* standard time and DST */
			/*
			** Two transitions per year, from EPOCH_YEAR to 2037.
			*/
			sp->timecnt = 2 * (2037 - EPOCH_YEAR + 1);
			if (sp->timecnt > TZ_MAX_TIMES)
				return -1;
			sp->ttis[0].tt_gmtoff = -dstoffset;
			sp->ttis[0].tt_isdst = 1;
			sp->ttis[0].tt_abbrind = stdlen + 1;
			sp->ttis[1].tt_gmtoff = -stdoffset;
			sp->ttis[1].tt_isdst = 0;
			sp->ttis[1].tt_abbrind = 0;
			atp = sp->ats;
			typep = sp->types;
			janfirst = 0;
			for (year = EPOCH_YEAR; year <= 2037; ++year) {
				starttime = transtime(janfirst, year, &start,
					stdoffset);
				endtime = transtime(janfirst, year, &end,
					dstoffset);
				if (starttime > endtime) {
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
				} else {
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
				}
				janfirst += year_lengths[isleap(year)] *
					SECSPERDAY;
			}
		} else {
			register long	theirstdoffset;
			register long	theirdstoffset;
			register long	theiroffset;
			register int	isdst;
			register int	i;
			register int	j;

			if (*name != '\0')
				return -1;
			if (load_result != 0)
				return -1;
			/*
			** Initial values of theirstdoffset and theirdstoffset.
			*/
			theirstdoffset = 0;
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				if (!sp->ttis[j].tt_isdst) {
					theirstdoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}
			theirdstoffset = 0;
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				if (sp->ttis[j].tt_isdst) {
					theirdstoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}
			/*
			** Initially we're assumed to be in standard time.
			*/
			isdst = FALSE;
			theiroffset = theirstdoffset;
			/*
			** Now juggle transition times and types
			** tracking offsets as you do.
			*/
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				sp->types[i] = sp->ttis[j].tt_isdst;
				if (sp->ttis[j].tt_ttisgmt) {
					/* No adjustment to transition time */
				} else {
					/*
					** If summer time is in effect, and the
					** transition time was not specified as
					** standard time, add the summer time
					** offset to the transition time;
					** otherwise, add the standard time
					** offset to the transition time.
					*/
					/*
					** Transitions from DST to DDST
					** will effectively disappear since
					** POSIX provides for only one DST
					** offset.
					*/
					if (isdst && !sp->ttis[j].tt_ttisstd) {
						sp->ats[i] += dstoffset -
							theirdstoffset;
					} else {
						sp->ats[i] += stdoffset -
							theirstdoffset;
					}
				}
				theiroffset = -sp->ttis[j].tt_gmtoff;
				if (sp->ttis[j].tt_isdst)
					theirdstoffset = theiroffset;
				else	theirstdoffset = theiroffset;
			}
			/*
			** Finally, fill in ttis.
			** ttisstd and ttisgmt need not be handled.
			*/
			sp->ttis[0].tt_gmtoff = -stdoffset;
			sp->ttis[0].tt_isdst = FALSE;
			sp->ttis[0].tt_abbrind = 0;
			sp->ttis[1].tt_gmtoff = -dstoffset;
			sp->ttis[1].tt_isdst = TRUE;
			sp->ttis[1].tt_abbrind = stdlen + 1;
		}
	} else {
		dstlen = 0;
		sp->typecnt = 1;		/* only standard time */
		sp->timecnt = 0;
		sp->ttis[0].tt_gmtoff = -stdoffset;
		sp->ttis[0].tt_isdst = 0;
		sp->ttis[0].tt_abbrind = 0;
	}
	sp->charcnt = stdlen + 1;
	if (dstlen != 0)
		sp->charcnt += dstlen + 1;
	if (sp->charcnt > sizeof sp->chars)
		return -1;
	cp = sp->chars;
	(void) strncpy(cp, stdname, stdlen);
	cp += stdlen;
	*cp++ = '\0';
	if (dstlen != 0) {
		(void) strncpy(cp, dstname, dstlen);
		*(cp + dstlen) = '\0';
	}
	return 0;
}

static void
gmtload(sp)
struct state * const	sp;
{
	if (tzload(gmt, sp) != 0)
		(void) tzparse(gmt, sp, TRUE);
}

/*
** A non-static declaration of ast_tzsetwall in a system header file
** may cause a warning about this upcoming static declaration...
*/
static
#ifdef	_THREAD_SAFE
int
ast_tzsetwall_basic P((void))
#else
int
ast_tzsetwall P((void))
#endif
{
	struct state *cur_state = lclptr;

	/* Find the appropriate structure, if already parsed */
	while (cur_state != NULL) {
		if (cur_state->name[0] == '\0')
			break;
		cur_state = cur_state->next;
	}
	if (cur_state != NULL)
		return 0;
	cur_state = malloc(sizeof(struct state));
	if (cur_state == NULL) {
		return -1;
	}
	memset(cur_state,0,sizeof(struct state));
	if (tzload((char *) NULL, cur_state) != 0)
#ifdef DEBUG
	{
		fprintf(stderr, "ast_tzsetwall: calling gmtload()\n");
#endif
		gmtload(cur_state);
#ifdef DEBUG
	}
#endif

	if (last_lclptr)
		last_lclptr->next = cur_state;
	else
		lclptr = cur_state;
	last_lclptr = cur_state;
	return 0;
}

#ifdef	_THREAD_SAFE
int
ast_tzsetwall P((void))
{
	ast_mutex_lock(&tzsetwall_mutex);
	ast_tzsetwall_basic();
	ast_mutex_unlock(&tzsetwall_mutex);
	return 0;
}
#endif

#ifdef	_THREAD_SAFE
static int
ast_tzset_basic P((const char *name))
#else
int
ast_tzset P((const char *name))
#endif
{
	struct state *cur_state = lclptr;

	/* Not set at all */
	if (name == NULL) {
		return ast_tzsetwall();
	}

	/* Find the appropriate structure, if already parsed */
	while (cur_state != NULL) {
		if (!strcmp(cur_state->name,name))
			break;
		cur_state = cur_state->next;
	}
	if (cur_state != NULL)
		return 0;

	cur_state = malloc(sizeof(struct state));
	if (cur_state == NULL) {
		return -1;
	}
	memset(cur_state,0,sizeof(*cur_state));

	/* Name is set, but set to the empty string == no adjustments */
	if (name[0] == '\0') {
		/*
		** User wants it fast rather than right.
		*/
		cur_state->leapcnt = 0;		/* so, we're off a little */
		cur_state->timecnt = 0;
		cur_state->ttis[0].tt_gmtoff = 0;
		cur_state->ttis[0].tt_abbrind = 0;
		(void) strncpy(cur_state->chars, gmt, sizeof(cur_state->chars) - 1);
	} else if (tzload(name, cur_state) != 0) {
		if (name[0] == ':') {
			(void) gmtload(cur_state);
		} else if (tzparse(name, cur_state, FALSE) != 0) {
			/* If not found, load localtime */
			if (tzload("/etc/localtime", cur_state) != 0)
				/* Last ditch, get GMT */
				(void) gmtload(cur_state);
		}
	}
	strncpy(cur_state->name, name, sizeof(cur_state->name) - 1);
	if (last_lclptr)
		last_lclptr->next = cur_state;
	else
		lclptr = cur_state;
	last_lclptr = cur_state;
	return 0;
}

#ifdef	_THREAD_SAFE
void
ast_tzset P((const char *name))
{
	ast_mutex_lock(&tzset_mutex);
	ast_tzset_basic(name);
	ast_mutex_unlock(&tzset_mutex);
}
#endif

/*
** The easy way to behave "as if no library function calls" localtime
** is to not call it--so we drop its guts into "localsub", which can be
** freely called.  (And no, the PANS doesn't require the above behavior--
** but it *is* desirable.)
**
** The unused offset argument is for the benefit of mktime variants.
*/

/*ARGSUSED*/
static void
localsub(timep, offset, tmp, zone)
const time_t * const	timep;
const long		offset;
struct tm * const	tmp;
const char * const	zone;
{
	register struct state *		sp;
	register const struct ttinfo *	ttisp;
	register int			i;
	const time_t			t = *timep;

	sp = lclptr;
	/* Find the right zone record */
	if (zone == NULL)
		sp = NULL;
	else
		while (sp != NULL) {
			if (!strcmp(sp->name,zone))
				break;
			sp = sp->next;
		}

	if (sp == NULL) {
		ast_tzsetwall();
		sp = lclptr;
		/* Find the default zone record */
		while (sp != NULL) {
			if (sp->name[0] == '\0')
				break;
			sp = sp->next;
		}
	}

	/* Last ditch effort, use GMT */
	if (sp == NULL) {
		gmtsub(timep, offset, tmp, zone);
		return;
	}
	if (sp->timecnt == 0 || t < sp->ats[0]) {
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt) {
				i = 0;
				break;
			}
	} else {
		for (i = 1; i < sp->timecnt; ++i)
			if (t < sp->ats[i])
				break;
		i = sp->types[i - 1];
	}
	ttisp = &sp->ttis[i];
	/*
	** To get (wrong) behavior that's compatible with System V Release 2.0
	** you'd replace the statement below with
	**	t += ttisp->tt_gmtoff;
	**	timesub(&t, 0L, sp, tmp);
	*/
	timesub(&t, ttisp->tt_gmtoff, sp, tmp);
	tmp->tm_isdst = ttisp->tt_isdst;
	tzname[tmp->tm_isdst] = &sp->chars[ttisp->tt_abbrind];
#ifdef TM_ZONE
	tmp->TM_ZONE = &sp->chars[ttisp->tt_abbrind];
#endif /* defined TM_ZONE */
}

struct tm *
ast_localtime(timep, p_tm, zone)
const time_t * const	timep;
struct tm *p_tm;
const char * const	zone;
{
#ifdef _THREAD_SAFE
	ast_mutex_lock(&lcl_mutex);
#endif
	ast_tzset(zone);
	localsub(timep, 0L, p_tm, zone);
#ifdef _THREAD_SAFE
	ast_mutex_unlock(&lcl_mutex);
#endif
	return(p_tm);
}

/*
** gmtsub is to gmtime as localsub is to localtime.
*/

static void
gmtsub(timep, offset, tmp, zone)
const time_t * const	timep;
const long		offset;
struct tm * const	tmp;
const char * const	zone;
{
#ifdef	_THREAD_SAFE
	ast_mutex_lock(&gmt_mutex);
#endif
	if (!gmt_is_set) {
		gmt_is_set = TRUE;
		gmtptr = (struct state *) malloc(sizeof *gmtptr);
		if (gmtptr != NULL)
			gmtload(gmtptr);
	}
	ast_mutex_unlock(&gmt_mutex);
	timesub(timep, offset, gmtptr, tmp);
#ifdef TM_ZONE
	/*
	** Could get fancy here and deliver something such as
	** "GMT+xxxx" or "GMT-xxxx" if offset is non-zero,
	** but this is no time for a treasure hunt.
	*/
	if (offset != 0)
		tmp->TM_ZONE = wildabbr;
	else {
		if (gmtptr == NULL)
			tmp->TM_ZONE = gmt;
		else	tmp->TM_ZONE = gmtptr->chars;
	}
#endif /* defined TM_ZONE */
}

static void
timesub(timep, offset, sp, tmp)
const time_t * const			timep;
const long				offset;
register const struct state * const	sp;
register struct tm * const		tmp;
{
	register const struct lsinfo *	lp;
	register long			days;
	register long			rem;
	register int			y;
	register int			yleap;
	register const int *		ip;
	register long			corr;
	register int			hit;
	register int			i;

	corr = 0;
	hit = 0;
	i = (sp == NULL) ? 0 : sp->leapcnt;
	while (--i >= 0) {
		lp = &sp->lsis[i];
		if (*timep >= lp->ls_trans) {
			if (*timep == lp->ls_trans) {
				hit = ((i == 0 && lp->ls_corr > 0) ||
					lp->ls_corr > sp->lsis[i - 1].ls_corr);
				if (hit)
					while (i > 0 &&
						sp->lsis[i].ls_trans ==
						sp->lsis[i - 1].ls_trans + 1 &&
						sp->lsis[i].ls_corr ==
						sp->lsis[i - 1].ls_corr + 1) {
							++hit;
							--i;
					}
			}
			corr = lp->ls_corr;
			break;
		}
	}
	days = *timep / SECSPERDAY;
	rem = *timep % SECSPERDAY;
#ifdef mc68k
	if (*timep == 0x80000000) {
		/*
		** A 3B1 muffs the division on the most negative number.
		*/
		days = -24855;
		rem = -11648;
	}
#endif /* defined mc68k */
	rem += (offset - corr);
	while (rem < 0) {
		rem += SECSPERDAY;
		--days;
	}
	while (rem >= SECSPERDAY) {
		rem -= SECSPERDAY;
		++days;
	}
	tmp->tm_hour = (int) (rem / SECSPERHOUR);
	rem = rem % SECSPERHOUR;
	tmp->tm_min = (int) (rem / SECSPERMIN);
	/*
	** A positive leap second requires a special
	** representation.  This uses "... ??:59:60" et seq.
	*/
	tmp->tm_sec = (int) (rem % SECSPERMIN) + hit;
	tmp->tm_wday = (int) ((EPOCH_WDAY + days) % DAYSPERWEEK);
	if (tmp->tm_wday < 0)
		tmp->tm_wday += DAYSPERWEEK;
	y = EPOCH_YEAR;
#define LEAPS_THRU_END_OF(y)	((y) / 4 - (y) / 100 + (y) / 400)
	while (days < 0 || days >= (long) year_lengths[yleap = isleap(y)]) {
		register int	newy;

		newy = y + days / DAYSPERNYEAR;
		if (days < 0)
			--newy;
		days -= (newy - y) * DAYSPERNYEAR +
			LEAPS_THRU_END_OF(newy - 1) -
			LEAPS_THRU_END_OF(y - 1);
		y = newy;
	}
	tmp->tm_year = y - TM_YEAR_BASE;
	tmp->tm_yday = (int) days;
	ip = mon_lengths[yleap];
	for (tmp->tm_mon = 0; days >= (long) ip[tmp->tm_mon]; ++(tmp->tm_mon))
		days = days - (long) ip[tmp->tm_mon];
	tmp->tm_mday = (int) (days + 1);
	tmp->tm_isdst = 0;
#ifdef TM_GMTOFF
	tmp->TM_GMTOFF = offset;
#endif /* defined TM_GMTOFF */
}

char *
ast_ctime(timep)
const time_t * const	timep;
{
/*
** Section 4.12.3.2 of X3.159-1989 requires that
**	The ctime funciton converts the calendar time pointed to by timer
**	to local time in the form of a string.  It is equivalent to
**		asctime(localtime(timer))
*/
	return asctime(localtime(timep));
}

char *
ast_ctime_r(timep, buf)
const time_t * const	timep;
char *buf;
{
        struct tm tm;
#ifdef SOLARIS
	return asctime_r(localtime_r(timep, &tm), buf, 256);
#else
	return asctime_r(localtime_r(timep, &tm), buf);
#endif
}

/*
** Adapted from code provided by Robert Elz, who writes:
**	The "best" way to do mktime I think is based on an idea of Bob
**	Kridle's (so its said...) from a long time ago.
**	[kridle@xinet.com as of 1996-01-16.]
**	It does a binary search of the time_t space.  Since time_t's are
**	just 32 bits, its a max of 32 iterations (even at 64 bits it
**	would still be very reasonable).
*/

#ifndef WRONG
#define WRONG	(-1)
#endif /* !defined WRONG */

/*
** Simplified normalize logic courtesy Paul Eggert (eggert@twinsun.com).
*/

static int
increment_overflow(number, delta)
int *	number;
int	delta;
{
	int	number0;

	number0 = *number;
	*number += delta;
	return (*number < number0) != (delta < 0);
}

static int
normalize_overflow(tensptr, unitsptr, base)
int * const	tensptr;
int * const	unitsptr;
const int	base;
{
	register int	tensdelta;

	tensdelta = (*unitsptr >= 0) ?
		(*unitsptr / base) :
		(-1 - (-1 - *unitsptr) / base);
	*unitsptr -= tensdelta * base;
	return increment_overflow(tensptr, tensdelta);
}

static int
tmcomp(atmp, btmp)
register const struct tm * const atmp;
register const struct tm * const btmp;
{
	register int	result;

	if ((result = (atmp->tm_year - btmp->tm_year)) == 0 &&
		(result = (atmp->tm_mon - btmp->tm_mon)) == 0 &&
		(result = (atmp->tm_mday - btmp->tm_mday)) == 0 &&
		(result = (atmp->tm_hour - btmp->tm_hour)) == 0 &&
		(result = (atmp->tm_min - btmp->tm_min)) == 0)
			result = atmp->tm_sec - btmp->tm_sec;
	return result;
}

static time_t
time2(tmp, funcp, offset, okayp, zone)
struct tm * const	tmp;
void (* const		funcp) P((const time_t*, long, struct tm*, const char*));
const long		offset;
int * const		okayp;
const char * const	zone;
{
	register const struct state *	sp;
	register int			dir;
	register int			bits;
	register int			i, j ;
	register int			saved_seconds;
	time_t				newt;
	time_t				t;
	struct tm			yourtm, mytm;

	*okayp = FALSE;
	yourtm = *tmp;
	if (normalize_overflow(&yourtm.tm_hour, &yourtm.tm_min, MINSPERHOUR))
		return WRONG;
	if (normalize_overflow(&yourtm.tm_mday, &yourtm.tm_hour, HOURSPERDAY))
		return WRONG;
	if (normalize_overflow(&yourtm.tm_year, &yourtm.tm_mon, MONSPERYEAR))
		return WRONG;
	/*
	** Turn yourtm.tm_year into an actual year number for now.
	** It is converted back to an offset from TM_YEAR_BASE later.
	*/
	if (increment_overflow(&yourtm.tm_year, TM_YEAR_BASE))
		return WRONG;
	while (yourtm.tm_mday <= 0) {
		if (increment_overflow(&yourtm.tm_year, -1))
			return WRONG;
		i = yourtm.tm_year + (1 < yourtm.tm_mon);
		yourtm.tm_mday += year_lengths[isleap(i)];
	}
	while (yourtm.tm_mday > DAYSPERLYEAR) {
		i = yourtm.tm_year + (1 < yourtm.tm_mon);
		yourtm.tm_mday -= year_lengths[isleap(i)];
		if (increment_overflow(&yourtm.tm_year, 1))
			return WRONG;
	}
	for ( ; ; ) {
		i = mon_lengths[isleap(yourtm.tm_year)][yourtm.tm_mon];
		if (yourtm.tm_mday <= i)
			break;
		yourtm.tm_mday -= i;
		if (++yourtm.tm_mon >= MONSPERYEAR) {
			yourtm.tm_mon = 0;
			if (increment_overflow(&yourtm.tm_year, 1))
				return WRONG;
		}
	}
	if (increment_overflow(&yourtm.tm_year, -TM_YEAR_BASE))
		return WRONG;
	if (yourtm.tm_year + TM_YEAR_BASE < EPOCH_YEAR) {
		/*
		** We can't set tm_sec to 0, because that might push the
		** time below the minimum representable time.
		** Set tm_sec to 59 instead.
		** This assumes that the minimum representable time is
		** not in the same minute that a leap second was deleted from,
		** which is a safer assumption than using 58 would be.
		*/
		if (increment_overflow(&yourtm.tm_sec, 1 - SECSPERMIN))
			return WRONG;
		saved_seconds = yourtm.tm_sec;
		yourtm.tm_sec = SECSPERMIN - 1;
	} else {
		saved_seconds = yourtm.tm_sec;
		yourtm.tm_sec = 0;
	}
	/*
	** Divide the search space in half
	** (this works whether time_t is signed or unsigned).
	*/
	bits = TYPE_BIT(time_t) - 1;
	/*
	** If time_t is signed, then 0 is just above the median,
	** assuming two's complement arithmetic.
	** If time_t is unsigned, then (1 << bits) is just above the median.
	*/
	t = TYPE_SIGNED(time_t) ? 0 : (((time_t) 1) << bits);
	for ( ; ; ) {
		(*funcp)(&t, offset, &mytm, zone);
		dir = tmcomp(&mytm, &yourtm);
		if (dir != 0) {
			if (bits-- < 0)
				return WRONG;
			if (bits < 0)
				--t; /* may be needed if new t is minimal */
			else if (dir > 0)
				t -= ((time_t) 1) << bits;
			else	t += ((time_t) 1) << bits;
			continue;
		}
		if (yourtm.tm_isdst < 0 || mytm.tm_isdst == yourtm.tm_isdst)
			break;
		/*
		** Right time, wrong type.
		** Hunt for right time, right type.
		** It's okay to guess wrong since the guess
		** gets checked.
		*/
		/*
		** The (void *) casts are the benefit of SunOS 3.3 on Sun 2's.
		*/
		sp = (const struct state *)
			(((void *) funcp == (void *) localsub) ?
			lclptr : gmtptr);
		if (sp == NULL)
			return WRONG;
		for (i = sp->typecnt - 1; i >= 0; --i) {
			if (sp->ttis[i].tt_isdst != yourtm.tm_isdst)
				continue;
			for (j = sp->typecnt - 1; j >= 0; --j) {
				if (sp->ttis[j].tt_isdst == yourtm.tm_isdst)
					continue;
				newt = t + sp->ttis[j].tt_gmtoff -
					sp->ttis[i].tt_gmtoff;
				(*funcp)(&newt, offset, &mytm, zone);
				if (tmcomp(&mytm, &yourtm) != 0)
					continue;
				if (mytm.tm_isdst != yourtm.tm_isdst)
					continue;
				/*
				** We have a match.
				*/
				t = newt;
				goto label;
			}
		}
		return WRONG;
	}
label:
	newt = t + saved_seconds;
	if ((newt < t) != (saved_seconds < 0))
		return WRONG;
	t = newt;
	(*funcp)(&t, offset, tmp, zone);
	*okayp = TRUE;
	return t;
}

static time_t
time1(tmp, funcp, offset, zone)
struct tm * const	tmp;
void (* const		funcp) P((const time_t *, long, struct tm *, const char*));
const long		offset;
const char * const	zone;
{
	register time_t			t;
	register const struct state *	sp;
	register int			samei, otheri;
	int				okay;

	if (tmp->tm_isdst > 1)
		tmp->tm_isdst = 1;
	t = time2(tmp, funcp, offset, &okay, zone);
#ifdef PCTS
	/*
	** PCTS code courtesy Grant Sullivan (grant@osf.org).
	*/
	if (okay)
		return t;
	if (tmp->tm_isdst < 0)
		tmp->tm_isdst = 0;	/* reset to std and try again */
#endif /* defined PCTS */
#ifndef PCTS
	if (okay || tmp->tm_isdst < 0)
		return t;
#endif /* !defined PCTS */
	/*
	** We're supposed to assume that somebody took a time of one type
	** and did some math on it that yielded a "struct tm" that's bad.
	** We try to divine the type they started from and adjust to the
	** type they need.
	*/
	/*
	** The (void *) casts are the benefit of SunOS 3.3 on Sun 2's.
	*/
	sp = (const struct state *) (((void *) funcp == (void *) localsub) ?
		lclptr : gmtptr);
	if (sp == NULL)
		return WRONG;
	for (samei = sp->typecnt - 1; samei >= 0; --samei) {
		if (sp->ttis[samei].tt_isdst != tmp->tm_isdst)
			continue;
		for (otheri = sp->typecnt - 1; otheri >= 0; --otheri) {
			if (sp->ttis[otheri].tt_isdst == tmp->tm_isdst)
				continue;
			tmp->tm_sec += sp->ttis[otheri].tt_gmtoff -
					sp->ttis[samei].tt_gmtoff;
			tmp->tm_isdst = !tmp->tm_isdst;
			t = time2(tmp, funcp, offset, &okay, zone);
			if (okay)
				return t;
			tmp->tm_sec -= sp->ttis[otheri].tt_gmtoff -
					sp->ttis[samei].tt_gmtoff;
			tmp->tm_isdst = !tmp->tm_isdst;
		}
	}
	return WRONG;
}

time_t
ast_mktime(tmp,zone)
struct tm * const	tmp;
const char * const	zone;
{
	time_t mktime_return_value;
#ifdef	_THREAD_SAFE
	ast_mutex_lock(&lcl_mutex);
#endif
	ast_tzset(zone);
	mktime_return_value = time1(tmp, localsub, 0L, zone);
#ifdef	_THREAD_SAFE
	ast_mutex_unlock(&lcl_mutex);
#endif
	return(mktime_return_value);
}

