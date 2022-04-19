/* UTMP API
 *
 * Copyright (c) 2016-2022  Joachim Wiberg <troglobit@gmail.com>
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

#include "config.h"

#include <time.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif

#include "helpers.h"
#include "util.h"
#include "utmp-api.h"

#define MAX_NO 5
#define MAX_SZ 100 * 1024

extern int logrotate(char *file, int num, off_t sz);


static void utmp_strncpy(char *dst, const char *src, size_t dlen)
{
	size_t i;

	for (i = 0; i < dlen && src[i]; i++)
		dst[i] = src[i];

	if (i < dlen)
		dst[i] = 0;
}

/*
 * Rotate /var/log/wtmp (+ btmp?) and /run/utmp
 *
 * Useful on systems with no logrotate daemon, e.g. BusyBox based
 * systems where syslogd rotates its own log files only.
 */
void utmp_logrotate(void)
{
#ifdef LOGROTATE_ENABLED
	int i;
	char *files[] = {
		_PATH_UTMP,
		_PATH_WTMP,
		_PATH_BTMP,
		_PATH_LASTLOG,
		NULL
	};

	if (!has_utmp())
		return;

	for (i = 0; files[i]; i++)
		logrotate(files[i], MAX_NO, MAX_SZ);
#endif /* LOGROTATE_ENABLED */
}

int utmp_set(int type, int pid, char *line, char *id, char *user)
{
	int result = 0;
	struct utmp ut;
	struct utsname uts;

	if (!has_utmp())
		return 0;

	switch (type) {
	case RUN_LVL:
	case BOOT_TIME:
		line = "~";
		id   = "~~";
		break;

	default:
		break;
	}

	memset(&ut, 0, sizeof(ut));
	ut.ut_type = type;
	ut.ut_pid  = pid;
	if (user)
		utmp_strncpy(ut.ut_user, user, sizeof(ut.ut_user));
	if (line)
		utmp_strncpy(ut.ut_line, line, sizeof(ut.ut_line));
	if (id)
		utmp_strncpy(ut.ut_id, id, sizeof(ut.ut_id));
	if (!uname(&uts))
		utmp_strncpy(ut.ut_host, uts.release, sizeof(ut.ut_host));
	ut.ut_tv.tv_sec = time(NULL);

	if (type != DEAD_PROCESS) {
		setutent();
		result += pututline(&ut) ? 0 : 1;
		endutent();
	}

	utmp_logrotate();
	updwtmp(_PATH_WTMP, &ut);

	return result;
}

int utmp_set_boot(void)
{
	return utmp_set(BOOT_TIME, 0, NULL, NULL, "reboot");
}

int utmp_set_halt(void)
{
	return utmp_set(RUN_LVL, 0, NULL, NULL, "shutdown");
}

static int set_getty(int type, char *tty, char *id, char *user)
{
	char *line;

	if (!has_utmp())
		return 0;

	if (!tty)
		line = NULL;
	else if (!strncmp(tty, "/dev/", 5))
		line = tty + 5;
	else
		line = tty;

	if (!id && line) {
		id = line;
		if (!strncmp(id, "pts/", 4))
			id += 4;
	}

	return utmp_set(type, getpid(), line, id, user);
}

int utmp_set_init(char *tty, char *id)
{
	return set_getty(INIT_PROCESS, tty, id, NULL);
}

int utmp_set_login(char *tty, char *id)
{
	return set_getty(LOGIN_PROCESS, tty, id, "LOGIN");
}

int utmp_set_dead(int pid)
{
	return utmp_set(DEAD_PROCESS, pid, NULL, NULL, NULL);
}

static int encode(int lvl)
{
	if (!lvl) return 0;
	return lvl + '0';
}

int utmp_set_runlevel(int pre, int now)
{
	return utmp_set(RUN_LVL, (encode(pre) << 8) | (encode(now) & 0xFF), NULL, NULL, "runlevel");
}

void runlevel_set(int pre, int now)
{
	utmp_set_runlevel(pre, now);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
