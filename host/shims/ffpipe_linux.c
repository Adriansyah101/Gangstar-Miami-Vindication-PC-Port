/* ffpipe_linux.c -- Linux: sound and pictures from the system's ffmpeg program.

   Windows decodes the intro movie and the custom radio's songs with Media Foundation.  Linux has no decoder that
   every system carries, so the host runs ffmpeg (or ffprobe) as a program of its own and reads raw samples or frames
   from a pipe.  A program, not its libraries: the host is 32-bit, ffmpeg usually is not, and a pipe does not care;
   nor does it care which FFmpeg version is installed.  With no ffmpeg on the PATH the callers go on without it. */

#ifndef _WIN32
#define _GNU_SOURCE             /* pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shims.h"

extern char **environ;

int gmv_ff_start(const char *const *argv, int *fd)
{
    int p[2];
    *fd = -1;
    if (pipe2(p, O_CLOEXEC))
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (err) {
        close(p[0]);
        errno = err;
        return -1;
    }
    *fd = p[0];
    return (int)pid;
}

size_t gmv_ff_read(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k > 0)
            got += (size_t)k;
        else if (k < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    return got;
}

int gmv_ff_stop(int pid, int fd)
{
    if (fd >= 0)
        close(fd);              /* a program still writing ends on the broken pipe */
    if (pid <= 0)
        return -1;
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

char *gmv_ff_output(const char *const *argv)
{
    int fd, pid = gmv_ff_start(argv, &fd);
    if (pid < 0)
        return NULL;
    size_t cap = 4096, len = 0;
    char *s = malloc(cap);
    while (s) {
        size_t k = gmv_ff_read(fd, s + len, cap - 1 - len);
        len += k;
        if (len < cap - 1)
            break;
        char *t = realloc(s, cap * 2);
        if (!t) {
            free(s);
            s = NULL;
            break;
        }
        s = t;
        cap *= 2;
    }
    if (gmv_ff_stop(pid, fd) != 0) {
        free(s);
        errno = EIO;            /* it ran, and failed: not "not installed" */
        return NULL;
    }
    if (s)
        s[len] = 0;
    return s;
}
#endif
