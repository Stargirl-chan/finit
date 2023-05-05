/* Misc. shared utility functions for initctl, reboot and finit
 *
 * Copyright (c) 2016-2023  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <ctype.h>		/* isprint() */
#include <errno.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <regex.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef HAVE_TERMIOS_H
# include <termios.h>
#endif
#include <time.h>
#include <unistd.h>
#include <utmp.h>
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#include <sys/sysinfo.h>	/* sysinfo() */
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif
#include "util.h"

#ifdef HAVE_TERMIOS_H
/*
 * This flag is used on *BSD when calling tcsetattr() to prevent it
 * from changing speed, duplex, parity.  GNU says we should use the
 * CIGNORE flag to c_cflag, but that doesn't exist so ... we rely on
 * our initial tcgetattr() and prey that nothing changes on the TTY
 * before we exit and restore with tcsetattr()
 */
#ifndef TCSASOFT
#define TCSASOFT 0
#endif

static struct termios ttold;
static struct termios ttnew;
#endif

int   ttrows  = 24;
int   ttcols  = 80;
char *prognm  = NULL;

static char *signames[] = {
	"",
	"HUP",				/* 1 */
	"INT",
	"QUIT",
	"ILL",
	"TRAP",
	"ABRT",
	"BUS",
	"FPE",				/* 8 */
	"KILL",
	"USR1",
	"SEGV",
	"USR2",
	"PIPE",
	"ALRM",
	"TERM",
	"STKFLT",			/* 16 */
	"CHLD",
	"CONT",
	"STOP",
	"TSTP",
	"TTIN",
	"TTOU",
	"URG",
	"XCPU",				/* 24 */
	"XFSZ",
	"VTALRM",
	"PROF",
	"WINCH",
	"IO",
	"PWR",
	"SYS",
};

/* https://freedesktop.org/software/systemd/man/systemd.exec.html#id-1.20.8 */
static char *exitcodes[] = {
	"SUCCESS",			/* 0: Std C exit OK   */
	"FAILURE",			/* 1: Std C exit FAIL */
	/* 2-7: LSB init scripts (usually) */
	"INVALIDARGUMENT",		/* 2: Invalid or excess args */
	"NOTIMPLEMENTED",		/* 3: Unimplemented feature, e.g. 'reload' */
	"NOPERMISSION",			/* 4: Insufficient privilege */
	"NOTINSTALLED",			/* 5: Program is not installed */
	"NOTCONFIGURED",		/* 6: Program is not configured */
	"NOTRUNNING",			/* 7: Program is not running */
	"", "", "", "", "", "", "", "",	/* 8-63: Not standardized */
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", /* 16-31 */
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", /* 32-47 */
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", /* 48-63 */
	/* 64-78: BSD, from sysexit.h */
	"USAGE",			/* 64: Command line usage error */
	"DATAERR",			/* 65: data format error */
	"NOINPUT",			/* 66: cannot open input */
	"NOUSER",			/* 67: addressee unknown */
	"NOHOST",			/* 68: host name unknown */
	"UNAVAILABLE",			/* 69: service unavailable */
	"SOFTWARE",			/* 70: internal software error */
	"OSERR",			/* 71: system error (e.g., can't fork) */
	"OSFILE",			/* 72: critical OS file missing */
	"CANTCREAT",			/* 73: can't create (user) output file */
	"IOERR",			/* 74: input/output error */
	"TEMPFAIL",			/* 75: temp failure; user is invited to retry */
	"PROTOCOL",			/* 76: remote error in protocol */
	"NOPERM",			/* 77: permission denied */
	"CONFIG",			/* 78: configuration error */
	/* 79-199: Not standardized, typically 127 == -1 */
	/* >= 200: reserved (LSB), used by systemd */
};


char *progname(char *arg0)
{
       prognm = strrchr(arg0, '/');
       if (prognm)
	       prognm++;
       else
	       prognm = arg0;

       return prognm;
}

char *str(char *fmt, ...)
{
	static char buf[32];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return buf;
}

int fnread(char *buf, size_t len, char *fmt, ...)
{
	char path[256];
	va_list ap;
	FILE *fp;

	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);

	if (!buf || !len) {
		struct stat st;

		if (stat(path, &st))
			return -1;

		return (ssize_t)st.st_size;
	}

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	len = fread(buf, sizeof(char), len - 1, fp);
	buf[len] = 0;
	fclose(fp);

	return (int)len;
}

int fnwrite(char *value, char *fmt, ...)
{
	char path[256];
	va_list ap;
	FILE *fp;
	int rc;

	if (!value) {
		errno = EINVAL;
		return -1;
	}

	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);

	fp = fopen(path, "w");
	if (!fp)
		return -1;

	/* echo(1) always adds a newline */
	if (fputs(value, fp) == EOF || fputs("\n", fp) == EOF)
		rc = -1;
	if (fclose(fp) == EOF)
		rc = -1;
	else
		rc = 0;

	return rc;
}

int fngetint(char *path, int *val)
{
	char buf[64];

	if (fnread(buf, sizeof(buf), "%s", path) <= 0)
		return -1;

	errno = 0;
	*val = strtoul(chomp(buf), NULL, 0);
	if (errno)
		return -1;

	return 0;
}

/*
 * This is a replacement for the non-working reboot(RB_SW_SUSPEND).  It
 * checks for suspend to RAM support, assuming `mem_sleep` has a sane
 * default, e.g. 'deep' or 's2idle'.  The latter should be possible to
 * select from finit.conf, but for now we go with the kernel default.
 * For more information on kernel sleep states, see:
 * https://www.kernel.org/doc/html/latest/admin-guide/pm/sleep-states.html
 */
int suspend(void)
{
	char buf[80];
	char *ptr;

	if (fnread(buf, sizeof(buf), "/sys/power/state") <= 0) {
	opnotsup:
		errno = EINVAL;
		return 1;
	}

	chomp(buf);

	ptr = strtok(buf, " ");
	while (ptr) {
		if (!strcmp(ptr, "mem"))
			break;

		ptr = strtok(NULL, " ");
	}
	if (!ptr)
		goto opnotsup;

	return fnwrite("mem", "/sys/power/state");
}

int strtobytes(char *arg)
{
	int mod = 0, bytes;
	size_t pos;

	if (!arg)
		return -1;

	pos = strspn(arg, "0123456789");
	if (arg[pos] != 0) {
		if (arg[pos] == 'G')
			mod = 3;
		else if (arg[pos] == 'M')
			mod = 2;
		else if (arg[pos] == 'k')
			mod = 1;
		else
			return -1;

		arg[pos] = 0;
	}

	bytes = atoi(arg);
	while (mod--)
		bytes *= 1000;

	return bytes;
}

char *sig2str(int signo)
{
	if (signo < 1 || signo >= (int)NELEMS(signames))
		return "";

	return signames[signo];
}

/**
 * str2sig - Translate signal name to the corresponding signal number.
 * @sig: The name of the signal
 *
 * A signal can be a complete signal name such as "SIGHUP", or
 * it can be the shortest unique name, such as "HUP" (no SIG prefix).
 */
int str2sig(char *sig)
{
	size_t i;

	if (!strncasecmp(sig, "SIG", 3))
		sig += 3;

	for (i = 1; i < NELEMS(signames); ++i) {
		if (strcasecmp(sig, signames[i]) == 0)
			return i;
	}

	return -1;
}

char *code2str(int code)
{
	if (code < 0 || code >= (int)NELEMS(exitcodes))
		return "";

	return exitcodes[code];
}


void do_sleep(unsigned int sec)
{
	struct timespec requested_sleep = {
		.tv_sec = sec,
		.tv_nsec = 0,
	};
	while (nanosleep(&requested_sleep, &requested_sleep))
		;
}

void do_usleep(unsigned int usec)
{
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);

	deadline.tv_nsec += usec * 1000;
	deadline.tv_sec += deadline.tv_nsec / 1000000000;
	deadline.tv_nsec = deadline.tv_nsec % 1000000000;

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) != 0 && errno == EINTR)
		;
}

/* Seconds since boot, from sysinfo() */
long jiffies(void)
{
	struct sysinfo si;

	if (!sysinfo(&si))
		return si.uptime;

	return 0;
}

char *uptime(long secs, char *buf, size_t len)
{
	long mins, hours, days, years;
	char y[20] = "", d[20] = "", h[20] = "", m[20] = "", s[20] = "";

	if (!buf) {
		errno = EINVAL;
		return NULL;
	}

	years = secs / 31556926;
	secs  = secs % 31556926;
	days  = secs / 86400;
	secs  = secs % 86400;
	hours = secs / 3600;
	secs  = secs % 3600;
	mins  = secs / 60;
	secs  = secs % 60;

	if (years)
		snprintf(y, sizeof(y), "%ld year", years);
	if (days)
		snprintf(d, sizeof(d), "%ld day", days);
	if (hours)
		snprintf(h, sizeof(h), "%ld hour", hours);
	if (mins)
		snprintf(m, sizeof(m), "%ld min", mins);
	if (secs)
		snprintf(s, sizeof(s), "%ld sec", secs);

	snprintf(buf, len, "%s%s%s%s%s%s%s%s%s",
		 y, years ? " " : "",
		 d, days  ? " " : "",
		 h, hours ? " " : "",
		 m, mins  ? " " : "",
		 s);

	return buf;
}

char *memsz(uint64_t sz, char *buf, size_t len)
{
        int gb, mb, kb, b;

	if (!sz) {
		strlcpy(buf, "--.--", len);
		return buf;
	}

        gb = sz / (1024 * 1024 * 1024);
        sz = sz % (1024 * 1024 * 1024);
        mb = sz / (1024 * 1024);
        sz = sz % (1024 * 1024);
        kb = sz / (1024);
        b  = sz % (1024);

        if (gb)
                snprintf(buf, len, "%d.%dG", gb, mb / 102);
        else if (mb)
                snprintf(buf, len, "%d.%dM", mb, kb / 102);
        else
                snprintf(buf, len, "%d.%dk", kb, b / 102);

        return buf;
}

/*
 * Verify string argument is NUL terminated
 * Verify string is a JOB[:ID], JOB and ID
 * can both be string or number, or combo.
 */
char *sanitize(char *arg, size_t len)
{
	const char *regex = "[a-z0-9_]+[:]?[a-z0-9_]*";
	regex_t preg;
	int rc;

	if (!memchr(arg, 0, len))
		return NULL;

	if (regcomp(&preg, regex, REG_ICASE | REG_EXTENDED))
		return NULL;

	rc = regexec(&preg, arg, 0, NULL, 0);
	regfree(&preg);
	if (rc)
		return NULL;

	return arg;
}

void de_dotdot(char *file)
{
        char *cp, *cp2;
        int l;

        /* Remove leading ./ and any /./ sequences. */
        while (strncmp(file, "./", 2) == 0)
                memmove(file, file + 2, strlen(file) - 1);
        while ((cp = strstr(file, "/./")))
                memmove(cp, cp + 2, strlen(cp) - 1);

        /* Alternate between removing leading ../ and removing xxx/../ */
        for (;;) {
                while (strncmp(file, "../", 3) == 0)
                        memmove(file, file + 3, strlen(file) - 2);
                cp = strstr(file, "/../");
                if (!cp)
                        break;

                for (cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2)
                        continue;

                memmove(cp2 + 1, cp + 4, strlen(cp + 3));
        }

        /* Also elide any xxx/.. at the end. */
        while ((l = strlen(file)) > 3 && strcmp((cp = file + l - 3), "/..") == 0) {
                for (cp2 = cp - 1; cp2 >= file && *cp2 != '/'; --cp2)
                        continue;
                if (cp2 < file)
                        break;
                *cp2 = '\0';
        }

        /* Collapse any multiple / sequences. */
        while ((cp = strstr(file, "//"))) {
                cp2 = cp + 1;
                memmove(cp, cp2, strlen(cp2) + 1);
        }
}

static int hasopt(char *opts, char *opt)
{
	char buf[strlen(opts) + 1];
	char *ptr;

	memcpy(buf, opts, sizeof(buf));
	ptr = strtok(buf, ",");
	while (ptr) {
		if (!strcmp(ptr, opt))
			return 1;

		ptr = strtok(NULL, ",");
	}

	return 0;
}

int ismnt(char *file, char *dir, char *mode)
{
	struct mntent mount;
	struct mntent *mnt;
	char buf[256];
	int found = 0;
	FILE *fp;

	fp = setmntent(file, "r");
	if (!fp)
		return 0;	/* Dunno, maybe not */

	while ((mnt = getmntent_r(fp, &mount, buf, sizeof(buf)))) {
		if (!strcmp(mnt->mnt_dir, dir)) {
			if (mode) {
				if (hasopt(mnt->mnt_opts, mode))
					found = 1;
			} else
				found = 1;

			break;
		}
	}
	endmntent(fp);

	return found;
}

/* Requires /proc to be mounted */
int fismnt(char *dir)
{
	return ismnt("/proc/mounts", dir, NULL);
}

#ifdef HAVE_TERMIOS_H
/*
 * Called by initctl, and by finit at boot and shutdown, to
 * (re)initialize the screen size for print() et al.
 */
int ttinit(void)
{
	struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
	struct winsize ws = { 0 };
	struct termios tc, saved;
	int cached = 0;

	/*
	 * Basic TTY init, CLOCaL is important or TIOCWINSZ will block
	 * until DCD is asserted, and we won't ever get it.
	 */
	if (!tcgetattr(STDERR_FILENO, &tc)) {
		saved = tc;
		cached = 1;

		tc.c_cflag |= (CLOCAL | CREAD);
		tc.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
		tcsetattr(STDERR_FILENO, TCSANOW, &tc);
	}

	/* 1. Try TIOCWINSZ to query window size from kernel */
	if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) {
		ttrows = ws.ws_row;
		ttcols = ws.ws_col;

		/* Likely doesn't work in PID 1 after kernel starts us ... */
		if (!ws.ws_row && !ws.ws_col)
			goto fallback;
	} else if (!isatty(STDOUT_FILENO)) {
		/* 2. We may be running under watch(1) */
		ttcols = atonum(getenv("COLUMNS"));
		ttrows = atonum(getenv("LINES"));
	} else {
	fallback:
		/* 3. ANSI goto + query cursor position trick as fallback */
		fprintf(stderr, "\e7\e[r\e[999;999H\e[6n");

		if (poll(&fd, 1, 300) > 0) {
			int row, col;

			if (scanf("\e[%d;%dR", &row, &col) == 2) {
				ttcols = col;
				ttrows = row;
			}
		}

		/* Jump back to where we started (\e7) */
		fprintf(stderr, "\e8");
	}

	if (cached)
		tcsetattr(STDERR_FILENO, TCSANOW, &saved);

	/* Sanity check */
	if (ttcols <= 0)
		ttcols = 80;
	if (ttrows <= 0)
		ttrows = 24;

	return ttcols;
}

/*
 * This function sets the terminal to RAW mode, as defined for the current
 * shell.  This is called both by ttopen() above and by spawncli() to
 * get the current terminal settings and then change them to what
 * mg expects.	Thus, tty changes done while spawncli() is in effect
 * will be reflected in mg.
 */
int ttraw(void)
{
	if (tcgetattr(0, &ttold) == -1) {
		fprintf(stderr, "%s: failed querying tty attrs: %s\n",
			prognm, strerror(errno));
		return 1;
	}

	(void)memcpy(&ttnew, &ttold, sizeof(ttnew));
	/* Set terminal to 'raw' mode and ignore a 'break' */
	ttnew.c_cc[VMIN] = 1;
	ttnew.c_cc[VTIME] = 0;
	ttnew.c_iflag |= IGNBRK;
	ttnew.c_iflag &= ~(BRKINT | PARMRK | INLCR | IGNCR | ICRNL | IXON);
	ttnew.c_oflag &= ~OPOST;
	ttnew.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);

	if (tcsetattr(0, TCSASOFT | TCSADRAIN, &ttnew) == -1) {
		fprintf(stderr, "%s: failed setting tty in raw mode: %s\n",
			prognm, strerror(errno));
		return 1;
	}

	return 0;
}

/*
 * This function restores all terminal settings to their default values,
 * in anticipation of exiting or suspending the editor.
 */
int ttcooked(void)
{
	if (tcsetattr(0, TCSASOFT | TCSADRAIN, &ttold) == -1) {
		fprintf(stderr, "%s: failed restoring tty to cooked mode: %s\n",
			prognm, strerror(errno));
		return 1;
	}

	return 0;
}
#endif /* HAVE_TERMIOS_H */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
