/*
 * fifologger $Id: fifologger.c,v 1.6 2001/11/17 16:57:40 project Exp $
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

/* Enable large file API, just in case ;) */
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

#define STRSIZE 32768

/* Interval to fflush() output file */
#define OUT_SYNC_INTERVAL 60

/* RCS version strings and stuff, to be used with in help text or when running
   ident on the binary */
static const char rcsid[] = "$Id$";
static const char rcsrev[] = "$Revision$";

FILE *fifo;
char *fifoname;
char *outformat;
int dolog = 1;

void
sighandler(int signum) {
    fflush(NULL);
    exit(1);
}

void
error(int lvl, char *str, char *arg) {
    char buf[STRSIZE];

    snprintf(buf, STRSIZE, "[%%s] %s: %s", str, strerror(errno));
    syslog(lvl, buf, fifoname, arg);
    if (dolog) {
        strcat(buf, "\n");
        fprintf(stderr, buf, fifoname, arg);
    }
}

FILE *
openfifo(char *name) {
    FILE *f;

    fifoname = name;
    f = fopen(name, "r");
    if (f == NULL) {
        error(LOG_ERR, "Unable to open fifo %s", name);
        closelog();
        exit(1);
    }

    return f;
}

int
writeline(char *line) {
    time_t t;
    static FILE *outf = NULL;
    static time_t outfclosetime = 0;
    static time_t outflastflush = 0;

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
    if (fputs(line, outf) == EOF) {
        error(LOG_CRIT, "Unable to write to outfile %s", outformat);
        fclose(outf);
        outf=NULL;
        return 2;
    }

    if(outflastflush+OUT_SYNC_INTERVAL < t) {
        fflush(outf);
        outflastflush=t;
    }

    return 0;
}

void
mainloop(void) {
    char *buf = malloc(STRSIZE+1);
    char *s;

    while (1) {
        s = fgets(buf, STRSIZE, fifo);
        if (s == 0) {
            sleep(1);
        } else {
            /* Doesn't handle return values, don't know if they're worth much
               though.. */
            writeline(buf);
        }
    }
}

int
main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("fifologger %s\n", rcsrev);
        printf("Usage: %s: <fifo> <outformat>\n\n", argv[0]);
        printf("Don't use any relative paths (will cd to /).\n");
        exit(0);
    }

    /* Trap some standard signals so we can fflush() the outfile on exit */
    signal(SIGHUP, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGQUIT, sighandler);
    signal(SIGTERM, sighandler);

    chdir("/");
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
