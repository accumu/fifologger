/*
 * fifologger $Id: $
 * Reads input from a FIFO and writes it into a file specified with strftime(3)
 * syntax. Changes file when appropriate.
 *
 * Suitable format: xferlog.%Y%m%d  (xferlog-20011027)
 *
 * -- stric 2001-10-27
 */
#include <stdio.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>

#define STRSIZE 32768

FILE *fifo;
char *fifoname;
char *outformat;
char currout[PATH_MAX];
FILE *outf = NULL;

void
error(int lvl, char *str, char *arg) {
    char buf[STRSIZE];

    sprintf(buf, "[%%s] %s: %s", str, strerror(errno));
    syslog(lvl, buf, fifoname, arg);
    strcat(buf, "\n");
    fprintf(stderr, buf, fifoname, arg);
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
    char buf[PATH_MAX];
    time_t t;
    struct tm *tim;

    t = time(NULL);
    tim = localtime(&t);

    strftime(buf, PATH_MAX, outformat, tim);
    if (!outf || strcmp(buf, currout)) {
        strcpy(currout, buf);
        if (outf)
            fclose(outf);
        outf = fopen(currout, "w+");
        if (!outf) {
            error(LOG_CRIT, "Unable to open outfile %s", currout);
            return 1;
        }
    }
    if (fputs(line, outf) == EOF) {
        error(LOG_CRIT, "Unable to write to %s", currout);
        return 2;
    }
    fflush(outf);

    return 0;
}

void
mainloop(void) {
    char *buf = malloc(STRSIZE);
    char *s;

    while (1) {
        s = fgets(buf, STRSIZE, fifo);
        if (s == 0) {
            sleep(1);
        } else {
            writeline(buf);
        }
    }
}

int
main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s: <fifo> <outformat>\n", argv[0]);
        exit(0);
    }
    openlog("fifologger", LOG_PID, LOG_DAEMON);
    fifo = openfifo(argv[1]);
    outformat = argv[2];
    mainloop();
    closelog();

    return 0;
}
