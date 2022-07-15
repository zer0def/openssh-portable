/* $OpenBSD: scp.c,v 1.247 2022/03/20 08:52:17 djm Exp $ */
/*
 * scp - secure remote copy.  This is basically patched BSD rcp which
 * uses ssh to do the data transfer (instead of using rcmd).
 *
 * NOTE: This version should NOT be suid root.  (This uses ssh to
 * do the transfer and ssh has the necessary privileges.)
 *
 * 1995 Timo Rinne <tri@iki.fi>, Tatu Ylonen <ylo@cs.hut.fi>
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */
/*
 * Copyright (c) 1999 Theo de Raadt.  All rights reserved.
 * Copyright (c) 1999 Aaron Campbell.  All rights reserved.
 * Copyright (c) 2021 Chris Rapier. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Parts from:
 *
 * Copyright (c) 1983, 1990, 1992, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "includes.h"

#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#else
# ifdef HAVE_SYS_POLL_H
#  include <sys/poll.h>
# endif
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <sys/wait.h>
#include <sys/uio.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_FNMATCH_H
#include <fnmatch.h>
#endif
#ifdef USE_SYSTEM_GLOB
# include <glob.h>
#else
# include "openbsd-compat/glob.h"
#endif
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(HAVE_STRNVIS) && defined(HAVE_VIS_H) && !defined(BROKEN_STRNVIS)
#include <vis.h>
#endif

#include "xmalloc.h"
#include "ssh.h"
#include "atomicio.h"
#include "pathnames.h"
#include "log.h"
#include "misc.h"
#include "progressmeter.h"
#include "utf8.h"
#ifdef WITH_OPENSSL 
#include <openssl/evp.h>
#endif
#include "sftp.h"

#include "sftp-common.h"
#include "sftp-client.h"

extern char *__progname;

#define COPY_BUFLEN	16384

int do_cmd(char *, char *, char *, int, int, char *, int *, int *, pid_t *);
int do_cmd2(char *, char *, int, char *, int, int);

/* Struct for addargs */
arglist args;
arglist remote_remote_args;

/* Bandwidth limit */
long long limit_kbps = 0;
struct bwlimit bwlimit;

/* Name of current file being transferred. */
char *curfile;

/* This is set to non-zero to enable verbose mode. */
int verbose_mode = 0;
LogLevel log_level = SYSLOG_LEVEL_INFO;

/* This is set to zero if the progressmeter is not desired. */
int showprogress = 1;

/*
 * This is set to non-zero if remote-remote copy should be piped
 * through this process.
 */
int throughlocal = 1;

/* Non-standard port to use for the ssh connection or -1. */
int sshport = -1;

/* This is the program to execute for the secured connection. ("ssh" or -S) */
char *ssh_program = _PATH_SSH_PROGRAM;

/* this is path to the remote scp program allowing the user to specify
 * a non-default scp */
char *remote_path;

/* This is used to store the pid of ssh_program */
pid_t do_cmd_pid = -1;
pid_t do_cmd_pid2 = -1;

/* Needed for sftp */
volatile sig_atomic_t interrupted = 0;

int remote_glob(struct sftp_conn *, const char *, int,
    int (*)(const char *, int), glob_t *); /* proto for sftp-glob.c */

/* Flag to indicate that this is a file resume */
int resume_flag = 0; /* 0 is off, 1 is on */

/* we want the host name for debugging purposes */
char hostname[HOST_NAME_MAX + 1];

/* defines for the resume function. Need them even if not supported */
#define HASH_LEN 128               /*40 sha1, 64 blake2s256 128 blake2b512*/
#define BUF_AND_HASH HASH_LEN + 64 /* length of the hash and other data to get size of buffer */
#define HASH_BUFLEN 8192	   /* 8192 seems to be a good balance between freads 
				    * and the digest func*/
static void
killchild(int signo)
{
	if (do_cmd_pid > 1) {
		kill(do_cmd_pid, signo ? signo : SIGTERM);
		waitpid(do_cmd_pid, NULL, 0);
	}
	if (do_cmd_pid2 > 1) {
		kill(do_cmd_pid2, signo ? signo : SIGTERM);
		waitpid(do_cmd_pid2, NULL, 0);
	}

	if (signo)
		_exit(1);
	exit(1);
}

static void
suspone(int pid, int signo)
{
	int status;

	if (pid > 1) {
		kill(pid, signo);
		while (waitpid(pid, &status, WUNTRACED) == -1 &&
		    errno == EINTR)
			;
	}
}

static void
suspchild(int signo)
{
	suspone(do_cmd_pid, signo);
	suspone(do_cmd_pid2, signo);
	kill(getpid(), SIGSTOP);
}

static int
do_local_cmd(arglist *a)
{
	u_int i;
	int status;
	pid_t pid;

#ifdef DEBUG
	fprintf(stderr, "In do_local_cmd\n");
#endif
	if (a->num == 0)
		fatal("do_local_cmd: no arguments");

	if (verbose_mode) {
		fprintf(stderr, "Executing:");
		for (i = 0; i < a->num; i++)
			fmprintf(stderr, " %s", a->list[i]);
		fprintf(stderr, "\n");
	}
	if ((pid = fork()) == -1)
		fatal("do_local_cmd: fork: %s", strerror(errno));

	if (pid == 0) {
		execvp(a->list[0], a->list);
		perror(a->list[0]);
		exit(1);
	}

	do_cmd_pid = pid;
	ssh_signal(SIGTERM, killchild);
	ssh_signal(SIGINT, killchild);
	ssh_signal(SIGHUP, killchild);

	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			fatal("do_local_cmd: waitpid: %s", strerror(errno));

	do_cmd_pid = -1;

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return (-1);

	return (0);
}

/*
 * This function executes the given command as the specified user on the
 * given host.  This returns < 0 if execution fails, and >= 0 otherwise. This
 * assigns the input and output file descriptors on success.
 */

int
do_cmd(char *program, char *host, char *remuser, int port, int subsystem,
    char *cmd, int *fdin, int *fdout, pid_t *pid)
{
	int pin[2], pout[2], reserved[2];

	if (verbose_mode)
		fmprintf(stderr,
		    "Executing: program %s host %s, user %s, command %s\n",
		    program, host,
		    remuser ? remuser : "(unspecified)", cmd);

	if (port == -1)
		port = sshport;

	/*
	 * Reserve two descriptors so that the real pipes won't get
	 * descriptors 0 and 1 because that will screw up dup2 below.
	 */
	if (pipe(reserved) == -1)
		fatal("pipe: %s", strerror(errno));

	/* Create a socket pair for communicating with ssh. */
	if (pipe(pin) == -1)
		fatal("pipe: %s", strerror(errno));
	if (pipe(pout) == -1)
		fatal("pipe: %s", strerror(errno));

	/* Free the reserved descriptors. */
	close(reserved[0]);
	close(reserved[1]);

	ssh_signal(SIGTSTP, suspchild);
	ssh_signal(SIGTTIN, suspchild);
	ssh_signal(SIGTTOU, suspchild);

	/* Fork a child to execute the command on the remote host using ssh. */
	*pid = fork();
	if (*pid == 0) {
		/* Child. */
		close(pin[1]);
		close(pout[0]);
		dup2(pin[0], 0);
		dup2(pout[1], 1);
		close(pin[0]);
		close(pout[1]);

		replacearg(&args, 0, "%s", program);
		if (port != -1) {
			addargs(&args, "-p");
			addargs(&args, "%d", port);
		}
		if (remuser != NULL) {
			addargs(&args, "-l");
			addargs(&args, "%s", remuser);
		}
		if (subsystem)
			addargs(&args, "-s");
		addargs(&args, "--");
		addargs(&args, "%s", host);
		addargs(&args, "%s", cmd);

		execvp(program, args.list);
		perror(program);
		exit(1);
	} else if (*pid == -1) {
		fatal("fork: %s", strerror(errno));
	}
	/* Parent.  Close the other side, and return the local side. */
	close(pin[0]);
	*fdout = pin[1];
	close(pout[1]);
	*fdin = pout[0];
	ssh_signal(SIGTERM, killchild);
	ssh_signal(SIGINT, killchild);
	ssh_signal(SIGHUP, killchild);
	return 0;
}

/*
 * This function executes a command similar to do_cmd(), but expects the
 * input and output descriptors to be setup by a previous call to do_cmd().
 * This way the input and output of two commands can be connected.
 */
int
do_cmd2(char *host, char *remuser, int port, char *cmd,
    int fdin, int fdout)
{
	int status;
	pid_t pid;

	if (verbose_mode)
		fmprintf(stderr,
		    "Executing: 2nd program %s host %s, user %s, command %s\n",
		    ssh_program, host,
		    remuser ? remuser : "(unspecified)", cmd);

	if (port == -1)
		port = sshport;

	/* Fork a child to execute the command on the remote host using ssh. */
	pid = fork();
	if (pid == 0) {
		dup2(fdin, 0);
		dup2(fdout, 1);

		replacearg(&args, 0, "%s", ssh_program);
		if (port != -1) {
			addargs(&args, "-p");
			addargs(&args, "%d", port);
		}
		if (remuser != NULL) {
			addargs(&args, "-l");
			addargs(&args, "%s", remuser);
		}
		addargs(&args, "-oBatchMode=yes");
		addargs(&args, "--");
		addargs(&args, "%s", host);
		addargs(&args, "%s", cmd);

		execvp(ssh_program, args.list);
		perror(ssh_program);
		exit(1);
	} else if (pid == -1) {
		fatal("fork: %s", strerror(errno));
	}
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			fatal("do_cmd2: waitpid: %s", strerror(errno));
	return 0;
}

typedef struct {
	size_t cnt;
	char *buf;
} BUF;

BUF *allocbuf(BUF *, int, int);
void lostconn(int);
int okname(char *);
void run_err(const char *,...)
    __attribute__((__format__ (printf, 1, 2)))
    __attribute__((__nonnull__ (1)));
int note_err(const char *,...)
    __attribute__((__format__ (printf, 1, 2)));
void verifydir(char *);

struct passwd *pwd;
uid_t userid;
int errs, remin, remout, remin2, remout2;
int Tflag, pflag, iamremote, iamrecursive, targetshouldbedirectory;

#define	CMDNEEDS	64
char cmd[CMDNEEDS];		/* must hold "rcp -r -p -d\0" */

enum scp_mode_e {
	MODE_SCP,
	MODE_SFTP
};

int response(void);
void rsource(char *, struct stat *);
void sink(int, char *[], const char *);
void source(int, char *[]);
void tolocal(int, char *[], enum scp_mode_e, char *sftp_direct);
void toremote(int, char *[], enum scp_mode_e, char *sftp_direct);
void usage(void);
void calculate_hash(char *, char *, off_t); /*get the hash of file to length*/
void rand_str(char *, size_t); /*gen randome char string */

void source_sftp(int, char *, char *, struct sftp_conn *);
void sink_sftp(int, char *, const char *, struct sftp_conn *);
void throughlocal_sftp(struct sftp_conn *, struct sftp_conn *,
    char *, char *);

int
main(int argc, char **argv)
{
	int ch, fflag, tflag, status, n;
	char **newargv, *argv0;
	const char *errstr;
	extern char *optarg;
	extern int optind;
	enum scp_mode_e mode = MODE_SFTP;
	char *sftp_direct = NULL;

	/* we use this to prepend the debugging statements
	 * so we know which side is saying what */
	gethostname(hostname, HOST_NAME_MAX + 1);

	/* Ensure that fds 0, 1 and 2 are open or directed to /dev/null */
	sanitise_stdfd();

	seed_rng();

	msetlocale();

	/* for use with rand function when resume option is used*/
	srand(time(NULL));

	/* Copy argv, because we modify it */
	argv0 = argv[0];
	newargv = xcalloc(MAXIMUM(argc + 1, 1), sizeof(*newargv));
	for (n = 0; n < argc; n++)
		newargv[n] = xstrdup(argv[n]);
	argv = newargv;

	__progname = ssh_get_progname(argv[0]);

	log_init(argv0, log_level, SYSLOG_FACILITY_USER, 2);

	memset(&args, '\0', sizeof(args));
	memset(&remote_remote_args, '\0', sizeof(remote_remote_args));
	args.list = remote_remote_args.list = NULL;
	addargs(&args, "%s", ssh_program);
	addargs(&args, "-x");
	addargs(&args, "-oPermitLocalCommand=no");
	addargs(&args, "-oClearAllForwardings=yes");
	addargs(&args, "-oRemoteCommand=none");
	addargs(&args, "-oRequestTTY=no");

	fflag = Tflag = tflag = 0;
	while ((ch = getopt(argc, argv,
	    "12346ABCTdfOpqRrstvZz:D:F:J:M:P:S:c:i:l:o:")) != -1) {
		switch (ch) {
		/* User-visible flags. */
		case '1':
			fatal("SSH protocol v.1 is no longer supported");
			break;
		case '2':
			/* Ignored */
			break;
		case 'A':
		case '4':
		case '6':
		case 'C':
			addargs(&args, "-%c", ch);
			addargs(&remote_remote_args, "-%c", ch);
			break;
		case 'D':
			sftp_direct = optarg;
			break;
		case '3':
			throughlocal = 1;
			break;
		case 'R':
			throughlocal = 0;
			break;
		case 'o':
		case 'c':
		case 'i':
		case 'F':
		case 'J':
			addargs(&remote_remote_args, "-%c", ch);
			addargs(&remote_remote_args, "%s", optarg);
			addargs(&args, "-%c", ch);
			addargs(&args, "%s", optarg);
			break;
		case 'O':
			mode = MODE_SCP;
			break;
		case 's':
			mode = MODE_SFTP;
			break;
		case 'P':
			sshport = a2port(optarg);
			if (sshport <= 0)
				fatal("bad port \"%s\"\n", optarg);
			break;
		case 'B':
			addargs(&remote_remote_args, "-oBatchmode=yes");
			addargs(&args, "-oBatchmode=yes");
			break;
		case 'l':
			limit_kbps = strtonum(optarg, 1, 100 * 1024 * 1024,
			    &errstr);
			if (errstr != NULL)
				usage();
			limit_kbps *= 1024; /* kbps */
			bandwidth_limit_init(&bwlimit, limit_kbps, COPY_BUFLEN);
			break;
		case 'p':
			pflag = 1;
			break;
		case 'r':
			iamrecursive = 1;
			break;
		case 'S':
			ssh_program = xstrdup(optarg);
			break;
		case 'z':
			remote_path = xstrdup(optarg);
			break;
		case 'v':
			addargs(&args, "-v");
			addargs(&remote_remote_args, "-v");
			if (verbose_mode == 0)
				log_level = SYSLOG_LEVEL_DEBUG1;
			else if (log_level < SYSLOG_LEVEL_DEBUG3)
				log_level++;
			verbose_mode = 1;
			break;
		case 'q':
			addargs(&args, "-q");
			addargs(&remote_remote_args, "-q");
			showprogress = 0;
			break;
#ifdef WITH_OPENSSL			
		case 'Z':
			resume_flag = 1;
			break;
#endif
		/* Server options. */
		case 'd':
			targetshouldbedirectory = 1;
			break;
		case 'f':	/* "from" */
			iamremote = 1;
			fflag = 1;
			break;
		case 't':	/* "to" */
			iamremote = 1;
			tflag = 1;
#ifdef HAVE_CYGWIN
			setmode(0, O_BINARY);
#endif
			break;
		case 'T':
			Tflag = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	log_init(argv0, log_level, SYSLOG_FACILITY_USER, 2);

	/* Do this last because we want the user to be able to override it */
	addargs(&args, "-oForwardAgent=no");

	if (iamremote)
		mode = MODE_SCP;

	if ((pwd = getpwuid(userid = getuid())) == NULL)
		fatal("unknown user %u", (u_int) userid);

	if (!isatty(STDOUT_FILENO))
		showprogress = 0;

	if (pflag) {
		/* Cannot pledge: -p allows setuid/setgid files... */
	} else {
		if (pledge("stdio rpath wpath cpath fattr tty proc exec",
		    NULL) == -1) {
			perror("pledge");
			exit(1);
		}
	}

	remin = STDIN_FILENO;
	remout = STDOUT_FILENO;

	if (fflag) {
		/* Follow "protocol", send data. */
		(void) response();
		source(argc, argv);
		exit(errs != 0);
	}
	if (tflag) {
		/* Receive data. */
		sink(argc, argv, NULL);
		exit(errs != 0);
	}
	if (argc < 2)
		usage();
	if (argc > 2)
		targetshouldbedirectory = 1;

	remin = remout = -1;
	do_cmd_pid = -1;
	/* Command to be executed on remote system using "ssh". */
	/* the command uses the first scp in the users
	 * path. This isn't necessarily going to be the 'right' scp to use.
	 * So the current solution is to give the user the option of
	 * entering the path to correct scp. If they don't then it defaults
	 * to whatever scp is first in their path -cjr */
	/* TODO: Rethink this in light renaming the binaries */
	
	(void) snprintf(cmd, sizeof cmd, "%s%s%s%s%s%s",
			remote_path ? remote_path : "scp",
			verbose_mode ? " -v" : "",
			iamrecursive ? " -r" : "",
			pflag ? " -p" : "",
			targetshouldbedirectory ? " -d" : "",
			resume_flag ? " -Z" : "");
#ifdef DEBUG
		fprintf(stderr, "%s: Sending cmd %s\n", hostname, cmd);
#endif
	(void) ssh_signal(SIGPIPE, lostconn);

	if (colon(argv[argc - 1]))	/* Dest is remote host. */
		toremote(argc, argv, mode, sftp_direct);
	else {
		if (targetshouldbedirectory)
			verifydir(argv[argc - 1]);
		tolocal(argc, argv, mode, sftp_direct);	/* Dest is local host. */
	}

	/*
	 * Finally check the exit status of the ssh process, if one was forked
	 * and no error has occurred yet
	 */
	if (do_cmd_pid != -1 && (mode == MODE_SFTP || errs == 0)) {
		if (remin != -1)
		    (void) close(remin);
		if (remout != -1)
		    (void) close(remout);
		if (waitpid(do_cmd_pid, &status, 0) == -1)
			errs = 1;
		else {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				errs = 1;
		}
	}
	exit(errs != 0);
}

/* Callback from atomicio6 to update progress meter and limit bandwidth */
static int
scpio(void *_cnt, size_t s)
{
	off_t *cnt = (off_t *)_cnt;

	*cnt += s;
	refresh_progress_meter(0);
	if (limit_kbps > 0)
		bandwidth_limit(&bwlimit, s);
	return 0;
}

static int
do_times(int fd, int verb, const struct stat *sb)
{
	/* strlen(2^64) == 20; strlen(10^6) == 7 */
	char buf[(20 + 7 + 2) * 2 + 2];

	(void)snprintf(buf, sizeof(buf), "T%llu 0 %llu 0\n",
	    (unsigned long long) (sb->st_mtime < 0 ? 0 : sb->st_mtime),
	    (unsigned long long) (sb->st_atime < 0 ? 0 : sb->st_atime));
	if (verb) {
		fprintf(stderr, "File mtime %lld atime %lld\n",
		    (long long)sb->st_mtime, (long long)sb->st_atime);
		fprintf(stderr, "Sending file timestamps: %s", buf);
	}
	(void) atomicio(vwrite, fd, buf, strlen(buf));
	return (response());
}

static int
parse_scp_uri(const char *uri, char **userp, char **hostp, int *portp,
    char **pathp)
{
	int r;

	r = parse_uri("scp", uri, userp, hostp, portp, pathp);
	if (r == 0 && *pathp == NULL)
		*pathp = xstrdup(".");
	return r;
}

/* Appends a string to an array; returns 0 on success, -1 on alloc failure */
static int
append(char *cp, char ***ap, size_t *np)
{
	char **tmp;

	if ((tmp = reallocarray(*ap, *np + 1, sizeof(*tmp))) == NULL)
		return -1;
	tmp[(*np)] = cp;
	(*np)++;
	*ap = tmp;
	return 0;
}

/*
 * Finds the start and end of the first brace pair in the pattern.
 * returns 0 on success or -1 for invalid patterns.
 */
static int
find_brace(const char *pattern, int *startp, int *endp)
{
	int i;
	int in_bracket, brace_level;

	*startp = *endp = -1;
	in_bracket = brace_level = 0;
	for (i = 0; i < INT_MAX && *endp < 0 && pattern[i] != '\0'; i++) {
		switch (pattern[i]) {
		case '\\':
			/* skip next character */
			if (pattern[i + 1] != '\0')
				i++;
			break;
		case '[':
			in_bracket = 1;
			break;
		case ']':
			in_bracket = 0;
			break;
		case '{':
			if (in_bracket)
				break;
			if (pattern[i + 1] == '}') {
				/* Protect a single {}, for find(1), like csh */
				i++; /* skip */
				break;
			}
			if (*startp == -1)
				*startp = i;
			brace_level++;
			break;
		case '}':
			if (in_bracket)
				break;
			if (*startp < 0) {
				/* Unbalanced brace */
				return -1;
			}
			if (--brace_level <= 0)
				*endp = i;
			break;
		}
	}
	/* unbalanced brackets/braces */
	if (*endp < 0 && (*startp >= 0 || in_bracket))
		return -1;
	return 0;
}

/*
 * Assembles and records a successfully-expanded pattern, returns -1 on
 * alloc failure.
 */
static int
emit_expansion(const char *pattern, int brace_start, int brace_end,
    int sel_start, int sel_end, char ***patternsp, size_t *npatternsp)
{
	char *cp;
	int o = 0, tail_len = strlen(pattern + brace_end + 1);

	if ((cp = malloc(brace_start + (sel_end - sel_start) +
	    tail_len + 1)) == NULL)
		return -1;

	/* Pattern before initial brace */
	if (brace_start > 0) {
		memcpy(cp, pattern, brace_start);
		o = brace_start;
	}
	/* Current braced selection */
	if (sel_end - sel_start > 0) {
		memcpy(cp + o, pattern + sel_start,
		    sel_end - sel_start);
		o += sel_end - sel_start;
	}
	/* Remainder of pattern after closing brace */
	if (tail_len > 0) {
		memcpy(cp + o, pattern + brace_end + 1, tail_len);
		o += tail_len;
	}
	cp[o] = '\0';
	if (append(cp, patternsp, npatternsp) != 0) {
		free(cp);
		return -1;
	}
	return 0;
}

/*
 * Expand the first encountered brace in pattern, appending the expanded
 * patterns it yielded to the *patternsp array.
 *
 * Returns 0 on success or -1 on allocation failure.
 *
 * Signals whether expansion was performed via *expanded and whether
 * pattern was invalid via *invalid.
 */
static int
brace_expand_one(const char *pattern, char ***patternsp, size_t *npatternsp,
    int *expanded, int *invalid)
{
	int i;
	int in_bracket, brace_start, brace_end, brace_level;
	int sel_start, sel_end;

	*invalid = *expanded = 0;

	if (find_brace(pattern, &brace_start, &brace_end) != 0) {
		*invalid = 1;
		return 0;
	} else if (brace_start == -1)
		return 0;

	in_bracket = brace_level = 0;
	for (i = sel_start = brace_start + 1; i < brace_end; i++) {
		switch (pattern[i]) {
		case '{':
			if (in_bracket)
				break;
			brace_level++;
			break;
		case '}':
			if (in_bracket)
				break;
			brace_level--;
			break;
		case '[':
			in_bracket = 1;
			break;
		case ']':
			in_bracket = 0;
			break;
		case '\\':
			if (i < brace_end - 1)
				i++; /* skip */
			break;
		}
		if (pattern[i] == ',' || i == brace_end - 1) {
			if (in_bracket || brace_level > 0)
				continue;
			/* End of a selection, emit an expanded pattern */

			/* Adjust end index for last selection */
			sel_end = (i == brace_end - 1) ? brace_end : i;
			if (emit_expansion(pattern, brace_start, brace_end,
			    sel_start, sel_end, patternsp, npatternsp) != 0)
				return -1;
			/* move on to the next selection */
			sel_start = i + 1;
			continue;
		}
	}
	if (in_bracket || brace_level > 0) {
		*invalid = 1;
		return 0;
	}
	/* success */
	*expanded = 1;
	return 0;
}

/* Expand braces from pattern. Returns 0 on success, -1 on failure */
static int
brace_expand(const char *pattern, char ***patternsp, size_t *npatternsp)
{
	char *cp, *cp2, **active = NULL, **done = NULL;
	size_t i, nactive = 0, ndone = 0;
	int ret = -1, invalid = 0, expanded = 0;

	*patternsp = NULL;
	*npatternsp = 0;

	/* Start the worklist with the original pattern */
	if ((cp = strdup(pattern)) == NULL)
		return -1;
	if (append(cp, &active, &nactive) != 0) {
		free(cp);
		return -1;
	}
	while (nactive > 0) {
		cp = active[nactive - 1];
		nactive--;
		if (brace_expand_one(cp, &active, &nactive,
		    &expanded, &invalid) == -1) {
			free(cp);
			goto fail;
		}
		if (invalid)
			fatal_f("invalid brace pattern \"%s\"", cp);
		if (expanded) {
			/*
			 * Current entry expanded to new entries on the
			 * active list; discard the progenitor pattern.
			 */
			free(cp);
			continue;
		}
		/*
		 * Pattern did not expand; append the finename component to
		 * the completed list
		 */
		if ((cp2 = strrchr(cp, '/')) != NULL)
			*cp2++ = '\0';
		else
			cp2 = cp;
		if (append(xstrdup(cp2), &done, &ndone) != 0) {
			free(cp);
			goto fail;
		}
		free(cp);
	}
	/* success */
	*patternsp = done;
	*npatternsp = ndone;
	done = NULL;
	ndone = 0;
	ret = 0;
 fail:
	for (i = 0; i < nactive; i++)
		free(active[i]);
	free(active);
	for (i = 0; i < ndone; i++)
		free(done[i]);
	free(done);
	return ret;
}

static struct sftp_conn *
do_sftp_connect(char *host, char *user, int port, char *sftp_direct,
   int *reminp, int *remoutp, int *pidp)
{
	if (sftp_direct == NULL) {
		if (do_cmd(ssh_program, host, user, port, 1, "sftp",
		    reminp, remoutp, pidp) < 0)
			return NULL;

	} else {
		freeargs(&args);
		addargs(&args, "sftp-server");
		if (do_cmd(sftp_direct, host, NULL, -1, 0, "sftp",
		    reminp, remoutp, pidp) < 0)
			return NULL;
	}
	return do_init(*reminp, *remoutp, 32768, 64, limit_kbps);
}

void
toremote(int argc, char **argv, enum scp_mode_e mode, char *sftp_direct)
{
	char *suser = NULL, *host = NULL, *src = NULL;
	char *bp, *tuser, *thost, *targ;
	int sport = -1, tport = -1;
	struct sftp_conn *conn = NULL, *conn2 = NULL;
	arglist alist;
	int i, r, status;
	u_int j;

	memset(&alist, '\0', sizeof(alist));
	alist.list = NULL;

	/* Parse target */
	r = parse_scp_uri(argv[argc - 1], &tuser, &thost, &tport, &targ);
	if (r == -1) {
		fmprintf(stderr, "%s: invalid uri\n", argv[argc - 1]);
		++errs;
		goto out;
	}
	if (r != 0) {
		if (parse_user_host_path(argv[argc - 1], &tuser, &thost,
		    &targ) == -1) {
			fmprintf(stderr, "%s: invalid target\n", argv[argc - 1]);
			++errs;
			goto out;
		}
	}

	/* Parse source files */
	for (i = 0; i < argc - 1; i++) {
		free(suser);
		free(host);
		free(src);
		r = parse_scp_uri(argv[i], &suser, &host, &sport, &src);
		if (r == -1) {
			fmprintf(stderr, "%s: invalid uri\n", argv[i]);
			++errs;
			continue;
		}
		if (r != 0) {
			parse_user_host_path(argv[i], &suser, &host, &src);
		}
		if (suser != NULL && !okname(suser)) {
			++errs;
			continue;
		}
		if (host && throughlocal) {	/* extended remote to remote */
			if (mode == MODE_SFTP) {
				if (remin == -1) {
					/* Connect to dest now */
					conn = do_sftp_connect(thost, tuser,
					    tport, sftp_direct,
					    &remin, &remout, &do_cmd_pid);
					if (conn == NULL) {
						fatal("Unable to open "
						    "destination connection");
					}
					debug3_f("origin in %d out %d pid %ld",
					    remin, remout, (long)do_cmd_pid);
				}
				/*
				 * XXX remember suser/host/sport and only
				 * reconnect if they change between arguments.
				 * would save reconnections for cases like
				 * scp -3 hosta:/foo hosta:/bar hostb:
				 */
				/* Connect to origin now */
				conn2 = do_sftp_connect(host, suser,
				    sport, sftp_direct,
				    &remin2, &remout2, &do_cmd_pid2);
				if (conn2 == NULL) {
					fatal("Unable to open "
					    "source connection");
				}
				debug3_f("destination in %d out %d pid %ld",
				    remin2, remout2, (long)do_cmd_pid2);
				throughlocal_sftp(conn2, conn, src, targ);
				(void) close(remin2);
				(void) close(remout2);
				remin2 = remout2 = -1;
				if (waitpid(do_cmd_pid2, &status, 0) == -1)
					++errs;
				else if (!WIFEXITED(status) ||
				    WEXITSTATUS(status) != 0)
					++errs;
				do_cmd_pid2 = -1;
				continue;
			} else {
				xasprintf(&bp, "%s -f %s%s", cmd,
				    *src == '-' ? "-- " : "", src);
				if (do_cmd(ssh_program, host, suser, sport, 0,
				    bp, &remin, &remout, &do_cmd_pid) < 0)
					exit(1);
				free(bp);
				xasprintf(&bp, "%s -t %s%s", cmd,
				    *targ == '-' ? "-- " : "", targ);
				if (do_cmd2(thost, tuser, tport, bp,
				    remin, remout) < 0)
					exit(1);
				free(bp);
				(void) close(remin);
				(void) close(remout);
				remin = remout = -1;
			}
		} else if (host) {	/* standard remote to remote */
			/*
			 * Second remote user is passed to first remote side
			 * via scp command-line. Ensure it contains no obvious
			 * shell characters.
			 */
			if (tuser != NULL && !okname(tuser)) {
				++errs;
				continue;
			}
			if (tport != -1 && tport != SSH_DEFAULT_PORT) {
				/* This would require the remote support URIs */
				fatal("target port not supported with two "
				    "remote hosts and the -R option");
			}

			freeargs(&alist);
			addargs(&alist, "%s", ssh_program);
			addargs(&alist, "-x");
			addargs(&alist, "-oClearAllForwardings=yes");
			addargs(&alist, "-n");
			for (j = 0; j < remote_remote_args.num; j++) {
				addargs(&alist, "%s",
				    remote_remote_args.list[j]);
			}

			if (sport != -1) {
				addargs(&alist, "-p");
				addargs(&alist, "%d", sport);
			}
			if (suser) {
				addargs(&alist, "-l");
				addargs(&alist, "%s", suser);
			}
			addargs(&alist, "--");
			addargs(&alist, "%s", host);
			addargs(&alist, "%s", cmd);
			addargs(&alist, "%s", src);
			addargs(&alist, "%s%s%s:%s",
			    tuser ? tuser : "", tuser ? "@" : "",
			    thost, targ);
			if (do_local_cmd(&alist) != 0)
				errs = 1;
		} else {	/* local to remote */
			if (mode == MODE_SFTP) {
				if (remin == -1) {
					/* Connect to remote now */
					conn = do_sftp_connect(thost, tuser,
					    tport, sftp_direct,
					    &remin, &remout, &do_cmd_pid);
					if (conn == NULL) {
						fatal("Unable to open sftp "
						    "connection");
					}
				}

				/* The protocol */
				source_sftp(1, argv[i], targ, conn);
				continue;
			}
			/* SCP */
			if (remin == -1) {
				xasprintf(&bp, "%s -t %s%s", cmd,
				    *targ == '-' ? "-- " : "", targ);
				if (do_cmd(ssh_program, thost, tuser, tport, 0,
				    bp, &remin, &remout, &do_cmd_pid) < 0)
					exit(1);
				if (response() < 0)
					exit(1);
				free(bp);
			}
			source(1, argv + i);
		}
	}
out:
	if (mode == MODE_SFTP)
		free(conn);
	free(tuser);
	free(thost);
	free(targ);
	free(suser);
	free(host);
	free(src);
}

void
tolocal(int argc, char **argv, enum scp_mode_e mode, char *sftp_direct)
{
	char *bp, *host = NULL, *src = NULL, *suser = NULL;
	arglist alist;
	struct sftp_conn *conn = NULL;
	int i, r, sport = -1;

	memset(&alist, '\0', sizeof(alist));
	alist.list = NULL;

	for (i = 0; i < argc - 1; i++) {
		free(suser);
		free(host);
		free(src);
		r = parse_scp_uri(argv[i], &suser, &host, &sport, &src);
		if (r == -1) {
			fmprintf(stderr, "%s: invalid uri\n", argv[i]);
			++errs;
			continue;
		}
		if (r != 0)
			parse_user_host_path(argv[i], &suser, &host, &src);
		if (suser != NULL && !okname(suser)) {
			++errs;
			continue;
		}
		if (!host) {	/* Local to local. */
			freeargs(&alist);
			addargs(&alist, "%s", _PATH_CP);
			if (iamrecursive)
				addargs(&alist, "-r");
			if (pflag)
				addargs(&alist, "-p");
			addargs(&alist, "--");
			addargs(&alist, "%s", argv[i]);
			addargs(&alist, "%s", argv[argc-1]);
			if (do_local_cmd(&alist))
				++errs;
			continue;
		}
		/* Remote to local. */
		if (mode == MODE_SFTP) {
			conn = do_sftp_connect(host, suser, sport,
			    sftp_direct, &remin, &remout, &do_cmd_pid);
			if (conn == NULL) {
				error("sftp connection failed");
				++errs;
				continue;
			}

			/* The protocol */
			sink_sftp(1, argv[argc - 1], src, conn);

			free(conn);
			(void) close(remin);
			(void) close(remout);
			remin = remout = -1;
			continue;
		}
		/* SCP */
		xasprintf(&bp, "%s -f %s%s",
		    cmd, *src == '-' ? "-- " : "", src);
		if (do_cmd(ssh_program, host, suser, sport, 0, bp,
		    &remin, &remout, &do_cmd_pid) < 0) {
			free(bp);
			++errs;
			continue;
		}
		free(bp);
		sink(1, argv + argc - 1, src);
		(void) close(remin);
		remin = remout = -1;
	}
	free(suser);
	free(host);
	free(src);
}

/* calculate the hash of a file up to length bytes
 * this is used to determine if remote and local file
 * fragments match. There may be a more efficient process for the hashing 
 * TODO: I'd like to XXHash for the hashing but that requires that both
 * ends have xxhash installed and then dealing with fallbacks */
#ifdef WITH_OPENSSL
void calculate_hash(char *filename, char *output, off_t length)
{
	int n, md_len;
	EVP_MD_CTX *c;
	const EVP_MD *md;
	char buf[HASH_BUFLEN];
	ssize_t bytes;
	unsigned char out[EVP_MAX_MD_SIZE];
	char tmp[3];
	FILE *file_ptr;
	*output = '\0';

	/* open file for calculating hash */
	file_ptr = fopen(filename, "r");
	if (file_ptr==NULL)
	{
		if (verbose_mode) {
			fprintf(stderr, "%s: error opening file %s\n", hostname, filename);
			/* file the expected output with spaces */
			snprintf(output, HASH_LEN, "%s",  " ");
		}
		return;
	}

	md = EVP_get_digestbyname("blake2b512");
	c = EVP_MD_CTX_new();
	EVP_DigestInit_ex(c, md, NULL);

	while (length > 0) {
		if (length > HASH_BUFLEN)
			/* fread returns the number of elements read. 
			 * in this case 1. Multiply by the length to get the bytes */
			bytes=fread(buf, HASH_BUFLEN, 1, file_ptr) * HASH_BUFLEN;
		else
			bytes=fread(buf, length, 1, file_ptr) * length;
		EVP_DigestUpdate(c, buf, bytes);
		length -= HASH_BUFLEN;
	}
	EVP_DigestFinal(c, out, &md_len);
	EVP_MD_CTX_free(c);
	/* convert the hash into a string */
	for(n=0; n < md_len; n++) {
		snprintf(tmp, 3, "%02x", out[n]);
		strncat(output, tmp, 3);
	}
#ifdef DEBUG
	fprintf(stderr, "%s: HASH IS '%s' of length %ld\n", hostname, output, strlen(output));
#endif
	fclose(file_ptr);
}
#else
void calculate_hash(char *filename, char *output, off_t length)
{
  /* empty function for builds without openssl */
}
#endif /* WITH_OPENSSL */

#define TYPE_OVERFLOW(type, val) \
	((sizeof(type) == 4 && (val) > INT32_MAX) || \
	 (sizeof(type) == 8 && (val) > INT64_MAX) || \
	 (sizeof(type) != 4 && sizeof(type) != 8))

/* Prepare remote path, handling ~ by assuming cwd is the homedir */
static char *
prepare_remote_path(struct sftp_conn *conn, const char *path)
{
	size_t nslash;

	/* Handle ~ prefixed paths */
	if (*path == '\0' || strcmp(path, "~") == 0)
		return xstrdup(".");
	if (*path != '~')
		return xstrdup(path);
	if (strncmp(path, "~/", 2) == 0) {
		if ((nslash = strspn(path + 2, "/")) == strlen(path + 2))
			return xstrdup(".");
		return xstrdup(path + 2 + nslash);
	}
	if (can_expand_path(conn))
		return do_expand_path(conn, path);
	/* No protocol extension */
	error("server expand-path extension is required "
	    "for ~user paths in SFTP mode");
	return NULL;
}

void
source_sftp(int argc, char *src, char *targ, struct sftp_conn *conn)
{
	char *target = NULL, *filename = NULL, *abs_dst = NULL;
	int src_is_dir, target_is_dir;
	Attrib a;
	struct stat st;

	memset(&a, '\0', sizeof(a));
	if (stat(src, &st) != 0)
		fatal("stat local \"%s\": %s", src, strerror(errno));
	src_is_dir = S_ISDIR(st.st_mode);
	if ((filename = basename(src)) == NULL)
		fatal("basename \"%s\": %s", src, strerror(errno));

	/*
	 * No need to glob here - the local shell already took care of
	 * the expansions
	 */
	if ((target = prepare_remote_path(conn, targ)) == NULL)
		cleanup_exit(255);
	target_is_dir = remote_is_dir(conn, target);
	if (targetshouldbedirectory && !target_is_dir) {
		debug("target directory \"%s\" does not exist", target);
		a.flags = SSH2_FILEXFER_ATTR_PERMISSIONS;
		a.perm = st.st_mode | 0700; /* ensure writable */
		if (do_mkdir(conn, target, &a, 1) != 0)
			cleanup_exit(255); /* error already logged */
		target_is_dir = 1;
	}
	if (target_is_dir)
		abs_dst = path_append(target, filename);
	else {
		abs_dst = target;
		target = NULL;
	}
	debug3_f("copying local %s to remote %s", src, abs_dst);

	if (src_is_dir && iamrecursive) {
		if (upload_dir(conn, src, abs_dst, pflag,
		    SFTP_PROGRESS_ONLY, 0, 0, 1) != 0) {
			error("failed to upload directory %s to %s", src, targ);
			errs = 1;
		}
	} else if (do_upload(conn, src, abs_dst, pflag, 0, 0) != 0) {
		error("failed to upload file %s to %s", src, targ);
		errs = 1;
	}

	free(abs_dst);
	free(target);
}

void
source(int argc, char **argv)
{
	struct stat stb;
	static BUF buffer;
	BUF *bp;
	off_t i, statbytes, xfer_size;
	size_t amt, nr;
	int fd = -1, haderr, indx;
	char *cp, *last, *name, buf[PATH_MAX + BUF_AND_HASH], encname[PATH_MAX];
	int len;
	char hashsum[HASH_LEN], test_hashsum[HASH_LEN];
	char inbuf[PATH_MAX + BUF_AND_HASH];
	size_t insize;
	unsigned long long ull;
	char *match; /* used to communicate fragment match */
	match = "\0"; /*default is to fail the match. NULL and F both indicate fail*/

	for (indx = 0; indx < argc; ++indx) {
		name = argv[indx];
#ifdef DEBUG
		fprintf(stderr, "%s index is %d, name is %s\n", hostname, indx, name);
#endif
		statbytes = 0;
		len = strlen(name);
		while (len > 1 && name[len-1] == '/')
			name[--len] = '\0';
		if ((fd = open(name, O_RDONLY|O_NONBLOCK)) == -1)
			goto syserr;
		if (strchr(name, '\n') != NULL) {
			strnvis(encname, name, sizeof(encname), VIS_NL);
			name = encname;
		}
		if (fstat(fd, &stb) == -1) {
syserr:			run_err("%s: %s", name, strerror(errno));
			goto next;
		}
		if (stb.st_size < 0) {
			run_err("%s: %s", name, "Negative file size");
			goto next;
		}
		unset_nonblock(fd);
		switch (stb.st_mode & S_IFMT) {
		case S_IFREG:
			/* only calculate hash if we are in resume mode and a file*/
			if (resume_flag) {
				calculate_hash(name, hashsum, stb.st_size);
#ifdef DEBUG
				fprintf(stderr, "%s: Name is '%s' and hash '%s'\n", hostname, name, hashsum);
				fprintf (stderr,"%s: size of %s is %ld\n", hostname, name, stb.st_size);
#endif
			}
			break;
		case S_IFDIR:
			if (iamrecursive) {
				rsource(name, &stb);
				goto next;
			}
			/* FALLTHROUGH */
		default:
			run_err("%s: not a regular file", name);
			goto next;
		}
		if ((last = strrchr(name, '/')) == NULL)
			last = name;
		else
			++last;
		curfile = last;
		if (pflag) {
			if (do_times(remout, verbose_mode, &stb) < 0)
				goto next;
		}
#define	FILEMODEMASK	(S_ISUID|S_ISGID|S_IRWXU|S_IRWXG|S_IRWXO)
		/* Add a hash of the file along with the filemode if in resume */
		if (resume_flag)
			snprintf(buf, sizeof buf, "C%04o %lld %s %s\n",
				 (u_int) (stb.st_mode & FILEMODEMASK),
				 (long long)stb.st_size, hashsum, last);
		else
			snprintf(buf, sizeof buf, "C%04o %lld %s\n",
				 (u_int) (stb.st_mode & FILEMODEMASK),
				 (long long)stb.st_size, last);

#ifdef DEBUG
		fprintf(stderr, "%s: Sending file modes: %s", hostname, buf);
#endif
		(void) atomicio(vwrite, remout, buf, strlen(buf));

#ifdef DEBUG
		fprintf(stderr, "%s: inbuf length %ld buf length %ld\n", hostname, strlen(inbuf), strlen(buf));
#endif
		if (resume_flag) { /* get the hash response from the remote */
			(void) atomicio(read, remin, inbuf, BUF_AND_HASH - 1);
#ifdef DEBUG
				fprintf(stderr, "%s: we got '%s' in inbuf length %ld buf was %ld\n",
					hostname, inbuf, strlen(inbuf), strlen(buf));
#endif
		}
		if (response() < 0) {
#ifdef DEBUG
			fprintf(stderr, "%s: response is less than 0\n", hostname);
#endif
			goto next;
		}
		xfer_size = stb.st_size;

		/* we only do the following in resume mode because we have a
		 * new buf from the remote to parse */
		if (resume_flag) {
			cp = inbuf;
			if (*cp == 'R') { /* resume file transfer*/
				char *in_hashsum; /* where to hold the incoming hash */
				in_hashsum = calloc(HASH_LEN+1, sizeof(char));
				for (++cp; cp < inbuf + 5; cp++) {
					/* skip over the mode */
				}
				if (*cp++ != ' ') {
					fprintf(stderr, "%s: mode not delineated!\n", hostname);
				}

				if (!isdigit((unsigned char)*cp))
					fprintf(stderr, "%s: size not present\n", hostname);
				ull = strtoull(cp, &cp, 10);
				if (!cp || *cp++ != ' ')
					fprintf(stderr, "%s: size not delimited\n", hostname);
				if (TYPE_OVERFLOW(off_t, ull))
					fprintf(stderr, "%s: size out of range\n", hostname);
				insize = (off_t)ull;

#ifdef DEBUG
				fprintf (stderr, "%s: received size of %ld\n", hostname, insize);
#endif

				/* copy the cp pointer byte by byte */
				int i;
				for (i = 0; i < HASH_LEN; i++) {
					strncat(in_hashsum, cp++, 1);
				}
#ifdef DEBUG
				fprintf (stderr, "%s: in_hashsum '%s'\n", hostname, in_hashsum);
#endif

				/*get the hash of the source file to the byte length we just got*/
				calculate_hash(name, test_hashsum, insize);
#ifdef DEBUG
				fprintf(stderr, "%s: calculated hashsum of local %s to %ld is %s\n",
					hostname, last, insize, test_hashsum);
#endif
				/* compare the incoming hash to the hash of the local file*/
				if (strcmp(in_hashsum, test_hashsum) == 0) {
					/* the fragments match so we should seek to the appropriate place in the
					 * local file and set the remote file to append */
#ifdef DEBUG
					fprintf(stderr, "%s: File fragments match\n", hostname);
					fprintf(stderr, "%s: seeking to %ld\n", hostname, insize);
#endif
					xfer_size = stb.st_size - insize;
#ifdef DEBUG
					fprintf(stderr, "%s: xfer_size: %ld, stb.st_size: %ld insize: %ld\n",
						hostname, xfer_size, stb.st_size, insize);
#endif
					if (lseek(fd, insize, SEEK_CUR) != (off_t)insize) {
#ifdef DEBUG
						fprintf(stderr, "%s: lseek did not return %ld\n", hostname, insize) ;
#endif
						goto next;
					}
					match = "M";
				} else {
					/* the fragments don't match so we should start over from the begining */
#ifdef DEBUG
					fprintf(stderr, "%s: File fragments do not match '%s'(in) '%s'(local)\n",
				       		hostname, in_hashsum, test_hashsum);
#endif
					match = "F";
					xfer_size = stb.st_size;
				}
				free(in_hashsum);
			}
			if (*cp == 'S') { /* skip file */
#ifdef DEBUG
				fprintf(stderr, "%s: Should be skipping this file\n", hostname);
#endif
				goto next;
			}
			if (*cp == 'C') { /*transfer entire file*/
#ifdef DEBUG
				fprintf(stderr, "%s: Resending entire file\n", hostname);
#endif
				xfer_size = stb.st_size;
			}
			/* need to send the match status
			 * We always send the match status or we get out of sync
			 */
#ifdef DEBUG
			fprintf(stderr, "%s: sending match %s\n", hostname, match);
#endif
			(void) atomicio(vwrite, remout, match, 1);
		}

		if ((bp = allocbuf(&buffer, fd, COPY_BUFLEN)) == NULL) {
next:			if (fd != -1) {
				(void) close(fd);
				fd = -1;
			}
			continue;
		}

#ifdef DEBUG
		fprintf(stderr, "%s: going to xfer %ld\n", hostname, xfer_size);
#endif
		if (showprogress) {
			start_progress_meter(curfile, xfer_size, &statbytes);
		}
		set_nonblock(remout);
		for (haderr = i = 0; i < xfer_size; i += bp->cnt) {
			amt = bp->cnt;
			if (i + (off_t)amt > xfer_size)
				amt = xfer_size - i;
			if (!haderr) {
				if ((nr = atomicio(read, fd, bp->buf, amt)) != amt) {
					haderr = errno;
					memset(bp->buf + nr, 0, amt - nr);
				}
			}
			/* Keep writing after error to retain sync */
			if (haderr) {
				(void)atomicio(vwrite, remout, bp->buf, amt);
				memset(bp->buf, 0, amt);
				continue;
			}
			if (atomicio6(vwrite, remout, bp->buf, amt, scpio,
			    &statbytes) != amt)
				haderr = errno;
		}
		unset_nonblock(remout);

		if (fd != -1) {
			if (close(fd) == -1 && !haderr)
				haderr = errno;
			fd = -1;
		}
		if (!haderr)
			(void) atomicio(vwrite, remout, "", 1);
		else
			run_err("%s: %s", name, strerror(haderr));
		(void) response();
		if (showprogress)
			stop_progress_meter();
	}
}

void
rsource(char *name, struct stat *statp)
{
	DIR *dirp;
	struct dirent *dp;
	char *last, *vect[1], path[PATH_MAX];

	if (!(dirp = opendir(name))) {
		run_err("%s: %s", name, strerror(errno));
		return;
	}
	last = strrchr(name, '/');
	if (last == NULL)
		last = name;
	else
		last++;
	if (pflag) {
		if (do_times(remout, verbose_mode, statp) < 0) {
			closedir(dirp);
			return;
		}
	}
	(void) snprintf(path, sizeof path, "D%04o %d %.1024s\n",
	    (u_int) (statp->st_mode & FILEMODEMASK), 0, last);
	if (verbose_mode)
		fmprintf(stderr, "Entering directory: %s", path);
	(void) atomicio(vwrite, remout, path, strlen(path));
	if (response() < 0) {
		closedir(dirp);
		return;
	}
	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_ino == 0)
			continue;
		if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
			continue;
		if (strlen(name) + 1 + strlen(dp->d_name) >= sizeof(path) - 1) {
			run_err("%s/%s: name too long", name, dp->d_name);
			continue;
		}
		(void) snprintf(path, sizeof path, "%s/%s", name, dp->d_name);
		vect[0] = path;
		source(1, vect);
	}
	(void) closedir(dirp);
	(void) atomicio(vwrite, remout, "E\n", 2);
	(void) response();
}

void
sink_sftp(int argc, char *dst, const char *src, struct sftp_conn *conn)
{
	char *abs_src = NULL;
	char *abs_dst = NULL;
	glob_t g;
	char *filename, *tmp = NULL;
	int i, r, err = 0, dst_is_dir;
	struct stat st;

	memset(&g, 0, sizeof(g));

	/*
	 * Here, we need remote glob as SFTP can not depend on remote shell
	 * expansions
	 */
	if ((abs_src = prepare_remote_path(conn, src)) == NULL) {
		err = -1;
		goto out;
	}

	debug3_f("copying remote %s to local %s", abs_src, dst);
	if ((r = remote_glob(conn, abs_src, GLOB_MARK, NULL, &g)) != 0) {
		if (r == GLOB_NOSPACE)
			error("%s: too many glob matches", src);
		else
			error("%s: %s", src, strerror(ENOENT));
		err = -1;
		goto out;
	}

	if ((r = stat(dst, &st)) != 0)
		debug2_f("stat local \"%s\": %s", dst, strerror(errno));
	dst_is_dir = r == 0 && S_ISDIR(st.st_mode);

	if (g.gl_matchc > 1 && !dst_is_dir) {
		if (r == 0) {
			error("Multiple files match pattern, but destination "
			    "\"%s\" is not a directory", dst);
			err = -1;
			goto out;
		}
		debug2_f("creating destination \"%s\"", dst);
		if (mkdir(dst, 0777) != 0) {
			error("local mkdir \"%s\": %s", dst, strerror(errno));
			err = -1;
			goto out;
		}
		dst_is_dir = 1;
	}

	for (i = 0; g.gl_pathv[i] && !interrupted; i++) {
		tmp = xstrdup(g.gl_pathv[i]);
		if ((filename = basename(tmp)) == NULL) {
			error("basename %s: %s", tmp, strerror(errno));
			err = -1;
			goto out;
		}

		if (dst_is_dir)
			abs_dst = path_append(dst, filename);
		else
			abs_dst = xstrdup(dst);

		debug("Fetching %s to %s\n", g.gl_pathv[i], abs_dst);
		if (globpath_is_dir(g.gl_pathv[i]) && iamrecursive) {
			if (download_dir(conn, g.gl_pathv[i], abs_dst, NULL,
			    pflag, SFTP_PROGRESS_ONLY, 0, 0, 1) == -1)
				err = -1;
		} else {
			if (do_download(conn, g.gl_pathv[i], abs_dst, NULL,
			    pflag, 0, 0) == -1)
				err = -1;
		}
		free(abs_dst);
		abs_dst = NULL;
		free(tmp);
		tmp = NULL;
	}

out:
	free(abs_src);
	free(tmp);
	globfree(&g);
	if (err == -1)
		errs = 1;
}


#define TYPE_OVERFLOW(type, val) \
	((sizeof(type) == 4 && (val) > INT32_MAX) || \
	 (sizeof(type) == 8 && (val) > INT64_MAX) || \
	 (sizeof(type) != 4 && sizeof(type) != 8))

void
sink(int argc, char **argv, const char *src)
{
	static BUF buffer;
	struct stat stb, cpstat, npstat;
	BUF *bp;
	off_t i;
	size_t j, count;
	int amt, exists, first, ofd;
	mode_t mode, omode, mask;
	off_t size, statbytes, xfer_size;
	unsigned long long ull;
	int setimes, targisdir, wrerr;
	char ch, *cp, *np, *np_tmp, *targ, *why, *vect[1], buf[16384], visbuf[16384];
	char **patterns = NULL;
	size_t n, npatterns = 0;
	struct timeval tv[2];
	char remote_hashsum[HASH_LEN+1];
	char local_hashsum[HASH_LEN+1];
	char tmpbuf[BUF_AND_HASH];
	char outbuf[BUF_AND_HASH];
	char match;
	int bad_match_flag = 0;
	np = '\0';
	np_tmp = NULL;
	
#define	atime	tv[0]
#define	mtime	tv[1]
#define	SCREWUP(str)	{ why = str; goto screwup; }

#ifdef DEBUG
       fprintf (stderr, "%s: LOCAL In sink with %s\n", hostname, src);
#endif	
	if (TYPE_OVERFLOW(time_t, 0) || TYPE_OVERFLOW(off_t, 0))
		SCREWUP("Unexpected off_t/time_t size");

	setimes = targisdir = 0;
	mask = umask(0);
	if (!pflag)
		(void) umask(mask);
	if (argc != 1) {
		run_err("ambiguous target");
		exit(1);
	}
	targ = *argv;
	if (targetshouldbedirectory)
		verifydir(targ);

#ifdef DEBUG
	fprintf (stderr, "%s: Sending null to remout.\n",hostname);
#endif
	(void) atomicio(vwrite, remout, "", 1);
	if (stat(targ, &stb) == 0 && S_ISDIR(stb.st_mode))
		targisdir = 1;

#ifdef DEBUG
	fprintf(stderr, "%s: Target is %s with a size of %ld\n", hostname, targ, stb.st_size);
#endif
	if (src != NULL && !iamrecursive && !Tflag) {
		/*
		 * Prepare to try to restrict incoming filenames to match
		 * the requested destination file glob.
		 */
		if (brace_expand(src, &patterns, &npatterns) != 0)
			fatal_f("could not expand pattern");
	}

	for (first = 1;; first = 0) {
		bad_match_flag = 0; /* used in resume mode. */
#ifdef DEBUG
		fprintf(stderr, "%s: At start of loop buf is %s\n", hostname, buf);
#endif
		cp = buf;
		if (atomicio(read, remin, cp, 1) != 1)
			goto done;
		if (*cp++ == '\n')
			SCREWUP("unexpected <newline>");
		do {
			if (atomicio(read, remin, &ch, sizeof(ch)) != sizeof(ch))
				SCREWUP("lost connection");
			*cp++ = ch;
		} while (cp < &buf[sizeof(buf) - 1] && ch != '\n');
		*cp = 0;
		if (verbose_mode)
			fmprintf(stderr, "Sink: %s", buf);

		if (buf[0] == '\01' || buf[0] == '\02') {
			if (iamremote == 0) {
				(void) snmprintf(visbuf, sizeof(visbuf),
				    NULL, "%s", buf + 1);
				(void) atomicio(vwrite, STDERR_FILENO,
				    visbuf, strlen(visbuf));
			}
			if (buf[0] == '\02')
				exit(1);
			++errs;
			continue;
		}
		if (buf[0] == 'E') {
#ifdef DEBUG
			fprintf (stderr, "%s: Sending null to remout.\n", hostname);
#endif
			(void) atomicio(vwrite, remout, "", 1);
			goto done;
		}
		if (ch == '\n')
			*--cp = 0;

		cp = buf;
#ifdef DEBUG
		fprintf(stderr, "%s: buf is %s\n", hostname, buf);
#endif
		if (*cp == 'T') {
			setimes++;
			cp++;
			if (!isdigit((unsigned char)*cp))
				SCREWUP("mtime.sec not present");
			ull = strtoull(cp, &cp, 10);
			if (!cp || *cp++ != ' ')
				SCREWUP("mtime.sec not delimited");
			if (TYPE_OVERFLOW(time_t, ull))
				setimes = 0;	/* out of range */
			mtime.tv_sec = ull;
			mtime.tv_usec = strtol(cp, &cp, 10);
			if (!cp || *cp++ != ' ' || mtime.tv_usec < 0 ||
			    mtime.tv_usec > 999999)
				SCREWUP("mtime.usec not delimited");
			if (!isdigit((unsigned char)*cp))
				SCREWUP("atime.sec not present");
			ull = strtoull(cp, &cp, 10);
			if (!cp || *cp++ != ' ')
				SCREWUP("atime.sec not delimited");
			if (TYPE_OVERFLOW(time_t, ull))
				setimes = 0;	/* out of range */
			atime.tv_sec = ull;
			atime.tv_usec = strtol(cp, &cp, 10);
			if (!cp || *cp++ != '\0' || atime.tv_usec < 0 ||
			    atime.tv_usec > 999999)
				SCREWUP("atime.usec not delimited");

#ifdef DEBUG
			fprintf (stderr, "%s: Sending null to remout.\n", hostname);
#endif
			(void) atomicio(vwrite, remout, "", 1);
			continue;
		}
		if (*cp == 'R') { /*resume file transfer (dont' think I need this here)*/
#ifdef DEBUG
			fprintf(stderr, "%s: Received a RESUME request with %s\n", hostname, cp);
#endif
			resume_flag = 1;
		}
		if (*cp != 'C' && *cp != 'D') {
			/*
			 * Check for the case "rcp remote:foo\* local:bar".
			 * In this case, the line "No match." can be returned
			 * by the shell before the rcp command on the remote is
			 * executed so the ^Aerror_message convention isn't
			 * followed.
			 */
			if (first) {
				run_err("%s", cp);
				exit(1);
			}
			SCREWUP("expected control record");
		}
		mode = 0;
#ifdef DEBUG
		fprintf(stderr, "%s: buf is %s\n", hostname, buf);
		fprintf(stderr, "%s: cp is %s\n", hostname, cp);
#endif
		/* we need to track if this object is a directory
		 * before we move the pointer. If we are in resume mode
		 * we might end up trying to get an mdsum on a directory
		 * and that doesn't work */
		int dir_flag = 0;
		if (*cp == 'D')
			dir_flag = 1;

		for (++cp; cp < buf + 5; cp++) {
			if (*cp < '0' || *cp > '7')
				SCREWUP("bad mode");
			mode = (mode << 3) | (*cp - '0');
		}
		if (!pflag)
			mode &= ~mask;
		if (*cp++ != ' ')
			SCREWUP("mode not delimited");

#ifdef DEBUG
		fprintf(stderr, "%s: cp is %s\n", hostname, cp);
#endif

		if (!isdigit((unsigned char)*cp))
			SCREWUP("size not present");
		ull = strtoull(cp, &cp, 10);
		if (!cp || *cp++ != ' ')
			SCREWUP("size not delimited");
		if (TYPE_OVERFLOW(off_t, ull))
			SCREWUP("size out of range");
		size = (off_t)ull;

#ifdef DEBUG
		fprintf(stderr, "%s: cp is %s\n", hostname, cp);
#endif
		if (resume_flag && !dir_flag) {
			*remote_hashsum = '\0';
			int i;
			for (i = 0; i < HASH_LEN; i++) {
				strncat (remote_hashsum, cp++, 1);
			}
#ifdef DEBUG
			fprintf (stderr, "%s: '%s'\n", hostname, remote_hashsum);
#endif
			if (!cp || *cp++ != ' ')
				SCREWUP("hash not delimited");
		}
#ifdef DEBUG
		fprintf(stderr, "%s: cp is %s\n", hostname, cp);
#endif
		if (*cp == '\0' || strchr(cp, '/') != NULL ||
		    strcmp(cp, ".") == 0 || strcmp(cp, "..") == 0) {
			run_err("error: unexpected filename: %s", cp);
			exit(1);
		}
#ifdef DEBUG
		fprintf(stderr, "%s cp is %s\n", hostname, cp);
#endif
		if (npatterns > 0) {
			for (n = 0; n < npatterns; n++) {
				if (fnmatch(patterns[n], cp, 0) == 0)
					break;
			}
			if (n >= npatterns)
				SCREWUP("filename does not match request");
		}
		if (targisdir) {
			static char *namebuf;
			static size_t cursize;
			size_t need;

			need = strlen(targ) + strlen(cp) + 250;
			if (need > cursize) {
				free(namebuf);
				namebuf = xmalloc(need);
				cursize = need;
			}
			(void) snprintf(namebuf, need, "%s%s%s", targ,
			    strcmp(targ, "/") ? "/" : "", cp);
			np = namebuf;
		} else
			np = targ;
		curfile = cp;
		exists = stat(np, &stb) == 0;
		if (buf[0] == 'D') {
			int mod_flag = pflag;
			if (!iamrecursive)
				SCREWUP("received directory without -r");
			if (exists) {
				if (!S_ISDIR(stb.st_mode)) {
					errno = ENOTDIR;
					goto bad;
				}
				if (pflag)
					(void) chmod(np, mode);
			} else {
				/* Handle copying from a read-only directory */
				mod_flag = 1;
				if (mkdir(np, mode | S_IRWXU) == -1)
					goto bad;
			}
			vect[0] = xstrdup(np);
			sink(1, vect, src);
			if (setimes) {
				setimes = 0;
				(void) utimes(vect[0], tv);
			}
			if (mod_flag)
				(void) chmod(vect[0], mode);
			free(vect[0]);
			continue;
		}
		omode = mode;
		mode |= S_IWUSR;
		stat(cp, &cpstat);
		xfer_size = size;
		if (resume_flag) {
#ifdef DEBUG
			fprintf(stderr, "%s: np is %s\n", hostname, np);
#endif
			/* does the file exist and if it does it writable? */
			if (stat(np, &npstat) == -1) {
#ifdef DEBUG
				fprintf(stderr, "%s Local file does not exist size is %ld!\n",
					hostname, npstat.st_size);
#endif
				npstat.st_size = 0;
			} else {
				/* check to see if the file is writeable
				 * if it isn't then we need to skip it but
				 * before we skip it we need to send the remote
				 * what they are expecting so BUF_AND_HASH bytes and then
				 * a null.
				 * NOTE!!! The format in the snprintf needs the actual numeric
				 * because using a define isn't working */
				if (access (np, W_OK) != 0) {
					fprintf(stderr, "scp: %s: Permission denied on %s\n", np, hostname);
					snprintf(outbuf, BUF_AND_HASH, "S%-*s", BUF_AND_HASH-2, " ");
					(void)atomicio(vwrite, remout, outbuf, strlen(outbuf));
					(void)atomicio(vwrite, remout, "", 1);
					continue;
				}
			}
			/* this file is already here do we need to move it?
			 * Check to make sure npstat.st_size > 0. If it is 0 then we
			 * may trying to be moving a zero byte file in which case this
			 * following block fails. See on 0 byte files the hashes will
			 * always match and the file won't be created even though it should
			 */
			if (xfer_size == npstat.st_size && (npstat.st_size > 0)) {
				calculate_hash(np, local_hashsum, npstat.st_size);
				if (strcmp(local_hashsum,remote_hashsum) == 0) {
					/* we can skip this file if we want to. */
#ifdef DEBUG
					fprintf(stderr, "%s: Files are the same\n", hostname);
#endif
					/* the remote is expecting something so we need to send them something*/
					snprintf(outbuf, BUF_AND_HASH, "S%-*s", BUF_AND_HASH-2, " ");
					(void)atomicio(vwrite, remout, outbuf, strlen(outbuf));
#ifdef DEBUG
					fprintf(stderr,"%s: sent '%s' to remote\n", hostname, outbuf);
#endif
					/* the remote is waiting on an ack so send a null */
					(void)atomicio(vwrite, remout, "", 1);
					if (showprogress)
						fprintf (stderr, "Skipping identical file: %s\n", np);
					continue;
				} else {
					/* file sizes are the same but they don't match */
#ifdef DEBUG
					fprintf(stderr, "%s: target(%ld) is different than source(%ld)!\n",
						hostname, npstat.st_size, size);
#endif
					snprintf(tmpbuf, sizeof outbuf, "C%04o %lld %s",
						 (u_int) (npstat.st_mode & FILEMODEMASK),
						 (long long)npstat.st_size, local_hashsum);
					snprintf(outbuf, BUF_AND_HASH, "%-*s", BUF_AND_HASH-1, tmpbuf);
					(void) atomicio(vwrite, remout, outbuf, strlen(outbuf));
					bad_match_flag = 1;
				}
			}
			/* if npstat.st_size is 0 then the local file doesn't exist and
			 * we have to move it. Since we are in resume mode treat it as a resume */
			if (npstat.st_size < xfer_size || (npstat.st_size == 0)) {
				char rand_string[9];
#ifdef DEBUG
				fprintf (stderr, "%s: %s is smaller than %s\n", hostname, np, cp);
#endif
				calculate_hash(np, local_hashsum, npstat.st_size);
#define	FILEMODEMASK	(S_ISUID|S_ISGID|S_IRWXU|S_IRWXG|S_IRWXO)
				snprintf(tmpbuf, sizeof outbuf, "R%04o %lld %s",
					 (u_int) (npstat.st_mode & FILEMODEMASK),
					 (long long)npstat.st_size, local_hashsum);
				snprintf(outbuf, BUF_AND_HASH, "%-*s", BUF_AND_HASH-1, tmpbuf);
#ifdef DEBUG
				fprintf (stderr, "%s: new buf is %s of length %ld\n",
					 hostname, outbuf, strlen(outbuf));
				fprintf(stderr, "%s: Sending new file (%s) modes: %s\n",
					hostname, np, outbuf);
#endif
				/*now we have to send np's length and hash to the other end
				 * if the computed hashes match then we seek to np's length in
				 * file and append to np starting from there */
				(void) atomicio(vwrite, remout, outbuf, strlen(outbuf));
#ifdef DEBUG
				fprintf(stderr, "%s: New size: %ld, size: %ld, st_size: %ld\n",
					hostname, size - npstat.st_size, size, npstat.st_size);
#endif
				xfer_size = size - npstat.st_size;
				resume_flag = 1;
				np_tmp = xstrdup(np);
				/* We should have a random component to avoid clobbering a
				 * local file */
				rand_str(rand_string, 8);
				strcat(np, rand_string);
#ifdef DEBUG
				fprintf(stderr, "%s: Will concat %s to %s after xfer\n",
					hostname, np, np_tmp);
#endif
			} else if (npstat.st_size > size) {
			/* the target file is larger than the source.
			 * so we need to overwrite it. We don't need to send the hash though. */
#ifdef DEBUG
				fprintf(stderr, "%s: target(%ld) is larger than source(%ld)!\n",
					hostname, npstat.st_size, size);
#endif
				snprintf(tmpbuf, sizeof outbuf, "C%04o %lld",
					 (u_int) (npstat.st_mode & FILEMODEMASK),
					 (long long)npstat.st_size);
				snprintf(outbuf, BUF_AND_HASH, "%-*s", BUF_AND_HASH-1, tmpbuf);
				(void) atomicio(vwrite, remout, outbuf, strlen(outbuf));
				bad_match_flag = 1;
			}

#ifdef DEBUG
			fprintf (stderr, "%s: CP is %s(%ld) NP is %s(%ld)\n",
				 hostname, cp, size, np, npstat.st_size);
#endif
			/* we are in resume mode so we need this *here* and not later
			 * because we need to get the file match information from the remote
			 * outside of resume mode we don't get that so we get out of sync
			 * so we have a test for the resume_flag after this block */
#ifdef DEBUG
			fprintf (stderr, "%s: Sending null to remout.\n", hostname);
#endif
			(void) atomicio(vwrite, remout, "", 1);

			/* the remote is always going to send a match status
			 * so we need to read it so we don't get out of sync */
			(void) atomicio(read, remin, &match, 1);
			if (match != 'M') {/*fragments do not match*/
				/* expected response of F, M and NULL *but*
				 * anything other than M is a failure
				 * if it's a NULL then we reset xfer_size but
				 * we retain the file pointers */
				xfer_size = size;
				bad_match_flag = 1;
				if (match == 'F') {
					/* got an F for failure and not NULL
					 * so we want to swap over the filename from
					 * the temp back to the original */
#ifdef DEBUG
					fprintf(stderr, "%s: match status is F\n", hostname);
#endif
					if (np_tmp != NULL)
						np = np_tmp;
					else {
						continue;
					}
				} else {
#ifdef DEBUG
					fprintf(stderr, "%s: match received is NULL\n", hostname);
#endif
				}
			} else {
#ifdef DEBUG
				fprintf(stderr, "%s match status is M\n", hostname);
#endif
				bad_match_flag = 0; /* while this is set at the beginning of the
                                                     * loop I'm setting it here explicitly as well */
			}
		}

#ifdef DEBUG
		fprintf(stderr, "%s: Creating file. mode is %d for %s\n",
			hostname, mode, np);
#endif
		if ((ofd = open(np, O_WRONLY|O_CREAT, mode)) == -1) {
bad:			run_err("%s: %s", np, strerror(errno));
			continue;
		}

		/* in the case of not using the resume function we need this vwrite here
		 * in the case of using the resume flag it comes in the above if (resume_flag) block
		 * why? because scp is weird and depends on an intricate and silly dance of
		 * call and response at just the right time. That's why */
		if (!resume_flag) {
#ifdef DEBUG
			fprintf (stderr, "%s: Sending null to remout.\n", hostname);
#endif
			(void) atomicio(vwrite, remout, "", 1);
		}

		if ((bp = allocbuf(&buffer, ofd, COPY_BUFLEN)) == NULL) {
			(void) close(ofd);
			continue;
		}
		cp = bp->buf;
		wrerr = 0;

		/*
		 * NB. do not use run_err() unless immediately followed by
		 * exit() below as it may send a spurious reply that might
		 * desyncronise us from the peer. Use note_err() instead.
		 */
		statbytes = 0;
		if (showprogress)
			start_progress_meter(curfile, xfer_size, &statbytes);
		set_nonblock(remin);
#ifdef DEBUG
		fprintf(stderr, "%s: xfer_size is %ld\n", hostname, xfer_size);
#endif
		for (count = i = 0; i < xfer_size; i += bp->cnt) {
			amt = bp->cnt;
			if (i + amt > xfer_size)
				amt = xfer_size - i;
			count += amt;
			/* read the data from the socket*/
			do {

				j = atomicio6(read, remin, cp, amt,
				    scpio, &statbytes);
				if (j == 0) {
					run_err("%s", j != EPIPE ?
					    strerror(errno) :
					    "dropped connection");
					exit(1);
				}
				amt -= j;
				cp += j;
			} while (amt > 0);
			if (count == bp->cnt) {
				/* Keep reading so we stay sync'd up. */
				if (!wrerr) {
					if (atomicio(vwrite, ofd, bp->buf,
					    count) != count) {
						note_err("%s: %s", np,
						    strerror(errno));
						wrerr = 1;
					}
				}
				count = 0;
				cp = bp->buf;
			}
		}
		unset_nonblock(remin);
		if (count != 0 && !wrerr &&
		    atomicio(vwrite, ofd, bp->buf, count) != count) {
			note_err("%s: %s", np, strerror(errno));
			wrerr = 1;
		}
		if (!wrerr && (!exists || S_ISREG(stb.st_mode)) &&
		    ftruncate(ofd, xfer_size) != 0)
			note_err("%s: truncate: %s", np, strerror(errno));

                /* if np_tmp isn't set then we don't have a resume file to cat */
		/* likewise, bad match flag means no resume flag */
#ifdef DEBUG
		fprintf (stderr, "%s: resume_flag: %d, np_tmp: %s, bad_match_flag: %d\n",
			 hostname, resume_flag, np_tmp, bad_match_flag);
#endif
		if (resume_flag && np_tmp && !bad_match_flag) {
			FILE *orig, *resume;
			char res_buf[512]; /* set at 512 just because, might want to increase*/
			ssize_t res_bytes = 0;
			off_t sum = 0;
			struct stat res_stat;
			*res_buf = '\0';
			orig = NULL; /*supress warnings*/
			resume = NULL; /*supress warnings*/
#ifdef DEBUG
			fprintf(stderr, "%s: Resume flag is set. Going to concat %s to %s  now\n",
				hostname, np, np_tmp);
#endif
			/* np/ofd is the resume file so open np_tmp for appending
			 * close ofd because we are going to be shifting it
			 * and I don't wnat the same file open in multiple descriptors */
			if (close(ofd) == -1)
				note_err("%s: close: %s", np, strerror(errno));
			/* orig is the target file, resume is the temp file */
			orig = fopen(np_tmp, "a"); /*open for appending*/
			if (orig == NULL) {
				fprintf(stderr, "%s: Could not open %s for appending.", hostname, np_tmp);
				goto stopcat;
			}
			resume = fopen(np, "r"); /*open for reading only*/
			if (resume == NULL) {
				fprintf(stderr, "%s: Could not open %s for reading.", hostname, np);
				goto stopcat;
			}
			/* get the number of bytes in the temp file*/
			if (fstat(fileno(resume), &res_stat) == -1) {
				fprintf(stderr, "%s: Could not stat %s", hostname, np);
				goto stopcat;
			}
			/* while the number of bytes read from the temp file
			 * is less than the size of the file read in a chunk and
			 * write it to the target file */
			do {
				res_bytes = fread(res_buf, 1, 512, resume);
				fwrite(res_buf, 1, res_bytes, orig);
				sum += res_bytes;
			} while (sum < res_stat.st_size);

stopcat:		if (orig)
				fclose(orig);
			if (resume)
				fclose(resume);
			/* delete the resume file */
			remove(np);
#ifdef DEBUG
			fprintf (stderr, "%s: np(%s) and np_tmp(%s)\n", hostname, np, np_tmp);
#endif
			np = np_tmp;
#ifdef DEBUG
			fprintf (stderr, "%s np(%s) and np_tmp(%s)\n", hostname, np, np_tmp);
#endif
			/* reset ofd to the original np */
			if ((ofd = open(np_tmp, O_WRONLY)) == -1) {
				fprintf(stderr, "%s: couldn't open %s in append function\n", hostname, np_tmp);
				atomicio(vwrite, remout, "", 1);
				goto bad;
			}
		}
		if (pflag) {
			if (exists || omode != mode)
#ifdef HAVE_FCHMOD
				if (fchmod(ofd, omode)) {
#else /* HAVE_FCHMOD */
				if (chmod(np, omode)) {
#endif /* HAVE_FCHMOD */
					note_err("%s: set mode: %s",
					    np, strerror(errno));
				}
		} else {
			if (!exists && omode != mode)
#ifdef HAVE_FCHMOD
				if (fchmod(ofd, omode & ~mask)) {
#else /* HAVE_FCHMOD */
				if (chmod(np, omode & ~mask)) {
#endif /* HAVE_FCHMOD */
					note_err("%s: set mode: %s",
					    np, strerror(errno));
				}
		}
		if (close(ofd) == -1)
			note_err("%s: close: %s", np, strerror(errno));
		(void) response();
		if (showprogress)
			stop_progress_meter();
		if (setimes && !wrerr) {
			setimes = 0;
			if (utimes(np, tv) == -1) {
				note_err("%s: set times: %s",
				    np, strerror(errno));
			}
		}
		/* If no error was noted then signal success for this file */
		if (note_err(NULL) == 0) {
#ifdef DEBUG
			fprintf (stderr, "%s: Sending null to remout.\n", hostname);
#endif
			(void) atomicio(vwrite, remout, "", 1); }
		/* we are in resume mode and we have allocated memory for np_tmp */
		if (resume_flag && np_tmp != NULL) {
			free(np_tmp);
			np_tmp = NULL;
		}
	}
done:
	for (n = 0; n < npatterns; n++)
		free(patterns[n]);
	free(patterns);
	return;
screwup:
	for (n = 0; n < npatterns; n++)
		free(patterns[n]);
	free(patterns);
	run_err("protocol error: %s", why);
	exit(1);
}

void
throughlocal_sftp(struct sftp_conn *from, struct sftp_conn *to,
    char *src, char *targ)
{
	char *target = NULL, *filename = NULL, *abs_dst = NULL;
	char *abs_src = NULL, *tmp = NULL;
	glob_t g;
	int i, r, targetisdir, err = 0;

	if ((filename = basename(src)) == NULL)
		fatal("basename %s: %s", src, strerror(errno));

	if ((abs_src = prepare_remote_path(from, src)) == NULL ||
	    (target = prepare_remote_path(to, targ)) == NULL)
		cleanup_exit(255);
	memset(&g, 0, sizeof(g));

	targetisdir = remote_is_dir(to, target);
	if (!targetisdir && targetshouldbedirectory) {
		error("%s: destination is not a directory", targ);
		err = -1;
		goto out;
	}

	debug3_f("copying remote %s to remote %s", abs_src, target);
	if ((r = remote_glob(from, abs_src, GLOB_MARK, NULL, &g)) != 0) {
		if (r == GLOB_NOSPACE)
			error("%s: too many glob matches", src);
		else
			error("%s: %s", src, strerror(ENOENT));
		err = -1;
		goto out;
	}

	for (i = 0; g.gl_pathv[i] && !interrupted; i++) {
		tmp = xstrdup(g.gl_pathv[i]);
		if ((filename = basename(tmp)) == NULL) {
			error("basename %s: %s", tmp, strerror(errno));
			err = -1;
			goto out;
		}

		if (targetisdir)
			abs_dst = path_append(target, filename);
		else
			abs_dst = xstrdup(target);

		debug("Fetching %s to %s\n", g.gl_pathv[i], abs_dst);
		if (globpath_is_dir(g.gl_pathv[i]) && iamrecursive) {
			if (crossload_dir(from, to, g.gl_pathv[i], abs_dst,
			    NULL, pflag, SFTP_PROGRESS_ONLY, 1) == -1)
				err = -1;
		} else {
			if (do_crossload(from, to, g.gl_pathv[i], abs_dst, NULL,
			    pflag) == -1)
				err = -1;
		}
		free(abs_dst);
		abs_dst = NULL;
		free(tmp);
		tmp = NULL;
	}

out:
	free(abs_src);
	free(abs_dst);
	free(target);
	free(tmp);
	globfree(&g);
	if (err == -1)
		errs = 1;
}

int
response(void)
{
	char ch, *cp, resp, rbuf[2048], visbuf[2048];

	if (atomicio(read, remin, &resp, sizeof(resp)) != sizeof(resp))
		lostconn(0);

	cp = rbuf;

	switch (resp) {
	case 0:		/* ok */
		return (0);
	default:
		*cp++ = resp;
		/* FALLTHROUGH */
	case 1:		/* error, followed by error msg */
	case 2:		/* fatal error, "" */
		do {
			if (atomicio(read, remin, &ch, sizeof(ch)) != sizeof(ch))
				lostconn(0);
			*cp++ = ch;
		} while (cp < &rbuf[sizeof(rbuf) - 1] && ch != '\n');

		if (!iamremote) {
			cp[-1] = '\0';
			(void) snmprintf(visbuf, sizeof(visbuf),
			    NULL, "%s\n", rbuf);
			(void) atomicio(vwrite, STDERR_FILENO,
			    visbuf, strlen(visbuf));
		}
		++errs;
		if (resp == 1)
			return (-1);
		exit(1);
	}
	/* NOTREACHED */
}

void
usage(void)
{
#ifdef WITH_OPENSSL
	(void) fprintf(stderr,
	    "usage: hpnscp [-346ABCOpqRrsTvZ] [-c cipher] [-D sftp_server_path] [-F ssh_config]\n"
	    "              [-i identity_file] [-J destination] [-l limit]\n"
	    "              [-o ssh_option] [-P port] [-z filepath of remote scp]" 
	    "              [-S program] source ... target\n");
	exit(1);
#else
	(void) fprintf(stderr,
	    "usage: hpnscp [-346ABCOpqRrsTv] [-c cipher] [-D sftp_server_path] [-F ssh_config]\n"
	    "              [-i identity_file] [-J destination] [-l limit]\n"
	    "              [-o ssh_option] [-P port]" 
	    "              [-S program] source ... target\n");
	exit(1);
#endif
	
}

void
run_err(const char *fmt,...)
{
	static FILE *fp;
	va_list ap;

	++errs;
	if (fp != NULL || (remout != -1 && (fp = fdopen(remout, "w")))) {
		(void) fprintf(fp, "%c", 0x01);
		(void) fprintf(fp, "scp: ");
		va_start(ap, fmt);
		(void) vfprintf(fp, fmt, ap);
		va_end(ap);
		(void) fprintf(fp, "\n");
		(void) fflush(fp);
	}

	if (!iamremote) {
		va_start(ap, fmt);
		vfmprintf(stderr, fmt, ap);
		va_end(ap);
		fprintf(stderr, "\n");
	}
}

/*
 * Notes a sink error for sending at the end of a file transfer. Returns 0 if
 * no error has been noted or -1 otherwise. Use note_err(NULL) to flush
 * any active error at the end of the transfer.
 */
int
note_err(const char *fmt, ...)
{
	static char *emsg;
	va_list ap;

	/* Replay any previously-noted error */
	if (fmt == NULL) {
		if (emsg == NULL)
			return 0;
		run_err("%s", emsg);
		free(emsg);
		emsg = NULL;
		return -1;
	}

	errs++;
	/* Prefer first-noted error */
	if (emsg != NULL)
		return -1;

	va_start(ap, fmt);
	vasnmprintf(&emsg, INT_MAX, NULL, fmt, ap);
	va_end(ap);
	return -1;
}

void
verifydir(char *cp)
{
	struct stat stb;

	if (!stat(cp, &stb)) {
		if (S_ISDIR(stb.st_mode))
			return;
		errno = ENOTDIR;
	}
	run_err("%s: %s", cp, strerror(errno));
	killchild(0);
}

int
okname(char *cp0)
{
	int c;
	char *cp;

	cp = cp0;
	do {
		c = (int)*cp;
		if (c & 0200)
			goto bad;
		if (!isalpha(c) && !isdigit((unsigned char)c)) {
			switch (c) {
			case '\'':
			case '"':
			case '`':
			case ' ':
			case '#':
				goto bad;
			default:
				break;
			}
		}
	} while (*++cp);
	return (1);

bad:	fmprintf(stderr, "%s: invalid user name\n", cp0);
	return (0);
}

BUF *
allocbuf(BUF *bp, int fd, int blksize)
{
	size_t size;
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
	struct stat stb;

	if (fstat(fd, &stb) == -1) {
		run_err("fstat: %s", strerror(errno));
		return (0);
	}
	size = ROUNDUP(stb.st_blksize, blksize);
	if (size == 0)
		size = blksize;
#else /* HAVE_STRUCT_STAT_ST_BLKSIZE */
	size = blksize;
#endif /* HAVE_STRUCT_STAT_ST_BLKSIZE */
	if (bp->cnt >= size)
		return (bp);
	bp->buf = xrecallocarray(bp->buf, bp->cnt, size, 1);
	bp->cnt = size;
	return (bp);
}

void
lostconn(int signo)
{
	if (!iamremote)
		(void)write(STDERR_FILENO, "lost connection\n", 16);
	if (signo)
		_exit(1);
	else
		exit(1);
}

void rand_str(char *dest, size_t length) {
	char charset[] = "0123456789"
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	while (length-- > 0) {
		size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
		*dest++ = charset[index];
	}
	*dest = '\0';
}

void
cleanup_exit(int i)
{
	if (remin > 0)
		close(remin);
	if (remout > 0)
		close(remout);
	if (remin2 > 0)
		close(remin2);
	if (remout2 > 0)
		close(remout2);
	if (do_cmd_pid > 0)
		waitpid(do_cmd_pid, NULL, 0);
	if (do_cmd_pid2 > 0)
		waitpid(do_cmd_pid2, NULL, 0);
	exit(i);
}
