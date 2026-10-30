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

/* Internet checksum for the ICMP echo request built below. */
static unsigned short inet_cksum(const unsigned char* p, int len) {
    unsigned sum = 0;
    while (len > 1) {
        sum += ((unsigned)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (unsigned)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

/* Memory-order IPv4 address: bytes a.b.c.d on a little-endian host. */
#define IP4(a,b,c,d) ((unsigned)(a) | ((unsigned)(b) << 8) | \
                      ((unsigned)(c) << 16) | ((unsigned)(d) << 24))
/* Convert a host port to the network-byte-order value stored in structs. */
static unsigned short htons16(unsigned p) {
    return (unsigned short)(((p << 8) & 0xFF00) | ((p >> 8) & 0xFF));
}

/* ---- PIE test state: an absolute pointer needs R_386_RELATIVE ---- */
static const char* volatile pie_ptr = "PIE-RELOC-OK";
static volatile int sig_count = 0;
static volatile int sig_seen = 0;
static void usr1_handler(int sig) {
    sig_count++;
    sig_seen = sig;
}

/* ---- thread test state (CLONE_VM threads share this image) ---- */
static volatile int thread_marks[2];
static unsigned char thread_stacks[2][4096] __attribute__((aligned(16)));

static void thread_body(void* arg) {
    int id = (int)arg;
    thread_marks[id] = 100 + id;      /* visible to the parent: shared VM */
    for (volatile unsigned n = 0; n < 200000u; n++) { }
    muos_exit(30 + id);
}

static void spin_child(void) {
    for (;;) {
        volatile unsigned x = 1;
        x = x + 1;
        (void)x;
    }
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

    if (argc > 1 && arg_is(argv[1], "fsata")) {
        /* Persistent file through the VFS: create/write/fsync, read back,
         * then verify after a reboot with `run fsread`. */
        int ok = 1;
        const char msg[] = "PERSIST-FILE";
        int len = (int)sizeof(msg) - 1;
        int fd = muos_open("persist.txt",
                           MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        muos_puts("fsata: create="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;
        if (fd >= 0) {
            int n = muos_write(fd, msg, len);
            muos_puts("fsata: write="); muos_putdec(n); muos_puts("\n");
            ok = ok && n == len;
            int rc = muos_fsync(fd);
            muos_puts("fsata: fsync="); muos_putdec(rc); muos_puts("\n");
            ok = ok && rc == 0;
            muos_close(fd);
        }
        fd = muos_open("persist.txt", 0);
        char rb[16];
        int rn = -1;
        if (fd >= 0) {
            rn = muos_read(fd, rb, len);
            muos_close(fd);
        }
        int same = (rn == len) ? 1 : 0;
        for (int i = 0; same && i < len; i++) if (rb[i] != msg[i]) same = 0;
        muos_puts("fsata: readback="); muos_putdec(rn);
        muos_puts(same ? " data-ok\n" : " data-bad\n");
        ok = ok && same;
        muos_puts(ok ? "FS-PERSIST-OK\n" : "FS-PERSIST-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "fsread")) {
        /* Second boot: the file created by `run fsata` must still exist. */
        int ok = 1;
        const char want[] = "PERSIST-FILE";
        int len = (int)sizeof(want) - 1;
        char buf[16];
        int fd = muos_open("persist.txt", 0);
        muos_puts("fsread: open="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;
        if (fd >= 0) {
            int n = muos_read(fd, buf, len);
            muos_puts("fsread: read="); muos_putdec(n); muos_puts("\n");
            ok = ok && n == len;
            for (int i = 0; ok && i < len; i++) if (buf[i] != want[i]) ok = 0;
            muos_close(fd);
        }
        muos_puts(ok ? "FS-PERSIST-OK\n" : "FS-PERSIST-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "ata")) {
        /* Raw ATA sectors through /dev/hda: write a marker through the
         * block device, read it back, then verify after a reboot. */
        int ok = 1;
        int fd = muos_open("/dev/hda", MUOS_O_WRONLY);
        muos_puts("ata: open="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;
        if (fd >= 0) {
            const char msg[] = "PERSIST-DATA";
            int len = (int)sizeof(msg) - 1;
            muos_lseek(fd, 4096, 0);
            int n = muos_write(fd, msg, len);
            muos_puts("ata: write="); muos_putdec(n); muos_puts("\n");
            ok = ok && n == len;

            char rb[16];
            muos_lseek(fd, 4096, 0);
            int rn = muos_read(fd, rb, len);
            int same = (rn == len) ? 1 : 0;
            for (int i = 0; same && i < len; i++) if (rb[i] != msg[i]) same = 0;
            muos_puts("ata: readback="); muos_putdec(rn);
            muos_puts(same ? " data-ok\n" : " data-bad\n");
            ok = ok && same;
            muos_close(fd);
        }
        muos_puts(ok ? "ATA-OK\n" : "ATA-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "ataread")) {
        /* Second boot: the marker written by `run ata` must still be there. */
        int ok = 1;
        char buf[16];
        int fd = muos_open("/dev/hda", 0);
        muos_puts("ata: reopen="); muos_putdec(fd); muos_puts("\n");
        ok = ok && fd >= 0;
        if (fd >= 0) {
            const char want[] = "PERSIST-DATA";
            int len = (int)sizeof(want) - 1;
            muos_lseek(fd, 4096, 0);
            int n = muos_read(fd, buf, len);
            muos_puts("ata: read="); muos_putdec(n); muos_puts("\n");
            ok = ok && n == len;
            for (int i = 0; ok && i < len; i++)
                if (buf[i] != want[i]) ok = 0;
            muos_close(fd);
        }
        muos_puts(ok ? "ATA-PERSIST-OK\n" : "ATA-PERSIST-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "net")) {
        /* IPv4 socket ABI: raw ICMP echo to the QEMU gateway and a UDP DNS
         * query to slirp's resolver (10.0.2.3). Run with
         * `-nic user,model=rtl8139`. */
        int ok = 1;
        int rc;
        unsigned char pkt[64];
        unsigned char rbuf[256];
        muos_sockaddr_in_t sa;
        int alen = 16;

        int s = muos_socket(MUOS_AF_INET, MUOS_SOCK_RAW, MUOS_IPPROTO_ICMP);
        muos_puts("net: raw socket="); muos_putdec(s); muos_puts("\n");
        ok = ok && s >= 0;

        for (unsigned i = 0; i < sizeof(pkt); i++) pkt[i] = 0;
        pkt[0] = 8;                     /* echo request */
        pkt[4] = 0x4D; pkt[5] = 0x55;   /* id */
        pkt[6] = 0;    pkt[7] = 1;      /* seq */
        unsigned short ck = inet_cksum(pkt, 32);
        pkt[2] = (unsigned char)(ck >> 8);      /* checksum is big-endian */
        pkt[3] = (unsigned char)(ck & 0xFF);

        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = 0;
        sa.sin_addr = IP4(10,0,2,2);    /* QEMU user-net gateway */
        sa.pad[0] = sa.pad[1] = 0;
        rc = muos_sendto(s, pkt, 32, 0, &sa, 16);
        muos_puts("net: icmp send="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 32;

        int n = muos_recvfrom(s, rbuf, sizeof(rbuf), 0, &sa, &alen);
        int ping_ok = (n >= 8 && rbuf[0] == 0 &&
                       rbuf[4] == 0x4D && rbuf[5] == 0x55 &&
                       rbuf[6] == 0 && rbuf[7] == 1);
        muos_puts("net: icmp recv="); muos_putdec(n);
        muos_puts(ping_ok ? " PING-OK\n" : " PING-BAD\n");
        ok = ok && ping_ok;
        muos_close(s);

        /* DNS A query for example.com over UDP. */
        int u = muos_socket(MUOS_AF_INET, MUOS_SOCK_DGRAM, 0);
        muos_puts("net: udp socket="); muos_putdec(u); muos_puts("\n");
        ok = ok && u >= 0;
        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(12345);
        sa.sin_addr = 0;
        sa.pad[0] = sa.pad[1] = 0;
        rc = muos_bind(u, &sa, 16);
        muos_puts("net: bind="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        unsigned char q[48];
        for (unsigned i = 0; i < sizeof(q); i++) q[i] = 0;
        q[0] = 0x4D; q[1] = 0x55;       /* transaction id */
        q[2] = 0x01;                    /* recursion desired */
        q[5] = 1;                       /* QDCOUNT */
        int qn = 12;
        q[qn++] = 7; for (int i = 0; i < 7; i++) q[qn++] = "example"[i];
        q[qn++] = 3; for (int i = 0; i < 3; i++) q[qn++] = "com"[i];
        q[qn++] = 0;
        q[qn++] = 0; q[qn++] = 1;       /* QTYPE A */
        q[qn++] = 0; q[qn++] = 1;       /* QCLASS IN */

        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(53);
        sa.sin_addr = IP4(10,0,2,3);    /* slirp DNS */
        rc = muos_sendto(u, q, (unsigned)qn, 0, &sa, 16);
        muos_puts("net: dns send="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == qn;

        alen = 16;
        n = muos_recvfrom(u, rbuf, sizeof(rbuf), 0, &sa, &alen);
        int dns_ok = (n >= 12 && rbuf[0] == 0x4D && rbuf[1] == 0x55 &&
                      (rbuf[2] & 0x80) != 0);
        muos_puts("net: dns recv="); muos_putdec(n);
        muos_puts(dns_ok ? " UDP-OK\n" : " UDP-BAD\n");
        ok = ok && dns_ok;
        muos_close(u);

        muos_puts(ok ? "NET-OK\n" : "NET-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "tcp")) {
        /* SOCK_STREAM over the local loopback against the kernel's demo
         * echo service on 127.0.0.1:7: handshake, send, receive echo, FIN. */
        int ok = 1;
        muos_sockaddr_in_t sa;

        int c = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        muos_puts("tcp: client="); muos_putdec(c); muos_puts("\n");
        ok = ok && c >= 0;

        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(7);
        sa.sin_addr = IP4(127,0,0,1);
        sa.pad[0] = sa.pad[1] = 0;
        int rc = muos_connect(c, &sa, 16);
        muos_puts("tcp: connect="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;

        const char msg[] = "TCP-ECHO-REQ";
        int n = muos_send(c, msg, sizeof(msg) - 1, 0);
        muos_puts("tcp: send="); muos_putdec(n); muos_puts("\n");
        ok = ok && n == (int)sizeof(msg) - 1;

        char rb[64];
        int rn = muos_recv(c, rb, sizeof(rb) - 1, 0);
        int same = (rn == (int)sizeof(msg) - 1) ? 1 : 0;
        for (int i = 0; same && i < rn; i++) if (rb[i] != msg[i]) same = 0;
        muos_puts("tcp: recv="); muos_putdec(rn);
        muos_puts(same ? " echo-ok\n" : " echo-bad\n");
        ok = ok && same;
        muos_close(c);

        muos_puts(ok ? "TCP-OK\n" : "TCP-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "pie")) {
        /* Runs from the ET_DYN image: the kernel loaded the lowest
         * segment at USER_BASE and applied its RELATIVE relocations. */
        muos_puts((const char*)pie_ptr);    /* relocated pointer */
        muos_puts("\n");
        muos_puts(sig_count == 0 ? "PIE-OK\n" : "PIE-BAD\n");
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

    if (argc > 1 && arg_is(argv[1], "sig")) {
        /* Signal delivery: handler + sigreturn trampoline, blocked-mask
         * deferral, and the default terminate actions of SIGTERM/SIGKILL. */
        int ok = 1;
        int rc, status = 0;
        unsigned set = 1u << 10, oldmask = 0;

        rc = muos_signal(10, usr1_handler);
        muos_puts("sig: install-old="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == 0;                      /* old action was SIG_DFL */

        rc = muos_kill(muos_getpid(), 10);
        muos_puts("sig: kill-self="); muos_putdec(rc);
        muos_puts(" seen="); muos_putdec(sig_seen); muos_puts("\n");
        ok = ok && rc == 0 && sig_seen == 10 && sig_count == 1;

        /* Blocking a signal defers delivery until it is unblocked. */
        rc = muos_sigprocmask(0, &set, &oldmask);
        muos_puts("sig: block-mask="); muos_putdec(rc);
        muos_puts(" old="); muos_puthex(oldmask); muos_puts("\n");
        ok = ok && rc == 0 && oldmask == 0;

        rc = muos_kill(muos_getpid(), 10);
        muos_puts("sig: blocked-kill="); muos_putdec(rc);
        muos_puts(" count="); muos_putdec(sig_count); muos_puts("\n");
        ok = ok && rc == 0 && sig_count == 1;

        set = 0;
        rc = muos_sigprocmask(2, &set, 0);       /* SIG_SETMASK */
        muos_puts("sig: unblock="); muos_putdec(rc);
        muos_puts(" count="); muos_putdec(sig_count); muos_puts("\n");
        ok = ok && rc == 0 && sig_count == 2;

        /* SIG_IGN swallows the signal; SIGKILL cannot be caught. */
        rc = muos_signal(10, (void*)1);
        ok = ok && rc > 0;
        rc = muos_kill(muos_getpid(), 10);
        ok = ok && rc == 0 && sig_count == 2;
        rc = muos_signal(9, usr1_handler);
        muos_puts("sig: catch-kill="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -22;

        /* Default action terminates: wait status = 128 + signal. */
        int pid = muos_fork();
        if (pid == 0) spin_child();
        rc = muos_kill(pid, 15);
        int w = muos_waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xFF;
        muos_puts("sig: term wait="); muos_putdec(w);
        muos_puts(" code="); muos_putdec(code); muos_puts("\n");
        ok = ok && rc == 0 && w == pid && code == 143;

        pid = muos_fork();
        if (pid == 0) spin_child();
        rc = muos_kill(pid, 9);
        w = muos_waitpid(pid, &status, 0);
        code = (status >> 8) & 0xFF;
        muos_puts("sig: kill wait="); muos_putdec(w);
        muos_puts(" code="); muos_putdec(code); muos_puts("\n");
        ok = ok && rc == 0 && w == pid && code == 137;

        rc = muos_kill(9999, 0);
        muos_puts("sig: kill-badpid="); muos_putdec(rc); muos_puts("\n");
        ok = ok && rc == -3;

        muos_puts(ok ? "SIG-OK\n" : "SIG-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "thread")) {
        /* Two CLONE_VM threads on their own stacks mutate this process's
         * globals; pid/tid are distinct and both join via waitpid. */
        int ok = 1;
        thread_marks[0] = thread_marks[1] = 0;

        int p0 = muos_thread_start((void*)thread_body, (void*)0,
                                   &thread_stacks[0][0] + 4096);
        int p1 = muos_thread_start((void*)thread_body, (void*)1,
                                   &thread_stacks[1][0] + 4096);
        muos_puts("thread: pids="); muos_putdec(p0);
        muos_puts("/"); muos_putdec(p1);
        muos_puts(" tid="); muos_putdec(muos_gettid()); muos_puts("\n");
        ok = ok && p0 > 0 && p1 > 0 && p0 != p1;

        int st0 = 0, st1 = 0;
        int w0 = muos_waitpid(p0, &st0, 0);
        int w1 = muos_waitpid(p1, &st1, 0);
        int c0 = (st0 >> 8) & 0xFF, c1 = (st1 >> 8) & 0xFF;
        muos_puts("thread: wait="); muos_putdec(w0);
        muos_puts("/"); muos_putdec(w1);
        muos_puts(" code="); muos_putdec(c0);
        muos_puts("/"); muos_putdec(c1);
        muos_puts(" marks="); muos_putdec(thread_marks[0]);
        muos_puts("/"); muos_putdec(thread_marks[1]); muos_puts("\n");
        ok = ok && w0 == p0 && w1 == p1 && c0 == 30 && c1 == 31;
        ok = ok && thread_marks[0] == 100 && thread_marks[1] == 101;

        muos_puts(ok ? "THREAD-OK\n" : "THREAD-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "blkpoll")) {
        /* poll really blocks: a child's pipe write wakes the parent, and a
         * PIT deadline wakes it while another process keeps running. */
        int ok = 1;
        int rc, status = 0;
        int fds[2] = { -1, -1 };
        int f2[2] = { -1, -1 };
        muos_pollfd_t pf;
        char buf[16];

        rc = muos_pipe(fds);
        ok = ok && rc == 0;
        int pid = muos_fork();
        if (pid == 0) {
            muos_close(fds[0]);
            muos_write(fds[1], "WAKE", 4);
            muos_close(fds[1]);
            muos_exit(0);
        }
        muos_close(fds[1]);

        pf.fd = fds[0]; pf.events = MUOS_POLLIN; pf.revents = 0;
        rc = muos_poll(&pf, 1, -1);
        muos_puts("blkpoll: wake="); muos_putdec(rc);
        muos_puts(" revents="); muos_puthex((unsigned)pf.revents); muos_puts("\n");
        ok = ok && rc == 1 && (pf.revents & MUOS_POLLIN);

        int n = muos_read(fds[0], buf, sizeof(buf));
        int same = (n == 4 && buf[0] == 'W' && buf[1] == 'A' &&
                    buf[2] == 'K' && buf[3] == 'E') ? 1 : 0;
        muos_puts("blkpoll: read="); muos_putdec(n);
        muos_puts(same ? " data-ok\n" : " data-bad\n");
        ok = ok && same;

        int w = muos_waitpid(pid, &status, 0);
        ok = ok && w == pid;
        muos_close(fds[0]);

        /* Timeout wakeup while a spinning child keeps the CPU busy. */
        rc = muos_pipe(f2);
        int p2 = muos_fork();
        if (p2 == 0) spin_child();
        pf.fd = f2[0]; pf.events = MUOS_POLLIN; pf.revents = 0;
        unsigned t0 = muos_times(0);
        rc = muos_poll(&pf, 1, 50);
        unsigned t1 = muos_times(0);
        muos_puts("blkpoll: timeout="); muos_putdec(rc);
        muos_puts(" ticks="); muos_putdec((int)(t1 - t0)); muos_puts("\n");
        ok = ok && rc == 0 && (t1 - t0) >= 2 && (t1 - t0) <= 12;

        muos_kill(p2, 9);
        muos_waitpid(p2, &status, 0);
        muos_close(f2[0]); muos_close(f2[1]);

        muos_puts(ok ? "BLKPOLL-OK\n" : "BLKPOLL-BAD\n");
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
