/*
 * TermmiK
 * Copyright (C) 2026 Szymon Grajner (SfymmiK)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE 600
#include "alloc.h"
#include "pty.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <string.h>

int pty_spawn(int *master_fd, pid_t *pid, int rows, int cols) {
    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0) return -1;

    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0) {
        close(mfd);
        return -1;
    }

    char *slavename = ptsname(mfd);
    if (!slavename) {
        close(mfd);
        return -1;
    }

    int sfd = open(slavename, O_RDWR | O_NOCTTY);
    if (sfd < 0) {
        close(mfd);
        return -1;
    }

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = cols * 9;
    ws.ws_ypixel = rows * 18;
    ioctl(sfd, TIOCSWINSZ, &ws);

    struct termios t;
    if (tcgetattr(sfd, &t) == 0) {
        t.c_lflag |= (ECHO | ICANON | ISIG | IEXTEN);
        t.c_oflag |= OPOST;
        t.c_iflag |= (BRKINT | ICRNL | IXON | IXANY);
        tcsetattr(sfd, TCSANOW, &t);
    }

    pid_t p = fork();
    if (p < 0) {
        close(mfd);
        close(sfd);
        return -1;
    }

    if (p == 0) {
        // Child
        close(mfd);
        setsid();
        ioctl(sfd, TIOCSCTTY, 0);

        dup2(sfd, STDIN_FILENO);
        dup2(sfd, STDOUT_FILENO);
        dup2(sfd, STDERR_FILENO);
        if (sfd > STDERR_FILENO) {
            close(sfd);
        }

        setenv("TERM", "xterm-256color", 1);
        char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/sh";

        execlp(shell, shell, NULL);
        exit(1);
    }

    // Parent
    close(sfd);
    *master_fd = mfd;
    *pid = p;
    return 0;
}

ssize_t pty_read(int fd, char *buf, size_t count) {
    return read(fd, buf, count);
}

ssize_t pty_write(int fd, const char *buf, size_t count) {
    return write(fd, buf, count);
}

void pty_resize(int fd, int rows, int cols) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = cols * 9;
    ws.ws_ypixel = rows * 18;
    ioctl(fd, TIOCSWINSZ, &ws);
}
