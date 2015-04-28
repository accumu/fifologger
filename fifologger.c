/*
 * fifologger $Id$
 *
 * vim: sts=4:sw=4:cindent
 *
 * Reads input from a FIFO and writes it into a file specified with strftime(3)
 * syntax.
 *
 * Outfile is reopened hourly and fflush():ed at intervals determined by
 * OUT_SYNC_INTERVAL.
 *
 * Suitable format: xferlog.%Y%m%d  (xferlog-20011027)
 *
 * -- stric 2001-10-27 - Initial implementation.
 * -- nikke 2004-09-18 - Added functionality to not fopen/fclose outfile on
 *                       each fgets.
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


#define STRSIZE 32768

/* Interval between forced fflush() of output file */
#define OUT_SYNC_INTERVAL 2

/* RCS version strings and stuff, to be used with in help text or when running
   ident on the binary */
static const char rcsid[] = "$Id$";

/* Global variables are sooo elegant ;-) */
int debug = 0;
int verbose = 0;
int fifo = -1;
char *fifoname = NULL;
char *outformat = NULL;
int printerrors = 1;

void
exithandler(int signum) {
    fflush(NULL);
    exit(1);
}

void
error(int lvl, char *str, char *arg) {
    char buf[STRSIZE];

    snprintf(buf, STRSIZE, "[%%s] %s: errno=%s", str, strerror(errno));
    syslog(lvl, buf, fifoname, arg);
    if (printerrors) {
        strcat(buf, "\n");
        fprintf(stderr, buf, fifoname, arg);
    }
}

int
openfifo(char *name) {
    int f;
    struct stat st;

    f = open(name, O_RDONLY|O_NONBLOCK);
    if (f < 0) {
        error(LOG_ERR, "Unable to open input fifo %s", name);
        closelog();
        exit(1);
    }
    if(fstat(f, &st) < 0) {
        error(LOG_ERR, "Unable to stat input fifo %s", name);
	close(f);
        closelog();
        exit(1);
    }
    if(!S_ISFIFO(st.st_mode)) {
        error(LOG_ERR, "Opening input file %s: not a FIFO", name);
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
    static time_t outflastflush = 0;
    static time_t outfclosetime = 0;

    t = time(NULL);

    /* Close outfile if it has been open too long */
    if( outf && t>=outfclosetime) {
        fclose(outf);
        outf=NULL;
    }

    /* Open outfile if not already open */
    if(!outf) {
        char buf[PATH_MAX+1];
        struct tm *tim = localtime(&t);
        struct tm hrtim;

        strftime(buf, PATH_MAX, outformat, tim);
        outf = fopen(buf, "a");
        if (!outf) {
            error(LOG_CRIT, "Unable to open outfile %s", buf);
            return 1;
        }

        /* Figure out when it's time to close it (when the next hour starts) */
        memcpy(&hrtim, tim, sizeof(struct tm));
        hrtim.tm_sec=0;
        hrtim.tm_min=0;
        outfclosetime = mktime(&hrtim)+3600;
    }

    if(size > 0) {
	if (fwrite(ptr, size, 1, outf) != 1) {
	    error(LOG_CRIT, "Failed to write to outfile %s", outformat);
	    fclose(outf);
	    outf=NULL;
	    return 2;
	}
    }

    if(outflastflush+OUT_SYNC_INTERVAL < t) {
        if(fflush(outf) != 0) {
	    error(LOG_CRIT, "Failed to flush outfile %s", outformat);
	    fclose(outf);
	    outf=NULL;
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
	    error(LOG_ERR, "poll() %s", "failed");
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
	    error(LOG_ERR, "poll() error: %s", e);
	    sleep(OUT_SYNC_INTERVAL); /* To avoid busy-error-looping */
	    continue;
	}

	rsize = read(fifo, buf, sizeof(buf));

	if(rsize < 0) {
	    if(errno == EAGAIN || errno == EWOULDBLOCK) {
		continue;
	    }
	    error(LOG_ERR, "read() %s", "failed");
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

    while ((opt = getopt(argc, argv, "u:dv")) != -1) {
	switch (opt) {
	    case 'u':
		runuser = optarg;
		break;
	    case 'd':
		debug = 1;
		break;
	    case 'v':
		verbose = 1;
		break;
	    default:
		fprintf(stderr, "%s %s\n", argv[0], rcsid);
		fprintf(stderr, "Usage: %s [-u username] [-d] [-v]  <fifo> <outformat>\n", argv[0]);
		fprintf(stderr, "          -u username - run as username\n");
		fprintf(stderr, "          -d - debug/donotdetach\n");
		fprintf(stderr, "          -v - verbose\n");
		exit(0);
	}
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

    if(argc != optind+2) {
	fprintf(stderr, "FATAL: Expected exactly 2 arguments after options\n");
	exit(1);
    }

    fifoname = argv[optind];
    outformat = argv[optind+1];

    if(fifoname[0] != '/' || outformat[0] != '/') {
	fprintf(stderr, "FATAL: Expected absolute paths\n");
	exit(1);
    }

    if(!debug) {
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
        error(LOG_ERR, "sicaction() failed for %s", "SIGHUP");
        perror("sigaction");
        exit(1);
    }
    if(sigaction(SIGINT, &sa, NULL)) {
        error(LOG_ERR, "sicaction() failed for %s", "SIGINT");
        exit(1);
    }
    if(sigaction(SIGTERM, &sa, NULL)) {
        error(LOG_ERR, "sicaction() failed for %s", "SIGTERM");
        exit(1);
    }
    if(sigaction(SIGQUIT, &sa, NULL)) {
        error(LOG_ERR, "sicaction() failed for %s", "SIGQUIT");
        exit(1);
    }

    fifo = openfifo(fifoname);
    if(!verbose) {
	printerrors = 0;
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);
    }
    mainloop();
    closelog();

    return 0;
}
