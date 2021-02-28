/* Parser for /etc/finit.conf and /etc/finit.d/<SVC>.conf
 *
 * Copyright (c) 2012-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include <dirent.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <lite/lite.h>
#include <lite/queue.h>		/* BSD sys/queue.h API */
#include <time.h>
#include <glob.h>

#include "finit.h"
#include "cond.h"
#include "iwatch.h"
#include "service.h"
#include "tty.h"
#include "helpers.h"
#include "util.h"

#define BOOTSTRAP (runlevel == 0)
#define MATCH_CMD(l, c, x) \
	(!strncasecmp(l, c, strlen(c)) && (x = (l) + strlen(c)))

int logfile_size_max = 200000;	/* 200 kB */
int logfile_count_max = 5;

struct rlimit global_rlimit[RLIMIT_NLIMITS];

struct conf_change {
	TAILQ_ENTRY(conf_change) link;
	char *name;
};

static struct iwatch iw_conf;
static uev_t etcw;

static TAILQ_HEAD(head, conf_change) conf_change_list = TAILQ_HEAD_INITIALIZER(conf_change_list);

static int  parse_conf(char *file, int is_rcsd);
static void drop_changes(void);

static void hide_args(int argc, char *argv[])
{
	/*
	 * Hide command line arguments from ps (in particular for
	 * forked children that don't execv()).  This is an ugly
	 * hack that only works on Linux.
	 * https://web.archive.org/web/20110227041321/http://netsplit.com/2007/01/10/hiding-arguments-from-ps/
	 */
	if (argc > 1) {
		char *arg_end;

		arg_end = argv[argc-1] + strlen (argv[argc-1]);
		*arg_end = ' ';
	}
}

int get_bool(char *ptr, int default_value)
{
	if (!ptr)
		goto fallback;

	if (string_compare(ptr, "true") || string_compare(ptr, "on") || string_compare(ptr, "1"))
		return 1;
	if (string_compare(ptr, "false") || string_compare(ptr, "off") || string_compare(ptr, "0"))
		return 0;
fallback:
	return default_value;
}

/*
 * finit.debug = [on,off]
 * finit.show_status = [on,off]
 */
static void parse_finit_opts(char *opt)
{
	char *ptr;

	ptr = strchr(opt, '=');
	if (ptr)
		*ptr++ = 0;

	if (string_compare(opt, "debug")) {
		debug = get_bool(ptr, 1);
		return;
	}

	if (string_compare(opt, "status_style")) {
		if (string_compare(ptr, "old") || string_compare(ptr, "classic"))
			show_progress(PROGRESS_CLASSIC);
		else
			show_progress(PROGRESS_MODERN);
		return;
	}

	if (string_compare(opt, "show_status")) {
		show_progress(get_bool(ptr, 1) ? PROGRESS_DEFAULT : PROGRESS_SILENT);
		return;
	}
}

static void parse_arg(char *arg)
{
	if (!strncmp(arg, "finit.", 6)) {
		parse_finit_opts(&arg[6]);
		return;
	}

	if (string_compare(arg, "rescue") || string_compare(arg, "recover"))
		rescue = 1;

	if (string_compare(arg, "single") || string_compare(arg, "S"))
		single = 1;
}

void conf_parse_cmdline(int argc, char *argv[])
{
	int dbg = 0;
	FILE *fp;
	char line[LINE_SIZE], *cmdline, *tok;

	for (int i = 1; i < argc; i++)
		parse_arg(argv[i]);

	hide_args(argc, argv);

	fp = fopen("/proc/cmdline", "r");
	if (!fp)
		goto done;

	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		goto done;
	}

	cmdline = chomp(line);
	_d("%s", cmdline);

	while ((tok = strtok(cmdline, " \t"))) {
		cmdline = NULL;
		parse_arg(tok);
	}
	fclose(fp);

done:
	log_init(dbg);
}

static int kmod_exists(char *mod)
{
	int found = 0;
	FILE *fp;
	char buf[256];
	size_t len;

	for (len = 0; mod[len]; len++) {
		if (mod[len] == ' ' || mod[len] == '\t')
			break;
	}

	fp = fopen("/proc/modules", "r");
	if (!fp)
		return 0;

	while (!found && (fgets(buf, sizeof(buf), fp))) {
		if (!strncmp(buf, mod, len))
			found = 1;
	}
	fclose(fp);

	return found;
}

static void kmod_load(char *line)
{
	char *mod;
	char cmd[CMD_SIZE];

	if (runlevel != 0)
		return;

	mod = strip_line(line);
	if (kmod_exists(mod))
		return;

	strcpy(cmd, "modprobe ");
	strlcat(cmd, mod, sizeof(cmd));

	run_interactive(cmd, "Loading kernel module %s", mod);
}

/* Convert optional "[!123456789S]" string into a bitmask */
int conf_parse_runlevels(char *runlevels)
{
	int i, not = 0, bitmask = 0;

	if (!runlevels)
		runlevels = "[234]";
	i = 1;
	while (i) {
		int level;
		char lvl = runlevels[i++];

		if (']' == lvl || 0 == lvl)
			break;
		if ('!' == lvl) {
			not = 1;
			bitmask = 0x3FE;
			continue;
		}

		if ('s' == lvl || 'S' == lvl)
			lvl = '0';

		level = lvl - '0';
		if (level > 9 || level < 0)
			continue;

		if (not)
			CLRBIT(bitmask, level);
		else
			SETBIT(bitmask, level);
	}

	return bitmask;
}

void conf_parse_cond(svc_t *svc, char *cond)
{
	size_t i = 0;
	char *ptr;

	if (!svc) {
		_e("Invalid service pointer");
		return;
	}

	/* By default we assume UNIX daemons support SIGHUP */
	if (svc_is_daemon(svc))
		svc->sighup = 1;

	if (!cond)
		return;

	/* First character must be '!' if SIGHUP is not supported. */
	ptr = cond;
	if (ptr[i] == '!') {
		svc->sighup = 0;
		ptr++;
	}

	while (ptr[i] != '>' && ptr[i] != 0)
		i++;
	ptr[i] = 0;

	if (i >= sizeof(svc->cond)) {
		logit(LOG_WARNING, "Too long event list in declaration of %s: %s", svc->cmd, ptr);
		return;
	}

	if (!strncmp(ptr, "svc/", 4)) {
		logit(LOG_ERR, "Unsupported cond syntax for %s: <%s", svc->cmd, ptr);
		return;
	}

	strlcpy(svc->cond, ptr, sizeof(svc->cond));
}

struct rlimit_name {
	char *name;
	int val;
};

static const struct rlimit_name rlimit_names[] = {
	{ "as",         RLIMIT_AS         },
	{ "core",       RLIMIT_CORE       },
	{ "cpu",        RLIMIT_CPU        },
	{ "data",       RLIMIT_DATA       },
	{ "fsize",      RLIMIT_FSIZE      },
	{ "locks",      RLIMIT_LOCKS      },
	{ "memlock",    RLIMIT_MEMLOCK    },
	{ "msgqueue",   RLIMIT_MSGQUEUE   },
	{ "nice",       RLIMIT_NICE       },
	{ "nofile",     RLIMIT_NOFILE     },
	{ "nproc",      RLIMIT_NPROC      },
	{ "rss",        RLIMIT_RSS        },
	{ "rtprio",     RLIMIT_RTPRIO     },
#ifdef RLIMIT_RTTIME
	{ "rttime",     RLIMIT_RTTIME     },
#endif
	{ "sigpending", RLIMIT_SIGPENDING },
	{ "stack",      RLIMIT_STACK      },

	{ NULL, 0 }
};

int str2rlim(char *str)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (!strcmp(str, rn->name))
			return rn->val;
	}

	return -1;
}

char *rlim2str(int rlim)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (rn->val == rlim)
			return rn->name;
	}

	return "unknown";
}

char *lim2str(struct rlimit *rlim)
{
	char tmp[25];
	static char buf[42];

	memset(buf, 0, sizeof(buf));
	if (RLIM_INFINITY == rlim->rlim_cur)
		snprintf(tmp, sizeof(tmp), "unlimited, ");
	else
		snprintf(tmp, sizeof(tmp), "%llu, ", (unsigned long long)rlim->rlim_cur);
	strlcat(buf, tmp, sizeof(buf));

	if (RLIM_INFINITY == rlim->rlim_max)
		snprintf(tmp, sizeof(tmp), "unlimited, ");
	else
		snprintf(tmp, sizeof(tmp), "%llu, ", (unsigned long long)rlim->rlim_max);
	strlcat(buf, tmp, sizeof(buf));

	return buf;
}

/* First form: `rlimit <hard|soft> RESOURCE LIMIT` */
void conf_parse_rlimit(char *line, struct rlimit arr[])
{
	char *level, *limit, *val;
	int resource = -1;
	rlim_t cfg;

	level = strtok(line, " \t");
	if (!level)
		goto error;

	limit = strtok(NULL, " \t");
	if (!limit)
		goto error;

	val = strtok(NULL, " \t");
	if (!val) {
		/* Second form: `rlimit RESOURCE LIMIT` */
		val   = limit;
		limit = level;
		level = "both";
	}

	resource = str2rlim(limit);
	if (resource < 0 || resource > RLIMIT_NLIMITS)
		goto error;

	/* Official keyword from v3.1 is `unlimited`, from prlimit(1) */
	if (!strcmp(val, "unlimited") || !strcmp(val, "infinity")) {
		cfg = RLIM_INFINITY;
	} else {
		const char *err = NULL;

		cfg = strtonum(val, 0, (long long)2 << 31, &err);
		if (err) {
			logit(LOG_WARNING, "rlimit: invalid %s value: %s",
			      rlim2str(resource), val);
			return;
		}
	}

	if (!strcmp(level, "soft"))
		arr[resource].rlim_cur = cfg;
	else if (!strcmp(level, "hard"))
		arr[resource].rlim_max = cfg;
	else if (!strcmp(level, "both"))
		arr[resource].rlim_max = arr[resource].rlim_cur = cfg;
	else
		goto error;

	return;
error:
	logit(LOG_WARNING, "rlimit: parse error");
}

static void parse_static(char *line, int is_rcsd)
{
	char cmd[CMD_SIZE];
	char *x;

	if (BOOTSTRAP && (MATCH_CMD(line, "host ", x) || MATCH_CMD(line, "hostname ", x))) {
		if (hostname) free(hostname);
		hostname = strdup(strip_line(x));
		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "mknod ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "mknod ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Creating device node %s", dev);
		return;
	}

	/* Kernel module to load */
	if (BOOTSTRAP && MATCH_CMD(line, "module ", x)) {
		kmod_load(x);
		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "network ", x)) {
		if (network) free(network);
		network = strdup(strip_line(x));
		return;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "runparts ", x)) {
		if (runparts) free(runparts);
		runparts = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "include ", x)) {
		char *file = strip_line(x);

		strlcpy(cmd, file, sizeof(cmd));
		if (!fexist(cmd)) {
			_e("Cannot find include file %s, absolute path required!", x);
			return;
		}

		parse_conf(cmd, is_rcsd);
		return;
	}

	if (MATCH_CMD(line, "log ", x)) {
		char *tok;
		static int size = 200000, count = 5;

		tok = strtok(x, ":= ");
		while (tok) {
			if (!strncmp(tok, "size", 4))
				size = strtobytes(strtok(NULL, ":= "));
			else if (!strncmp(tok, "count", 5))
				count = strtobytes(strtok(NULL, ":= "));

			tok = strtok(NULL, ":= ");
		}

		if (size >= 0)
			logfile_size_max = size;
		if (count >= 0)
			logfile_count_max = count;
	}

	if (MATCH_CMD(line, "shutdown ", x)) {
		if (sdown) free(sdown);
		sdown = strdup(strip_line(x));
		return;
	}

	/*
	 * The desired runlevel to start when leaving bootstrap (S).
	 * Finit supports 1-9, but most systems only use 1-6, where
	 * 6 is reserved for reboot and 0 for halt/poweroff.
	 */
	if (BOOTSTRAP && MATCH_CMD(line, "runlevel ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;

		cfglevel = strtonum(token, 1, 9, &err);
		if (err)
			cfglevel = RUNLEVEL;
		if (cfglevel < 1 || cfglevel > 9 || cfglevel == 6)
			cfglevel = 2; /* Fallback */
		return;
	}
}

static void parse_dynamic(char *line, struct rlimit rlimit[], char *file)
{
	char *x;

	/* Monitored daemon, will be respawned on exit */
	if (MATCH_CMD(line, "service ", x)) {
		service_register(SVC_TYPE_SERVICE, x, rlimit, file);
		return;
	}

	/* One-shot task, will not be respawned */
	if (MATCH_CMD(line, "task ", x)) {
		service_register(SVC_TYPE_TASK, x, rlimit, file);
		return;
	}

	/* Like task but waits for completion, useful w/ [S] */
	if (MATCH_CMD(line, "run ", x)) {
		service_register(SVC_TYPE_RUN, x, rlimit, file);
		return;
	}

	/* Similar to task but is treated like a SysV init script */
	if (MATCH_CMD(line, "sysv ", x)) {
		service_register(SVC_TYPE_SYSV, x, rlimit, file);
		return;
	}

	/* Read resource limits */
	if (MATCH_CMD(line, "rlimit ", x)) {
		conf_parse_rlimit(x, rlimit);
		return;
	}

	/* Regular or serial TTYs to run getty */
	if (MATCH_CMD(line, "tty ", x)) {
		tty_register(strip_line(x), rlimit, file);
		return;
	}
}

static void tabstospaces(char *line)
{
	if (!line)
		return;

	for (int i = 0; line[i]; i++) {
		if (line[i] == '\t')
			line[i] = ' ';
	}
}

static int parse_conf(char *file, int is_rcsd)
{
	struct rlimit rlimit[RLIMIT_NLIMITS];
	char line[LINE_SIZE] = "";
	FILE *fp;

	fp = fopen(file, "r");
	if (!fp)
		return 1;

	/* Prepare default limits for each service in /etc/finit.d/ */
	if (is_rcsd)
		memcpy(rlimit, global_rlimit, sizeof(rlimit));

	_d("*** Parsing %s", file);
	while (!feof(fp)) {
		char *x;

		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
//DEV		_d("%s", line);

		/* Skip comments, i.e. lines beginning with # */
		if (MATCH_CMD(line, "#", x))
			continue;

		parse_static(line, is_rcsd);
		parse_dynamic(line, is_rcsd ? rlimit : global_rlimit, file);
	}

	fclose(fp);

	return 0;
}

/*
 * Reload /etc/finit.conf and all *.conf in /etc/finit.d/
 */
int conf_reload(void)
{
	size_t i;
	glob_t gl;

	/* Set time according to current time zone */
	tzset();
	_d("Set time  daylight: %d  timezone: %ld  tzname: %s %s",
	   daylight, timezone, tzname[0], tzname[1]);

	/* Mark and sweep */
	svc_mark_dynamic();
	tty_mark();

	/*
	 * Get current global limits, which may be overridden from both
	 * finit.conf, for Finit and its services like getty+watchdogd,
	 * and *.conf in finit.d/, for each service(s) listed there.
	 */
	for (int i = 0; i < RLIMIT_NLIMITS; i++)
		getrlimit(i, &global_rlimit[i]);

	if (rescue) {
		int rc;
		char line[80] = "tty [12345] @console noclear nologin";

		/* If rescue.conf is missing, fall back to a root shell */
		rc = parse_conf(RESCUE_CONF, 0);
		if (rc)
			tty_register(line, global_rlimit, NULL);

		print(rc, "Entering rescue mode");
		goto done;
	}

	/* First, read /etc/finit.conf */
	parse_conf(FINIT_CONF, 0);

	/* Set global limits */
	for (int i = 0; i < RLIMIT_NLIMITS; i++) {
		if (setrlimit(i, &global_rlimit[i]) == -1)
			logit(LOG_WARNING, "rlimit: Failed setting %s: %s",
			      rlim2str(i), lim2str(&global_rlimit[i]));
	}

	/* Next, read all *.conf in /etc/finit.d/ */
	glob(FINIT_RCSD "/*.conf", 0, NULL, &gl);
	glob(FINIT_RCSD "/enabled/*.conf", GLOB_APPEND, NULL, &gl);

	for (i = 0; i < gl.gl_pathc; i++) {
		char *path = gl.gl_pathv[i];
		char *rp = NULL;
		struct stat st;
		size_t len;

		/* Check that it's an actual file ... beyond any symlinks */
		if (lstat(path, &st)) {
			_d("Skipping %s, cannot access: %s", path, strerror(errno));
			continue;
		}

		/* Skip directories */
		if (S_ISDIR(st.st_mode)) {
			_d("Skipping directory %s", path);
			continue;
		}

		/* Check for dangling symlinks */
		if (S_ISLNK(st.st_mode)) {
			path = rp = realpath(path, NULL);
			if (!rp) {
				logit(LOG_WARNING, "Skipping %s, dangling symlink: %s", path, strerror(errno));
				continue;
			}
		}

		/* Check that file ends with '.conf' */
		len = strlen(path);
		if (len < 6 || strcmp(&path[len - 5], ".conf"))
			_d("Skipping %s, not a valid .conf ... ", path);
		else
			parse_conf(path, 1);

		if (rp)
			free(rp);
	}

	globfree(&gl);

done:
	/* Drop record of all .conf changes */
	drop_changes();

	/* Override configured runlevel, user said 'S' on /proc/cmdline */
	if (BOOTSTRAP && single)
		cfglevel = 1;

	/*
	 * Set host name, from %DEFHOST, *.conf or /etc/hostname.  The
	 * latter wins, if neither exists we default to "noname"
	 */
	set_hostname(&hostname);

	return 0;
}

static struct conf_change *conf_find(char *file)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp) {
		_d("file: %s vs changed file: %s", file, node->name);
		if (string_compare(node->name, file))
			return node;
	}

	return NULL;
}

static void drop_change(struct conf_change *node)
{
	if (!node)
		return;

	TAILQ_REMOVE(&conf_change_list, node, link);
	free(node->name);
	free(node);
}


static void drop_changes(void)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp)
		drop_change(node);
}

static int do_change(char *dir, char *name, uint32_t mask)
{
	char fn[strlen(dir) + strlen(name) + 2];
	struct conf_change *node;

	paste(fn, sizeof(fn), dir, name);
	_d("path: %s mask: %08x", fn, mask);

	node = conf_find(fn);
	if (mask & (IN_DELETE | IN_MOVED_FROM)) {
		drop_change(node);
		return 0;
	}

	if (node) {
		_d("Event already registered for %s ...", name);
		return 0;
	}

	node = malloc(sizeof(*node));
	if (!node)
		return 1;

	node->name = strdup(fn);
	if (!node->name) {
		free(node);
		return 1;
	}

	_d("Event registered for %s, mask 0x%x", fn, mask);
	TAILQ_INSERT_HEAD(&conf_change_list, node, link);

	return 0;
}

int conf_any_change(void)
{
	if (TAILQ_EMPTY(&conf_change_list))
		return 0;

	return 1;
}

int conf_changed(char *file)
{
	if (!file)
		return 0;

	if (conf_find(file))
		return 1;

	return 0;
}

static void conf_cb(uev_t *w, void *arg, int events)
{
	static char ev_buf[8 *(sizeof(struct inotify_event) + NAME_MAX + 1) + 1];
	struct inotify_event *ev;
	ssize_t sz, off;

	sz = read(w->fd, ev_buf, sizeof(ev_buf) - 1);
	if (sz <= 0) {
		_pe("invalid inotify event");
		return;
	}
	ev_buf[sz] = 0;

	for (off = 0; off < sz; off += sizeof(*ev) + ev->len) {
		struct iwatch_path *iwp;

		ev = (struct inotify_event *)&ev_buf[off];
		if (!ev->mask)
			continue;

		_d("name %s, event: 0x%08x", ev->name, ev->mask);

		/* Find base path for this event */
		iwp = iwatch_find_by_wd(&iw_conf, ev->wd);
		if (!iwp)
			continue;

		if (do_change(iwp->path, ev->name, ev->mask)) {
			_pe(" Out of memory");
			break;
		}
	}

#ifdef ENABLE_AUTO_RELOAD
	if (conf_any_change())
		service_reload_dynamic();
#endif
}

/*
 * Set up inotify watcher and load all *.conf in /etc/finit.d/
 */
int conf_monitor(void)
{
	int rc = 0;

	/*
	 * If only one watcher fails, that's OK.  A user may have only
	 * one of /etc/finit.conf or /etc/finit.d in use, and may also
	 * have or not have symlinks in place.  We need to monitor for
	 * changes to either symlink or target.
	 */
	rc += iwatch_add(&iw_conf, FINIT_RCSD, IN_ONLYDIR);
	rc += iwatch_add(&iw_conf, FINIT_RCSD "/available/", IN_ONLYDIR | IN_DONT_FOLLOW);
	rc += iwatch_add(&iw_conf, FINIT_RCSD "/enabled/",   IN_ONLYDIR | IN_DONT_FOLLOW);
	rc += iwatch_add(&iw_conf, FINIT_CONF, 0);

	/*
	 * Systems with /etc/default, or similar, can also monitor changes
	 * in env files sourced by .conf files (above)
	 */
#ifdef FINIT_SYSCONFIG
	rc += iwatch_add(&iw_conf, FINIT_SYSCONFIG, IN_ONLYDIR);
#endif

	return rc + conf_reload();
}

/*
 * Prepare .conf parser and load all .conf files
 */
int conf_init(uev_ctx_t *ctx)
{
	int fd;

	/* default hostname */
	hostname = strdup(DEFHOST);

	/*
	 * Get current global limits, which may be overridden from both
         * finit.conf, for Finit and its services like getty+watchdogd,
         * and *.conf in finit.d/, for each service(s) listed there.
         */
        for (int i = 0; i < RLIMIT_NLIMITS; i++)
                getrlimit(i, &global_rlimit[i]);

	/* prepare /etc watcher */
	fd = iwatch_init(&iw_conf);
	if (fd < 0)
		return 1;

	if (uev_io_init(ctx, &etcw, conf_cb, NULL, fd, UEV_READ)) {
		_pe("Failed setting up I/O callback for /etc watcher");
		close(fd);
		return 1;
	}

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
