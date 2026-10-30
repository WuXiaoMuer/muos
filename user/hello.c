/* hello.c - first C program running in Ring3 under MuOS.
 * Built freestanding for the Linux i386 syscall ABI (int 0x80). */
#include "muos.h"

#define USER_MARKER "USER-HELLO-OK"

static unsigned read_cs(void) {
    unsigned cs;
    __asm__ volatile ("push %%cs\n\tpop %0" : "=r"(cs));
    return cs & 0xFFFF;
}

static int arg_is(const char* s, const char* t) {
    if (!s) return 0;
    while (*s && *t && *s == *t) { s++; t++; }
    return *s == '\0' && *t == '\0';
}

int main(int argc, char** argv) {
    unsigned cs = read_cs();

    if (argc > 1 && arg_is(argv[1], "exec-child")) {
        muos_puts("USER-EXEC-OK\n");
        return 42;
    }

    if (argc > 1 && arg_is(argv[1], "pipe")) {
        int fds[2] = { -1, -1 };
        int rc = muos_pipe(fds);
        muos_puts("pipe-test: rc="); muos_putdec(rc);
        muos_puts(" fds="); muos_putdec(fds[0]); muos_puts("/"); muos_putdec(fds[1]);
        muos_puts("\n");
        if (rc == 0) {
            const char msg[] = "PIPE-OK";
            muos_write(fds[1], msg, sizeof(msg) - 1);
            char rb[16];
            int n = muos_read(fds[0], rb, sizeof(rb) - 1);
            rb[n > 0 ? n : 0] = '\0';
            muos_puts("pipe-test: read="); muos_puts(rb); muos_puts("\n");
            muos_close(fds[0]); muos_close(fds[1]);
        }
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "execbin")) {
        int pid = muos_fork();
        if (pid == 0) {
            char* args[3];
            args[0] = "echo";
            args[1] = "CHILD-EXEC-BIN-OK";
            args[2] = 0;
            muos_execve("/bin/echo", args, 0);
            muos_exit(99);
        }
        int status = 0;
        muos_waitpid(pid, &status, 0);
        muos_puts("execbin: status="); muos_putdec((status >> 8) & 0xFF);
        muos_puts("\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "pipefork")) {
        int fds[2] = { -1, -1 };
        if (muos_pipe(fds) != 0) { muos_puts("PIPE-FORK-BAD\n"); return 0; }
        int pid = muos_fork();
        if (pid == 0) {
            muos_close(fds[0]);
            const char msg[] = "PIPE-FORK-OK";
            muos_write(fds[1], msg, sizeof(msg) - 1);
            muos_close(fds[1]);
            muos_exit(0);
        }
        muos_close(fds[1]);
        int status = 0;
        muos_waitpid(pid, &status, 0);
        char rb[24];
        int n = muos_read(fds[0], rb, sizeof(rb) - 1);
        rb[n > 0 ? n : 0] = '\0';
        muos_puts("pipe-fork: read="); muos_puts(rb); muos_puts("\n");
        muos_close(fds[0]);
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "sched")) {
        /* Two children spin in Ring3 (no syscalls) so the 100Hz timer
         * preempts them and interleaves their output. The parent blocks in
         * waitpid while both run. */
        int pids[2] = { -1, -1 };
        for (int k = 0; k < 2; k++) {
            int pid = muos_fork();
            if (pid == 0) {
                for (int i = 0; i < 4; i++) {
                    volatile unsigned spin = 0;
                    for (unsigned n = 0; n < 900000u; n++) spin += n;
                    muos_puts(spin ? "child" : "child");
                    muos_putdec(k); muos_puts(":"); muos_putdec(i);
                    muos_puts(" ");
                }
                muos_puts("\n");
                muos_exit(10 + k);
            }
            pids[k] = pid;
        }

        int ok = 1;
        int seen10 = 0, seen11 = 0;
        for (int k = 0; k < 2; k++) {
            int status = 0;
            int w = muos_waitpid(-1, &status, 0);
            int code = (status >> 8) & 0xFF;
            muos_puts("sched: wait="); muos_putdec(w);
            muos_puts(" status="); muos_putdec(code); muos_puts("\n");
            if (w <= 0) ok = 0;
            if (code == 10) seen10 = 1;
            if (code == 11) seen11 = 1;
        }
        ok = ok && seen10 && seen11;
        muos_puts(ok ? "SCHED-OK\n" : "SCHED-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "p1")) {
        /* P1 ABI close-out: directory rename, unlink-while-open, pipe HUP,
         * bounded poll timeout and waitid. */
        int ok = 1;
        int rc;
        char buf[64];
        unsigned si[32];
        muos_pollfd_t pf;

        muos_unlinkat(MUOS_AT_FDCWD, "p1new/f.txt", 0);
        muos_unlinkat(MUOS_AT_FDCWD, "p1old/f.txt", 0);
        muos_unlinkat(MUOS_AT_FDCWD, "p1new", MUOS_AT_REMOVEDIR);
        muos_unlinkat(MUOS_AT_FDCWD, "p1old", MUOS_AT_REMOVEDIR);
        muos_unlinkat(MUOS_AT_FDCWD, "p1file", 0);

        rc = muos_mkdirat(MUOS_AT_FDCWD, "p1old", 0755);
        ok = ok && rc == 0;
        int dfd = muos_openat(MUOS_AT_FDCWD, "p1old", MUOS_O_DIRECTORY);
        int fd = -1;
        if (dfd >= 0) fd = muos_openat(dfd, "f.txt",
                                       MUOS_O_CREAT | MUOS_O_WRONLY);
        if (fd >= 0) {
            const char msg[] = "DIR-RENAME";
            muos_write(fd, msg, sizeof(msg) - 1);
            muos_close(fd);
        }
        if (dfd >= 0) muos_close(dfd);

        rc = muos_renameat(MUOS_AT_FDCWD, "p1old", MUOS_AT_FDCWD, "p1new");
        muos_puts("p1: dir-rename="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        fd = muos_open("/p1new/f.txt", 0);
        int got = 0;
        if (fd >= 0) {
            got = muos_read(fd, buf, sizeof(buf));
            muos_close(fd);
        }
        muos_puts("p1: renamed-content="); muos_putdec(got); muos_puts("\n");
        ok = ok && got == 10;             /* "DIR-RENAME" */
        rc = muos_open("/p1old/f.txt", 0);
        ok = ok && rc == -2;
        muos_unlinkat(MUOS_AT_FDCWD, "p1new/f.txt", 0);
        muos_unlinkat(MUOS_AT_FDCWD, "p1new", MUOS_AT_REMOVEDIR);

        /* unlink while an fd is open: the data must stay readable */
        fd = muos_open("p1file", MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        ok = ok && fd >= 0;
        if (fd >= 0) {
            const char msg[] = "OPEN-DATA";
            muos_write(fd, msg, sizeof(msg) - 1);
            muos_lseek(fd, 0, 0);
            rc = muos_unlinkat(MUOS_AT_FDCWD, "p1file", 0);
            muos_puts("p1: unlink-open="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;

            int n = muos_read(fd, buf, sizeof(buf));
            int same = 1;
            for (int i = 0; i < 9; i++) if (buf[i] != msg[i]) same = 0;
            muos_puts("p1: open-read="); muos_putdec(n);
            muos_puts(same ? " data-ok\n" : " data-bad\n");
            ok = ok && n == 9 && same;
            muos_close(fd);
        }
        rc = muos_open("/p1file", 0);
        ok = ok && rc == -2;

        /* pipe with no writers: POLLHUP and EOF read */
        int pfds[2] = { -1, -1 };
        if (muos_pipe(pfds) == 0) {
            muos_close(pfds[1]);
            pf.fd = pfds[0]; pf.events = MUOS_POLLIN; pf.revents = 0;
            rc = muos_poll(&pf, 1, 0);
            muos_puts("p1: hup="); muos_putdec(rc);
            muos_puts(" revents="); muos_puthex((unsigned)pf.revents);
            muos_puts("\n");
            ok = ok && rc == 1 && (pf.revents & MUOS_POLLHUP);
            int n = muos_read(pfds[0], buf, sizeof(buf));
            ok = ok && n == 0;
            muos_close(pfds[0]);
        }

        /* bounded poll timeout really waits on the PIT */
        pfds[0] = pfds[1] = -1;
        if (muos_pipe(pfds) == 0) {
            pf.fd = pfds[0]; pf.events = MUOS_POLLIN; pf.revents = 0;
            unsigned t0 = muos_times(0);
            rc = muos_poll(&pf, 1, 30);
            unsigned t1 = muos_times(0);
            muos_puts("p1: timeout="); muos_putdec(rc);
            muos_puts(" ticks="); muos_putdec((int)(t1 - t0)); muos_puts("\n");
            ok = ok && rc == 0 && (t1 - t0) >= 2 && (t1 - t0) <= 8;
            muos_close(pfds[0]); muos_close(pfds[1]);
        }

        /* waitid: SIGCHLD siginfo with pid and exit status */
        int pid = muos_fork();
        if (pid == 0) {
            muos_exit(7);
        } else if (pid > 0) {
            for (unsigned i = 0; i < 32; i++) si[i] = 0;
            rc = muos_waitid(1, pid, si, 0);
            muos_puts("p1: waitid="); muos_putdec(rc);
            muos_puts(" sig="); muos_putdec((int)si[0]);
            muos_puts(" pid="); muos_putdec((int)si[3]);
            muos_puts(" status="); muos_putdec((int)si[5]); muos_puts("\n");
            ok = ok && rc == 0 && si[0] == 17 && si[2] == 1 &&
                 si[3] == (unsigned)pid && si[5] == 7;
        } else {
            ok = 0;
        }

        muos_puts(ok ? "P1-API-OK\n" : "P1-API-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "extra")) {
        /* nanosleep, readlink, chown family, getrusage and times. */
        int ok = 1;
        int rc;
        unsigned req[2], rem[2];
        unsigned rusage[18];
        unsigned tms[4];
        char rdbuf[16];

        req[0] = 0; req[1] = 30000000;      /* 30ms */
        rem[0] = 0xAAAAAAAA; rem[1] = 0xAAAAAAAA;
        rc = muos_nanosleep(req, rem);
        muos_puts("extra: nanosleep="); muos_putdec(rc);
        muos_puts(" rem="); muos_putdec((int)rem[0]);
        muos_puts("/"); muos_putdec((int)rem[1]); muos_puts("\n");
        ok = ok && rc == 0 && rem[0] == 0 && rem[1] == 0;

        req[1] = 1000000000;                /* nsec out of range */
        rc = muos_nanosleep(req, rem);
        muos_puts("extra: nanosleep-EINVAL="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -22;

        rc = muos_readlink("/notes.txt", rdbuf, sizeof(rdbuf));
        muos_puts("extra: readlink="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -22;               /* not a symlink */

        rc = muos_chown("/notes.txt", 0, 0);
        muos_puts("extra: chown="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        rc = muos_chown("/missing.txt", 0, 0);
        muos_puts("extra: chown-ENOENT="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -2;

        int fd = muos_open("/notes.txt", 0);
        muos_puts("extra: open="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;
        if (fd >= 0) {
            rc = muos_fchown(fd, 0, 0);
            muos_puts("extra: fchown="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;
            muos_close(fd);
        }

        rc = muos_fchownat(MUOS_AT_FDCWD, "notes.txt", 0, 0, 0);
        muos_puts("extra: fchownat="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        rc = muos_getrusage(0, rusage);
        muos_puts("extra: getrusage="); muos_putdec(rc);
        muos_puts(" utime="); muos_putdec((int)rusage[0]); muos_puts("\n");
        ok = ok && rc == 0 && rusage[0] == 0;

        rc = muos_getrusage(5, rusage);
        muos_puts("extra: getrusage-EINVAL="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -22;

        unsigned ticks = muos_times(tms);
        muos_puts("extra: times="); muos_putdec((int)ticks);
        muos_puts(" tms0="); muos_putdec((int)tms[0]); muos_puts("\n");
        ok = ok && ticks > 0 && tms[0] == 0;

        muos_puts(ok ? "EXTRA-API-OK\n" : "EXTRA-API-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "poll")) {
        /* poll/select/pselect6/ppoll readiness on pipe read/write ends. */
        int ok = 1;
        int rc;
        int fds[2] = { -1, -1 };
        muos_pollfd_t pf[2];
        unsigned rset;
        char c = 'Z';
        char rb = 0;

        rc = muos_pipe(fds);
        muos_puts("poll: pipe="); muos_putdec(rc);
        muos_puts(" fds="); muos_putdec(fds[0]); muos_puts("/"); muos_putdec(fds[1]);
        muos_puts("\n");
        ok = ok && rc == 0;

        if (rc == 0) {
            pf[0].fd = fds[0]; pf[0].events = MUOS_POLLIN;  pf[0].revents = 0;
            pf[1].fd = fds[1]; pf[1].events = MUOS_POLLOUT; pf[1].revents = 0;
            rc = muos_poll(pf, 2, 0);
            muos_puts("poll: empty="); muos_putdec(rc);
            muos_puts(" revents="); muos_putdec(pf[0].revents);
            muos_puts("/"); muos_putdec(pf[1].revents); muos_puts("\n");
            ok = ok && rc == 1 && pf[0].revents == 0
                 && pf[1].revents == MUOS_POLLOUT;

            muos_write(fds[1], &c, 1);
            pf[0].revents = pf[1].revents = 0;
            rc = muos_poll(pf, 2, 0);
            muos_puts("poll: filled="); muos_putdec(rc);
            muos_puts(" revents="); muos_putdec(pf[0].revents);
            muos_puts("/"); muos_putdec(pf[1].revents); muos_puts("\n");
            ok = ok && rc == 2 && (pf[0].revents & MUOS_POLLIN)
                 && (pf[1].revents & MUOS_POLLOUT);

            rset = 1u << fds[0];
            rc = muos_select(fds[0] + 1, &rset, 0, 0, 0);
            muos_puts("poll: select="); muos_putdec(rc);
            muos_puts(" set="); muos_puthex(rset); muos_puts("\n");
            ok = ok && rc == 1 && rset == (1u << fds[0]);

            muos_read(fds[0], &rb, 1);
            rset = 1u << fds[0];
            rc = muos_select(fds[0] + 1, &rset, 0, 0, 0);
            muos_puts("poll: select-empty="); muos_putdec(rc);
            muos_puts(" set="); muos_puthex(rset); muos_puts("\n");
            ok = ok && rc == 0 && rset == 0;

            muos_write(fds[1], &c, 1);
            rset = 1u << fds[0];
            rc = muos_pselect6(fds[0] + 1, &rset, 0, 0, 0, 0);
            muos_puts("poll: pselect6="); muos_putdec(rc);
            muos_puts(" set="); muos_puthex(rset); muos_puts("\n");
            ok = ok && rc == 1;

            pf[0].fd = fds[0]; pf[0].events = MUOS_POLLIN; pf[0].revents = 0;
            rc = muos_ppoll(pf, 1, 0);
            muos_puts("poll: ppoll="); muos_putdec(rc);
            muos_puts(" revents="); muos_putdec(pf[0].revents); muos_puts("\n");
            ok = ok && rc == 1 && (pf[0].revents & MUOS_POLLIN);

            muos_read(fds[0], &rb, 1);

            pf[0].fd = 99; pf[0].events = MUOS_POLLIN; pf[0].revents = 0;
            rc = muos_poll(pf, 1, 0);
            muos_puts("poll: invalid="); muos_putdec(rc);
            muos_puts(" revents="); muos_putdec(pf[0].revents); muos_puts("\n");
            ok = ok && rc == 1 && pf[0].revents == MUOS_POLLNVAL;

            pf[0].fd = -1; pf[0].events = MUOS_POLLIN; pf[0].revents = 0;
            rc = muos_poll(pf, 1, 0);
            muos_puts("poll: negative="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;

            rc = muos_poll(0, 0, 0);
            muos_puts("poll: nfds0="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;

            muos_close(fds[0]);
            muos_close(fds[1]);
        }

        muos_puts(ok ? "POLL-READY-OK\n" : "POLL-READY-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "mmapfile")) {
        /* File-backed mmap, truncate (shrink/grow), chmod, fsync and rlimits. */
        int ok = 1;
        int rc;
        const char msg[] = "FILE-MMAP-CONTENT";
        const unsigned mlen = sizeof(msg) - 1;
        union {
            char raw[MUOS_STAT64_SIZE];
            unsigned word[MUOS_STAT64_SIZE / 4];
        } st;

        int wfd = muos_open("mmap.txt", MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        muos_puts("mfile: create="); muos_putdec(wfd); muos_puts("\n");
        ok = ok && wfd >= 0;
        if (wfd >= 0) {
            muos_write(wfd, msg, mlen);
            muos_close(wfd);
        }

        int fd = muos_open("mmap.txt", 0);
        muos_puts("mfile: open="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;

        if (fd >= 0) {
            char* p = (char*)muos_mmap_file(fd, 4096, MUOS_PROT_READ,
                                            MUOS_MAP_PRIVATE, 0);
            int same = (p && (unsigned)p >= 0x08048000u) ? 1 : 0;
            for (unsigned i = 0; same && i < mlen; i++)
                if (p[i] != msg[i]) same = 0;
            if (same && p[mlen] != 0) same = 0;      /* zero-filled tail */
            muos_puts("mfile: mmap="); muos_puthex((unsigned)p);
            muos_puts(same ? " content-ok\n" : " content-bad\n");
            ok = ok && same;
            if (p) muos_munmap((unsigned)p, 4096);

            rc = muos_ftruncate(fd, 8);
            muos_puts("mfile: ftruncate-shrink="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;

            char* p2 = (char*)muos_mmap_file(fd, 4096, MUOS_PROT_READ,
                                             MUOS_MAP_PRIVATE, 0);
            int shrunk = (p2 && p2[0] == 'F' && p2[7] == 'A' && p2[8] == 0) ? 1 : 0;
            muos_puts(shrunk ? "mfile: shrink-ok\n" : "mfile: shrink-bad\n");
            ok = ok && shrunk;
            if (p2) muos_munmap((unsigned)p2, 4096);

            rc = muos_ftruncate(fd, 64);
            muos_puts("mfile: ftruncate-grow="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;

            char* p3 = (char*)muos_mmap_file(fd, 4096, MUOS_PROT_READ,
                                             MUOS_MAP_PRIVATE, 0);
            int grown = (p3 && p3[8] == 0 && p3[20] == 0 && p3[63] == 0) ? 1 : 0;
            muos_puts(grown ? "mfile: grow-zeros-ok\n" : "mfile: grow-zeros-bad\n");
            ok = ok && grown;
            if (p3) muos_munmap((unsigned)p3, 4096);

            rc = muos_fsync(fd);
            muos_puts("mfile: fsync="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;
            muos_close(fd);
        }

        rc = muos_chmod("mmap.txt", 0644);
        muos_puts("mfile: chmod="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        rc = muos_truncate("mmap.txt", 32);
        muos_puts("mfile: truncate="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        rc = muos_fstatat64(MUOS_AT_FDCWD, "mmap.txt", st.raw, 0);
        muos_puts("mfile: stat-size="); muos_putdec((int)st.word[11]); muos_puts("\n");
        ok = ok && rc == 0 && st.word[11] == 32;

        rc = muos_truncate("/no-such.txt", 8);
        muos_puts("mfile: truncate-ENOENT="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -2;

        rc = muos_ftruncate(0, 8);
        muos_puts("mfile: ftruncate-stdin="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -22;

        unsigned lim[2];
        rc = muos_getrlimit(MUOS_RLIMIT_NOFILE, lim);
        muos_puts("mfile: getrlimit="); muos_putdec(rc);
        muos_puts(" nofile="); muos_putdec((int)lim[0]); muos_puts("\n");
        ok = ok && rc == 0 && lim[0] == 11;

        unsigned lim64[4];
        rc = muos_prlimit64(MUOS_RLIMIT_NOFILE, lim64);
        muos_puts("mfile: prlimit64="); muos_putdec(rc);
        muos_puts(" nofile="); muos_putdec((int)lim64[0]); muos_puts("\n");
        ok = ok && rc == 0 && lim64[0] == 11u;

        muos_puts(ok ? "FILE-MMAP-OK\n" : "FILE-MMAP-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "at")) {
        /* Exercised entirely through the i386 *at syscalls: mkdirat,
         * openat with a directory fd, fstatat64, fchmodat, readlinkat,
         * renameat, faccessat and unlinkat (file + AT_REMOVEDIR). */
        union {
            char raw[MUOS_STAT64_SIZE];
            unsigned word[MUOS_STAT64_SIZE / 4];
        } st;
        int ok = 1;
        int rc;

        /* Make the test repeatable when the shell is used interactively. */
        muos_unlinkat(MUOS_AT_FDCWD, "atdir/g.txt", 0);
        muos_unlinkat(MUOS_AT_FDCWD, "atdir/f.txt", 0);
        muos_unlinkat(MUOS_AT_FDCWD, "atdir", MUOS_AT_REMOVEDIR);

        rc = muos_mkdirat(MUOS_AT_FDCWD, "atdir", 0755);
        muos_puts("at: mkdirat="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        int dfd = muos_openat(MUOS_AT_FDCWD, "atdir", MUOS_O_DIRECTORY);
        muos_puts("at: dirfd="); muos_putdec(dfd); muos_puts("\n");
        ok = ok && dfd >= 0;

        if (dfd >= 0) {
            int fd = muos_openat(dfd, "f.txt", MUOS_O_CREAT | MUOS_O_WRONLY);
            muos_puts("at: create="); muos_putdec(fd); muos_puts("\n");
            ok = ok && fd >= 0;
            if (fd >= 0) {
                const char msg[] = "AT-FAMILY";
                muos_write(fd, msg, sizeof(msg) - 1);
                muos_close(fd);

                rc = muos_fstatat64(dfd, "f.txt", st.raw, 0);
                muos_puts("at: fstatat="); muos_putdec(rc);
                muos_puts(" size="); muos_putdec((int)st.word[11]);
                muos_puts("\n");
                ok = ok && rc == 0 && st.word[11] == sizeof(msg) - 1;

                rc = muos_fstatat64(dfd, "", st.raw, MUOS_AT_EMPTY_PATH);
                muos_puts("at: fstatat-dir-fd="); muos_putdec(rc);
                muos_puts(" mode="); muos_puthex(st.word[4]);
                muos_puts("\n");
                ok = ok && rc == 0 && (st.word[4] & 0xF000) == 0x4000;

                rc = muos_fchmodat(dfd, "f.txt", 0644);
                muos_puts("at: fchmodat="); muos_putdec(rc); muos_puts("\n");
                ok = ok && rc == 0;

                rc = muos_readlinkat(dfd, "f.txt", st.raw, 16);
                muos_puts("at: readlinkat="); muos_putdec(rc); muos_puts("\n");
                ok = ok && rc == -22;      /* not a symlink: EINVAL */

                rc = muos_renameat(dfd, "f.txt", MUOS_AT_FDCWD, "atdir/g.txt");
                muos_puts("at: renameat="); muos_putdec(rc); muos_puts("\n");
                ok = ok && rc == 0;

                rc = muos_faccessat(MUOS_AT_FDCWD, "atdir/g.txt", 0);
                muos_puts("at: faccessat="); muos_putdec(rc); muos_puts("\n");
                ok = ok && rc == 0;

                rc = muos_unlinkat(MUOS_AT_FDCWD, "atdir/g.txt", 0);
                muos_puts("at: unlinkat="); muos_putdec(rc); muos_puts("\n");
                ok = ok && rc == 0;
            }
            muos_close(dfd);
        }

        rc = muos_unlinkat(MUOS_AT_FDCWD, "atdir", MUOS_AT_REMOVEDIR);
        muos_puts("at: remove-dir="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        muos_puts(ok ? "AT-FAMILY-OK\n" : "AT-FAMILY-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "fork")) {
        muos_puts("fork-test: parent before fork\n");
        int pid = muos_fork();
        if (pid == 0) {
            char* args[3];
            args[0] = "hello.elf";
            args[1] = "exec-child";
            args[2] = 0;
            muos_puts("fork-test: child execve /hello.elf\n");
            int r = muos_execve("/hello.elf", args, 0);
            muos_puts("fork-test: execve failed "); muos_putdec(r); muos_puts("\n");
            muos_exit(99);
        }
        muos_puts("fork-test: parent child pid="); muos_putdec(pid); muos_puts("\n");
        int status = 0;
        int w = muos_waitpid(pid, &status, 0);
        muos_puts("fork-test: wait="); muos_putdec(w);
        muos_puts(" status="); muos_putdec((status >> 8) & 0xFF);
        muos_puts(w == pid && ((status >> 8) & 0xFF) == 42
                  ? " USER-FORK-OK\n" : " USER-FORK-BAD\n");
        return 0;
    }

    muos_puts("======================================\n");
    muos_puts("  Hello from C in Ring3 (MuOS user mode)\n");
    muos_puts("======================================\n");

    muos_puts("CS=");    muos_puthex(cs);
    muos_puts(" argc="); muos_putdec(argc);
    muos_puts(" pid=");  muos_putdec(muos_getpid());
    muos_puts("\nargv[0]=");
    if (argc > 0 && argv && argv[0]) muos_puts(argv[0]);
    muos_puts("\n");

    /* `run fault`: deliberately touch an address above all RAM. The kernel
     * must kill only this process and return to the shell. */
    if (argc > 1 && argv && argv[1] && argv[1][0] == 'f') {
        muos_puts("fault demo: writing to unmapped 0xC0000000\n");
        *(volatile unsigned*)0xC0000000u = 0xDEADBEEF;
        muos_puts("BUG: the bad write did not fault\n");
    }

    /* Plain C computation, proving the user stack and text work */
    int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;

    int local[8];
    for (int i = 0; i < 8; i++) local[i] = i * i;
    int local_sum = 0;
    for (int i = 0; i < 8; i++) local_sum += local[i];

    muos_puts("sum(1..100)="); muos_putdec(sum);
    muos_puts(" stack_sum=");  muos_putdec(local_sum);
    muos_puts("\n");

    /* brk: user heap grows inside the sandbox */
    unsigned old = muos_brk(0);
    unsigned fresh = old + 4096;
    if (muos_brk(fresh) == fresh) {
        volatile unsigned* p = (volatile unsigned*)old;
        p[0] = 0x4D754F53;  /* 'MuOS' */
        p[1] = 0xDEADBEEF;
        muos_puts("brk="); muos_puthex(old);
        muos_puts(" heap[0]="); muos_puthex(p[0]);
        muos_puts(p[0] == 0x4D754F53 ? " heap-ok\n" : " heap-bad\n");
    } else {
        muos_puts("brk failed\n");
    }

    /* uname(122): Linux i386 utsname layout, 6 fields of 65 bytes */
    char uts[390];
    if (muos_uname(uts) == 0) {
        muos_puts("uname: sysname="); muos_puts(uts);
        muos_puts(" release="); muos_puts(uts + 65 * 2);
        muos_puts(" machine="); muos_puts(uts + 65 * 4);
        muos_puts("\n");
    } else {
        muos_puts("uname failed\n");
    }

    /* open/read/close on the RAM FS */
    char fbuf[80];
    int fd = muos_open("readme.txt", 0);
    if (fd >= 0) {
        int rn = muos_read(fd, fbuf, sizeof(fbuf) - 1);
        if (rn > 0) {
            fbuf[rn] = '\0';
            muos_puts("fs read bytes="); muos_putdec(rn);
            muos_puts("\n  "); muos_puts(fbuf); muos_puts("\n");
        }
        muos_close(fd);
    } else {
        muos_puts("open(readme.txt) failed\n");
    }

    /* create/truncate/write/read-back */
    int wfd = muos_open("ring3.txt", MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
    if (wfd >= 0) {
        const char msg[] = "written by Ring3\n";
        int wn = muos_write(wfd, msg, sizeof(msg) - 1);
        muos_close(wfd);
        muos_puts("fs write bytes="); muos_putdec(wn); muos_puts("\n");

        int rfd = muos_open("ring3.txt", 0);
        if (rfd >= 0) {
            char rb[32];
            int rr = muos_read(rfd, rb, sizeof(rb) - 1);
            if (rr > 0) {
                rb[rr] = '\0';
                muos_puts("fs readback: "); muos_puts(rb);
            }
            muos_close(rfd);
        }
    } else {
        muos_puts("create(ring3.txt) failed\n");
    }

    /* anonymous mmap(90): sandbox bump arena, zeroed page */
    unsigned mp = muos_mmap(4096);
    if (mp >= 0x08048000u && mp < 0x09000000u) {
        volatile unsigned* hp = (volatile unsigned*)mp;
        hp[0] = 0xCAFEBABE;
        hp[1023] = 0x12345678;
        muos_puts("mmap="); muos_puthex(mp);
        muos_puts(hp[0] == 0xCAFEBABE && hp[1023] == 0x12345678
                  ? " mmap-ok\n" : " mmap-bad\n");
        muos_munmap(mp, 4096);
    } else {
        muos_puts("mmap failed="); muos_puthex(mp); muos_puts("\n");
    }

    /* read(0): blocking keyboard read through int 0x80 */
    muos_puts("Type one character: ");
    char ch = 0;
    int n = muos_read(0, &ch, 1);
    muos_puts("\nread returned "); muos_putdec(n);
    if (n == 1) {
        muos_puts(", char="); muos_puthex((unsigned char)ch);
        muos_puts(" ('"); muos_write(1, &ch, 1); muos_puts("')\n");
    }

    muos_puts(USER_MARKER "\n");
    muos_puts("Goodbye from Ring3.\n");
    return 0;
}
