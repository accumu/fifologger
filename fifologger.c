/*
 * fifologger $Id: fifologger.c,v 1.5 2001/11/17 16:51:41 project Exp project $
 * Reads input from a FIFO and writes it into a file specified with strftime(3)
 * syntax. Open+close for each read for maximum flexibility. The overhead
 * is insignificant compared to the other things it does to produce the line.
 * <bla bla>
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
int dolog = 1;

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
    char buf[PATH_MAX+1];
    time_t t;
    struct tm *tim;
    FILE *outf = NULL;

    t = time(NULL);
    tim = localtime(&t);

    strftime(buf, PATH_MAX, outformat, tim);
    outf = fopen(buf, "a");
    if (!outf) {
        error(LOG_CRIT, "Unable to open outfile %s", buf);
        return 1;
    }
    if (fputs(line, outf) == EOF) {
        error(LOG_CRIT, "Unable to write to %s", buf);
        fclose(outf);
        return 2;
    }
    fclose(outf);

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
        printf("Usage: %s: <fifo> <outformat>\n\n", argv[0]);
        printf("Don't use any relative paths (will cd to /).\n");
        exit(0);
    }
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
