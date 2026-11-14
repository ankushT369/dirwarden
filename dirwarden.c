/* dirwarden -- A small Linux daemon that watches one or more directories
 * and automatically deletes files a set number of seconds after they appear.
 *
 * Copyright (C) 2026 Ankush Mondal <mondalankush9851 at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/types.h>

#define APP_NAME "dirwarden"

#define MAX_PATHS 128
#define MAX_EVENTS 64
#define BUF_SIZE 1024

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (BUF_SIZE * (EVENT_SIZE + NAME_MAX + 1))

/* For now its hardcoded */
const char *version = "dirwarden v0.1.0";

static char *paths[MAX_PATHS];
static size_t path_count = 0;
static unsigned int interval = 60;
static volatile sig_atomic_t running = 1;
static int foreground = 0;

typedef struct watch_t {
    int wd;
    int dirfd;
    char *path;
} watch_t;

typedef struct pending_entry {
    int wd;
    char *name;
    dev_t dev;
    ino_t ino;
    struct timespec deadline;
} pending_entry;

static watch_t watches[MAX_PATHS];
static size_t watch_count = 0;

static pending_entry *pending = NULL;
static size_t pending_count = 0;

/* Handle graceful shutdown. */
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* Find a watched directory using its watch descriptor. */
static watch_t *find_watch(int wd)
{
    for (size_t i = 0; i < watch_count; i++) {
        if (watches[i].wd == wd)
            return &watches[i];
    }

    return NULL;
}

/* Compare two monotonic timestamps. */
static int compare_time(struct timespec a, struct timespec b)
{
    if (a.tv_sec < b.tv_sec)
        return -1;

    if (a.tv_sec > b.tv_sec)
        return 1;

    if (a.tv_nsec < b.tv_nsec)
        return -1;

    if (a.tv_nsec > b.tv_nsec)
        return 1;

    return 0;
}

/* Remove a pending file entry. */
static void remove_pending(size_t index)
{
    free(pending[index].name);

    pending[index] = pending[pending_count - 1];
    pending_count--;
}

/* Remove a file from the pending list using its directory and name. */
static void remove_pending_file(int wd, const char *name)
{
    for (size_t i = 0; i < pending_count;) {
        if (pending[i].wd == wd &&
            strcmp(pending[i].name, name) == 0) {
            remove_pending(i);
        } else {
            i++;
        }
    }
}

/* Set the timer to the nearest pending deadline. */
static int rearm_timer(int tfd)
{
    struct itimerspec timer = {0};

    if (pending_count > 0) {
        struct timespec earliest = pending[0].deadline;

        for (size_t i = 1; i < pending_count; i++) {
            if (compare_time(pending[i].deadline, earliest) < 0)
                earliest = pending[i].deadline;
        }

        timer.it_value = earliest;
    }

    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &timer, NULL) < 0) {
        syslog(LOG_ERR, "timerfd_settime: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* Add a newly created regular file to the pending list. */
static void schedule_file(int wd, const char *name, int tfd)
{
    watch_t *watch = find_watch(wd);

    if (!watch)
        return;

    struct stat st;

    if (fstatat(watch->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        syslog(LOG_WARNING, "Cannot inspect %s/%s: %s",
               watch->path, name, strerror(errno));
        return;
    }

    if (!S_ISREG(st.st_mode))
        return;

    /* Avoid duplicate entries for the same file name. */
    remove_pending_file(wd, name);

    pending_entry *new_pending =
        realloc(pending, (pending_count + 1) * sizeof(*pending));

    if (!new_pending) {
        syslog(LOG_ERR, "Cannot allocate pending file entry");
        return;
    }

    pending = new_pending;

    pending_entry *file = &pending[pending_count];

    file->name = strdup(name);

    if (!file->name) {
        syslog(LOG_ERR, "Cannot allocate file name");
        return;
    }

    file->wd = wd;
    file->dev = st.st_dev;
    file->ino = st.st_ino;

    clock_gettime(CLOCK_MONOTONIC, &file->deadline);
    file->deadline.tv_sec += interval;

    pending_count++;

    syslog(LOG_INFO, "[FILE CREATED] %s/%s; deletion in %u seconds",
           watch->path, name, interval);

    rearm_timer(tfd);
}

/* Delete files whose individual deadlines have expired. */
static void expire_files(int tfd)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (size_t i = 0; i < pending_count;) {
        pending_entry *file = &pending[i];

        if (compare_time(now, file->deadline) < 0) {
            i++;
            continue;
        }

        watch_t *watch = find_watch(file->wd);

        if (watch) {
            struct stat st;

            if (fstatat(watch->dirfd, file->name, &st,
                        AT_SYMLINK_NOFOLLOW) == 0) {

                /* Make sure the file is still the same file we scheduled. */
                if (S_ISREG(st.st_mode) &&
                    st.st_dev == file->dev &&
                    st.st_ino == file->ino) {

                    if (unlinkat(watch->dirfd, file->name, 0) == 0) {
                        syslog(LOG_INFO, "[FILE DELETED] %s/%s",
                               watch->path, file->name);
                    } else {
                        syslog(LOG_WARNING, "Cannot delete %s/%s: %s",
                               watch->path, file->name, strerror(errno));
                    }
                }
            }
        }

        remove_pending(i);
    }

    rearm_timer(tfd);
}

/* Read and process inotify events. */
static void process_inotify(int ifd, int tfd)
{
    char buffer[BUF_LEN]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    while (1) {
        ssize_t len = read(ifd, buffer, sizeof(buffer));

        if (len < 0) {
            if (errno == EAGAIN)
                break;

            if (errno == EINTR)
                continue;

            syslog(LOG_ERR, "inotify read: %s", strerror(errno));
            break;
        }

        if (len == 0)
            break;

        for (char *ptr = buffer; ptr < buffer + len;) {
            struct inotify_event *event =
                (struct inotify_event *)ptr;

            if (event->mask & IN_Q_OVERFLOW) {
                syslog(LOG_WARNING, "Inotify event queue overflow");
            }

            if (event->len > 0) {
                watch_t *watch = find_watch(event->wd);
                watch = NULL;

                if (event->mask & IN_CREATE) {
                    if (event->mask & IN_ISDIR) {
                        syslog(LOG_INFO, "[DIRECTORY CREATED] %s",
                               event->name);
                    } else {
                        schedule_file(event->wd, event->name, tfd);
                    }
                }

                if (event->mask & IN_DELETE) {
                    if (event->mask & IN_ISDIR) {
                        syslog(LOG_INFO, "[DIRECTORY DELETED] %s",
                               event->name);
                    } else {
                        syslog(LOG_INFO, "[FILE DELETED] %s",
                               event->name);
                    }

                    remove_pending_file(event->wd, event->name);
                    rearm_timer(tfd);
                }

                if (event->mask & IN_MOVED_FROM) {
                    remove_pending_file(event->wd, event->name);
                    rearm_timer(tfd);
                }

                if (event->mask & IN_MOVED_TO) {
                    if (!(event->mask & IN_ISDIR))
                        schedule_file(event->wd, event->name, tfd);
                }
            }

            ptr += sizeof(struct inotify_event) + event->len;
        }
    }
}

/* Convert a user-supplied path to an absolute canonical path. */
static char *validate_path(const char *path)
{
    char *resolved = realpath(path, NULL);

    if (!resolved) {
        fprintf(stderr, "Invalid path %s: %s\n",
                path, strerror(errno));
        return NULL;
    }

    if (!strcmp(resolved, "/")) {
        fprintf(stderr, "Refusing to monitor the root directory.\n");
        free(resolved);
        return NULL;
    }

    struct stat st;

    if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Not a valid directory: %s\n", path);
        free(resolved);
        return NULL;
    }

    return resolved;
}

/* Detach the process from the terminal. */
static int daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0)
        return -1;

    signal(SIGHUP, SIG_IGN);

    pid = fork();

    if (pid < 0)
        return -1;

    if (pid > 0)
        exit(EXIT_SUCCESS);

    umask(027);

    if (chdir("/") < 0)
        return -1;

    long maxfd = sysconf(_SC_OPEN_MAX);

    if (maxfd < 0)
        maxfd = 1024;

    for (int fd = 0; fd < maxfd; fd++)
        close(fd);

    int nullfd = open("/dev/null", O_RDWR);

    if (nullfd < 0)
        return -1;

    if (dup2(nullfd, STDIN_FILENO) < 0 ||
        dup2(nullfd, STDOUT_FILENO) < 0 ||
        dup2(nullfd, STDERR_FILENO) < 0) {
        close(nullfd);
        return -1;
    }

    if (nullfd > STDERR_FILENO)
        close(nullfd);

    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s -p <directory> [-p <directory> ...] "
        "-t <seconds> [-f]\n\n"
        "Options:\n"
        "  -p PATH    Directory to monitor (repeatable)\n"
        "  -t SEC     File deletion time in seconds\n"
        "  -f         Run in foreground\n"
        "  -h         Show this help\n"
        "  -v         Version\n",
        program);
}

int main(int argc, char **argv)
{
    int opt;
    int ifd = -1, tfd = -1, epfd = -1;

    while ((opt = getopt(argc, argv, "p:t:fhv")) != -1) {
        switch (opt) {
        case 'p':
            if (path_count >= MAX_PATHS) {
                fprintf(stderr, "Maximum %d paths allowed.\n",
                        MAX_PATHS);
                return EXIT_FAILURE;
            }

            paths[path_count] = validate_path(optarg);

            if (!paths[path_count])
                return EXIT_FAILURE;

            path_count++;
            break;

        case 't': {
            char *end;
            errno = 0;

            unsigned long value = strtoul(optarg, &end, 10);

            if (errno || *optarg == '\0' || *end != '\0' ||
                value == 0 || value > UINT_MAX) {
                fprintf(stderr, "Invalid interval: %s\n", optarg);
                return EXIT_FAILURE;
            }

            interval = (unsigned int)value;
            break;
        }

        case 'f':
            foreground = 1;
            break;

        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;

        case 'v':
            printf("%s\n", version);
            return EXIT_SUCCESS;

        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (path_count == 0) {
        fprintf(stderr, "At least one -p path is required.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected positional argument: %s\n",
                argv[optind]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!foreground && daemonize() < 0) {
        perror("daemonize");
        return EXIT_FAILURE;
    }

    openlog(APP_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);

    /* Register singals */
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, SIG_IGN);

    syslog(LOG_INFO,
           "%s started. Paths: %zu, interval: %u seconds",
           APP_NAME, path_count, interval);


    /* Initialize inotify. */
    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    if (ifd < 0) {
        syslog(LOG_ERR, "inotify_init1: %s", strerror(errno));
        goto cleanup;
    }

    /* Add watches for all configured directories. */
    for (size_t i = 0; i < path_count; i++) {
        int wd = inotify_add_watch(
            ifd,
            paths[i],
            IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
        );

        if (wd < 0) {
            syslog(LOG_ERR, "inotify_add_watch %s: %s",
                   paths[i], strerror(errno));
            goto cleanup;
        }

        int dirfd = open(paths[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (dirfd < 0) {
            syslog(LOG_ERR, "Cannot open %s: %s",
                   paths[i], strerror(errno));
            goto cleanup;
        }

        watches[watch_count].wd = wd;
        watches[watch_count].dirfd = dirfd;
        watches[watch_count].path = paths[i];

        watch_count++;

        syslog(LOG_DEBUG, "watch_t descriptor: %d, path: %s",
               wd, paths[i]);
    }

    /* Initialize timerfd. */
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (tfd < 0) {
        syslog(LOG_ERR, "timerfd_create: %s", strerror(errno));
        goto cleanup;
    }

    /* Initialize epoll. */
    epfd = epoll_create1(EPOLL_CLOEXEC);

    if (epfd < 0) {
        syslog(LOG_ERR, "epoll_create1: %s", strerror(errno));
        goto cleanup;
    }

    struct epoll_event ev;

    /* Register inotify fd with epoll. */
    ev.events = EPOLLIN;
    ev.data.fd = ifd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &ev) < 0) {
        syslog(LOG_ERR, "epoll_ctl inotify: %s", strerror(errno));
        goto cleanup;
    }

    /* Register timerfd with epoll. */
    ev.events = EPOLLIN;
    ev.data.fd = tfd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
        syslog(LOG_ERR, "epoll_ctl timerfd: %s", strerror(errno));
        goto cleanup;
    }

    struct epoll_event events[MAX_EVENTS];

    /* Event Loop */
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            syslog(LOG_ERR, "epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* Inotify Event */
            if (fd == ifd) {
                process_inotify(ifd, tfd);
            }

            /* Timer Event */
            else if (fd == tfd) {
                uint64_t expirations;
                ssize_t s;

                do {
                    s = read(tfd, &expirations, sizeof(expirations));
                } while (s < 0 && errno == EINTR);

                if (s == sizeof(expirations)) {
                    expire_files(tfd);
                } else if (s < 0 && errno != EAGAIN) {
                    syslog(LOG_ERR, "timerfd read: %s", strerror(errno));
                }
            }
        }
    }

    syslog(LOG_INFO, "%s shutting down.", APP_NAME);

cleanup:
    if (ifd >= 0) {
        for (size_t i = 0; i < watch_count; i++)
            inotify_rm_watch(ifd, watches[i].wd);
    }

    for (size_t i = 0; i < watch_count; i++)
        close(watches[i].dirfd);

    if (ifd >= 0)
        close(ifd);

    if (tfd >= 0)
        close(tfd);

    if (epfd >= 0)
        close(epfd);

    for (size_t i = 0; i < pending_count; i++)
        free(pending[i].name);

    free(pending);

    for (size_t i = 0; i < path_count; i++)
        free(paths[i]);

    closelog();

    return EXIT_SUCCESS;
}
