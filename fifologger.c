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


#define STRSIZE 32768

/* Interval between forced fflush() of output file */
#define OUT_SYNC_INTERVAL 2

/* RCS version strings and stuff, to be used with in help text or when running
   ident on the binary */
static const char rcsid[] = "$Id$";

/* Global variables are sooo elegant ;-) */
int fifo;
char *fifoname;
char *outformat;
int dolog = 1;
time_t outflastflush = 0;

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
    if (dolog) {
        strcat(buf, "\n");
        fprintf(stderr, buf, fifoname, arg);
    }
}

int
openfifo(char *name) {
    int f;
    struct stat st;

    fifoname = name;
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
flushoutfile(FILE * file, time_t t) {
    if(outflastflush+OUT_SYNC_INTERVAL < t) {
        if(fflush(file) != 0) {
	    return 0;
	}
        outflastflush=t;
    }

    return 1;
}


int
writedata(char *ptr, ssize_t size) {
    time_t t;
    static FILE *outf = NULL;
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

    if(!flushoutfile(outf, t)) {
	error(LOG_CRIT, "Failed to flush outfile %s", outformat);
	fclose(outf);
	outf=NULL;
	return 3;
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

    if (argc != 3) {
        printf("fifologger %s\n", rcsid);
        printf("Usage: %s: <fifo> <outformat>\n\n", argv[0]);
        printf("Don't use any relative paths (will cd to /).\n");
        exit(0);
    }

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* Trap some standard signals so we can fflush() the outfile on exit */
    sa.sa_handler = exithandler;
    if(sigaction(SIGHUP, &sa, NULL)) {
        perror("sigaction");
        exit(1);
    }
    if(sigaction(SIGINT, &sa, NULL)) {
        perror("sigaction");
        exit(1);
    }
    if(sigaction(SIGTERM, &sa, NULL)) {
        perror("sigaction");
        exit(1);
    }
    if(sigaction(SIGQUIT, &sa, NULL)) {
        perror("sigaction");
        exit(1);
    }

    if(chdir("/") < 0) {
        perror("chdir /");
    }
    openlog("fifologger", LOG_PID, LOG_DAEMON);
    fifo = openfifo(argv[1]);
    outformat = argv[2];
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    dolog = 0;
    mainloop();
    closelog();

    return 0;
}
