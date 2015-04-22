uname := $(shell uname)

ifeq ($(uname),Linux)
	CC := gcc
	CFLAGS := -O -Wall
else
	CFLAGS := -O
endif

all: fifologger


fifologger: fifologger.c

clean:
	-rm fifologger
