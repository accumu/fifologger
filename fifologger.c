/*
 * fifologger $Id$
 *
 * vim: sts=4:sw=4:cindent
 *
 * Reads input from a FIFO and writes it into a file specified with strftime(3)
 * syntax.
 *
 * Outfile is checked each minute if it needs to be reopened. 
 * fflush() happens at intervals determined by OUT_SYNC_INTERVAL.
 *
 * Suitable format: xferlog.%Y%m%d  (xferlog-20011027)
 *
 * -- stric 2001-10-27 - Initial implementation.
 * -- nikke 2004-09-18 - Added functionality to not fopen/fclose outfile on
 *                       each fgets.
 */

/*
 * Copyright (C) 2001 Tomas Forsman (né Ögren)
 * Copyright (C) 2004 Niklas Edmundsson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Enable large file API, it's needed */
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE 1
#define _LARGE_FILES 1

#include <stdio.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <grp.h>

/* String buffer size */
#define STRSIZE 32768

/* Interval between forced fflush() of output file */
#define OUT_SYNC_INTERVAL 2

/* Suppress identical errors for this many seconds */
#define ERR_SUPPRESS_TIME 60

/* Emulate RCS $Id$, simply because it's handy to be able to run ident
      on an executable/library/etc and see the version.
 */
static const char rcsid[] = "$Id: " __FILE__ " " GIT_SOURCE_DESC " $";


/* Global variables are sooo elegant ;-) */
int detach = 1;
int fifo = -1;
char *fifoname = NULL;
char *outnametemplate = NULL;
int printmessages = 1;

void
message(int lvl, char *str, char *arg) {
    char buf[STRSIZE];
    static int lasterrno=0;
    static time_t lasttime=0;

    if(errno) {
	time_t now=time(NULL);
	if(now < lasttime + ERR_SUPPRESS_TIME && errno == lasterrno) {
		return;
	}
	lasttime=now;
	lasterrno=errno;
	snprintf(buf, STRSIZE, "[%%s] %s: %s", str, strerror(errno));
    }
    else {
	snprintf(buf, STRSIZE, "[%%s] %s", str);
    }
    syslog(lvl, buf, fifoname, arg);
    if (printmessages) {
        strcat(buf, "\n");
        fprintf(stderr, buf, fifoname, arg);
    }
}

void
exithandler(int signum) {
    errno = 0; /* Clear out errno for our message function */
    message(LOG_NOTICE, "%s, flushing and exiting...", strsignal(signum));
    fflush(NULL);
    exit(0);
}

int
openfifo(char *name) {
    int f;
    struct stat st;

    f = open(name, O_RDONLY|O_NONBLOCK);
    if (f < 0) {
        message(LOG_ERR, "Unable to open input fifo %s", name);
        closelog();
        exit(1);
    }
    if(fstat(f, &st) < 0) {
        message(LOG_ERR, "Unable to stat input fifo %s", name);
	close(f);
        closelog();
        exit(1);
    }
    if(!S_ISFIFO(st.st_mode)) {
        message(LOG_ERR, "Opening input file %s: not a FIFO", name);
	close(f);
        closelog();
        exit(1);
    }

    return f;
}


int
writedata(char *ptr, ssize_t size) {
    time_t t;
    static FILE *outf = NULL;
    static char outname[PATH_MAX];
    static time_t outflastflush = 0;
    static time_t outfchecktime = 0;

    t = time(NULL);

    /* Investigate if we need to reopen outfile */
    if(!outf || (outf && t>=outfchecktime) ) {
	char newname[PATH_MAX];
	struct tm *tim = localtime(&t);

        strftime(newname, PATH_MAX, outnametemplate, tim);
	if(outf && strcmp(outname, newname)) {
	    fclose(outf);
	    outf=NULL;
	    outname[0] = '\0';
	}

	/* Open outfile if not already open */
	if(!outf) {
	    struct tm hrtim;

	    errno = 0; /* Clear out errno for our message function */
	    outf = fopen(newname, "a");
	    if (!outf) {
		message(LOG_CRIT, "Unable to open outfile %s", newname);
		return 1;
	    }
	    strcpy(outname, newname);

	    /* Try to only emit message if we open a new file */
	    if(ftell(outf) == 0) {
		message(LOG_INFO, "Opened outfile %s", outname);
	    }

	    /* Figure out when next check for outfile name change is */
	    memcpy(&hrtim, tim, sizeof(struct tm));
	    hrtim.tm_sec=0;
	    outfchecktime = mktime(&hrtim)+60;
	}
    }


    if(size > 0) {
	if (fwrite(ptr, size, 1, outf) != 1) {
	    message(LOG_CRIT, "Failed to write to outfile %s", outname);
	    fclose(outf);
	    outf=NULL;
	    outname[0] = '\0';
	    return 2;
	}
    }

    if(outflastflush+OUT_SYNC_INTERVAL < t) {
        if(fflush(outf) != 0) {
	    message(LOG_CRIT, "Failed to flush outfile %s", outname);
	    fclose(outf);
	    outf=NULL;
	    outname[0] = '\0';
	    return 3;
	}
        outflastflush=t;
    }

    return 0;
}

void
mainloop(void) {
    char buf[STRSIZE];
    struct pollfd fds;
    int rc;
    ssize_t rsize;

    fds.fd = fifo;
    fds.events = POLLIN;
    fds.revents = 0;

    while (1) {
	rc = poll(&fds, 1, OUT_SYNC_INTERVAL*1000);
	if(rc < 0) {
	    /* Error/failure */
	    message(LOG_ERR, "poll() %s", "failed");
	    fflush(NULL);
	    closelog();
	    exit(1);
	}
	else if(rc == 0) {
	    /* Timeout */
	    writedata(NULL, 0); /* Flush/reopen outfile if needed */
	    continue;
	}

	if(fds.revents == POLLHUP) {
	    /* No writer attached to fifo */
	    writedata(NULL, 0); /* Flush/reopen outfile if needed */
	    sleep(OUT_SYNC_INTERVAL); /* To avoid busy-error-looping */
	    continue;
	}

	/* Activity on input fd, we only have one */
	if(! fds.revents & POLLIN) {
	    /* Some kind of error */
	    char *e="UNKNOWN";
	    if(fds.revents & POLLERR) {
		e = "POLLERR";
	    }
	    else if(fds.revents & POLLNVAL) {
		e = "POLLNVAL";
	    }
	    message(LOG_ERR, "poll() error: %s", e);
	    sleep(OUT_SYNC_INTERVAL); /* To avoid busy-error-looping */
	    continue;
	}

	rsize = read(fifo, buf, sizeof(buf));

	if(rsize < 0) {
	    if(errno == EAGAIN || errno == EWOULDBLOCK) {
		continue;
	    }
	    message(LOG_ERR, "read() %s", "failed");
	    sleep(OUT_SYNC_INTERVAL); /* To avoid busy-error-looping */
	    continue;
	}
	else if(rsize == 0) {
	    sleep(OUT_SYNC_INTERVAL); /* To avoid eof-looping */
	    continue;
	}

	writedata(buf, rsize);
    }
}

int
main(int argc, char *argv[]) {
    struct sigaction sa;
    char * runuser = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "u:D")) != -1) {
	switch (opt) {
	    case 'u':
		runuser = optarg;
		break;
	    case 'D':
		detach = 0;
		break;
	    default:
		fprintf(stderr, "%s %s\n", argv[0], rcsid);
		fprintf(stderr, "Usage: %s [-u username] [-d] [-v]  <fifo> <outnametemplate>\n", argv[0]);
		fprintf(stderr, "          -u username - run as username\n");
		fprintf(stderr, "          -D - Don't detach\n");
		exit(0);
	}
    }

    if(argc != optind+2) {
	fprintf(stderr, "FATAL: Expected exactly 2 arguments after options\n");
	exit(1);
    }

    fifoname = argv[optind];
    outnametemplate = argv[optind+1];

    if(fifoname[0] != '/' || outnametemplate[0] != '/') {
	fprintf(stderr, "FATAL: Expected absolute paths\n");
	exit(1);
    }

    if(geteuid() != 0) {
       if(runuser != NULL) {
	   fprintf(stderr, "FATAL: Can only specify -u runuser if started as root\n");
	   exit(1);
       }
    }
    else {
	struct passwd *pwd;

	if(runuser == NULL) {
	    fprintf(stderr, "FATAL: -u runuser required when started as root\n");
	    exit(1);
	}
	pwd = getpwnam(runuser);
	if (pwd == NULL) {
	    fprintf(stderr, "FATAL: user %s not found\n", runuser);
	    exit(1);
	}
	if (initgroups(runuser, pwd->pw_gid) < 0
		|| setgid(pwd->pw_gid) < 0
		|| setuid(pwd->pw_uid) < 0)
	{
	    perror("unable to drop privilege");
	    exit(1);
	}
    }

    if(detach) {
	if(daemon(0, 0) != 0) {
	    perror("daemon() failed");
	    exit(1);
	}
    }

    openlog("fifologger", LOG_PID, LOG_DAEMON);

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* Trap some standard signals so we can fflush() the outfile on exit */
    sa.sa_handler = exithandler;
    if(sigaction(SIGHUP, &sa, NULL)) {
        message(LOG_ERR, "sigaction() failed for %s", "SIGHUP");
        exit(1);
    }
    if(sigaction(SIGINT, &sa, NULL)) {
        message(LOG_ERR, "sigaction() failed for %s", "SIGINT");
        exit(1);
    }
    if(sigaction(SIGTERM, &sa, NULL)) {
        message(LOG_ERR, "sigaction() failed for %s", "SIGTERM");
        exit(1);
    }
    if(sigaction(SIGQUIT, &sa, NULL)) {
        message(LOG_ERR, "sigaction() failed for %s", "SIGQUIT");
        exit(1);
    }

    fifo = openfifo(fifoname);
    if(detach) {
	printmessages = 0;
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);
    }
    mainloop();
    closelog();

    return 0;
}
