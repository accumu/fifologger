
# Copyright (C) 2001 Tomas Forsman (né Ögren)
# Copyright (C) 2004 Niklas Edmundsson
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

uname := $(shell uname)

ifeq ($(uname),Linux)
	CC := gcc
	CFLAGS := -g -O -Wall -Wextra -DGIT_SOURCE_DESC='"$(shell git describe --tags --always --dirty)"'

else
	CFLAGS := -g -O
endif

all: fifologger

fifologger: fifologger.c

clean:
	-rm fifologger
