# Introduction

Reads input from a FIFO and writes it into a file specified with strftime(3)
syntax. Enables logfiles with date-based names without restarting services
periodically. 

# Building

`make`

# Using

The main use for this is to get log files named by date or similar
without having to do log rotation and restart/reload services. Some
services/processes might even have a very long runtime making it impossible.

* Run as an unprivileged user dedicated to log handling.
* First argument must be a fifo writable by the service doing logging
  and readable by your chosen log handling user.
* Second argument is the target file, formatted using strftime(3).
* File is checked for name change each minute.
* When having many log files it makes sense to implement startup and
  sanity checking (paths, fifo, permissions, etc) in a script.

Example:

The following command would read the fifo `/var/log/xferlog` and emit
log files named `xferlog_YYYY-MM-DD` in the `/var/log/fifologger`
directory.

`fifologger /var/log/xferlog /var/log/fifologger/xferlog_%Y-%m-%d`
