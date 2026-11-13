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

/* Count non-overlapping occurrences in a NUL-terminated string. */
static int count_sub(const char* s, const char* sub) {
    int n = 0;
    while (*s) {
        const char* a = s;
        const char* b = sub;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) n++;
        s++;
    }
    return n;
}

static char maps_buf[512];
static char statx_buf[MUOS_STATX_SIZE];
static int maps_count(const char* sub) {
    int fd = muos_open("/proc/self/maps", 0);
    if (fd < 0) return -1;
    int n = muos_read(fd, maps_buf, sizeof(maps_buf) - 1);
    muos_close(fd);
    maps_buf[n > 0 ? n : 0] = '\0';
    return count_sub(maps_buf, sub);
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

/* ---- futex test state (shared by CLONE_VM threads) ---- */
static volatile unsigned futex_word;
static volatile int futex_wake_ret = -1;

/* AF_UNIX 8KB ring check uses two 2KB buffers outside the 8KB stack. */
static unsigned char unix_big[2048];
static unsigned char unix_big2[2048];
static unsigned char unix_fill[8192];
static unsigned char unix_drainbuf[4096];
static unsigned char unix_small[512];
static unsigned char vfs_big[524288];       /* 512KB mounted-volume write test */

static void unix_drainer(void* arg) {
    unsigned ts[2] = { 0, 20000000 };       /* 20ms */
    muos_nanosleep(ts, 0);
    muos_recv((int)arg, unix_drainbuf, sizeof(unix_drainbuf), 0);
    muos_exit(0);
}

static void futex_waker(void* arg) {
    (void)arg;
    unsigned ts[2] = { 0, 20000000 };       /* 20ms */
    muos_nanosleep(ts, 0);
    futex_word = 1;
    futex_wake_ret = muos_futex((void*)&futex_word, MUOS_FUTEX_WAKE, 1, 0);
    muos_exit(0);
}

static void futex_sleeper(void* arg) {
    (void)arg;
    unsigned ts[2] = { 0, 100000000 };      /* 100ms */
    muos_nanosleep(ts, 0);
    muos_exit(0);
}

/* FUTEX_REQUEUE moves this waiter from req_a to req_b. */
static unsigned futex_req_a, futex_req_b;
static volatile int futex_req_started, futex_req_ret, futex_req_seen;
static void futex_requeue_waiter(void* arg) {
    (void)arg;
    futex_req_started = 1;
    futex_req_seen = (int)futex_req_a;
    futex_req_ret = muos_futex((void*)&futex_req_a, MUOS_FUTEX_WAIT, 0, 0);
}

/* Robust list: node -> lock word; exiting while holding it must zero it. */
static unsigned robust_head[3], robust_node[2], robust_word;
static void robust_thread(void* arg) {
    (void)arg;
    robust_node[0] = (unsigned)&robust_head[0];
    robust_node[1] = (unsigned)((char*)&robust_word - (char*)&robust_node[0]);
    robust_head[0] = (unsigned)&robust_node[0];
    robust_head[1] = 0;
    robust_head[2] = 0;
    muos_set_robust_list(&robust_head[0], 12);
    robust_word = 1;
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

    if (argc > 1 && arg_is(argv[1], "vfs")) {
        /* Mounted FAT32/ext2 volumes through open/read/stat. The test image
         * has one volume, so try /fat first and fall back to /ext2. */
        union {
            char raw[MUOS_STAT64_SIZE];
            unsigned word[MUOS_STAT64_SIZE / 4];
        } st;
        char buf[64];
        const char* path = "/fat/HELLO.TXT";
        int ok = 1;

        int fd = muos_open(path, 0);
        if (fd < 0) {
            path = "/ext2/hello.txt";
            fd = muos_open(path, 0);
        }
        muos_puts("vfs: open="); muos_putdec(fd);
        muos_puts(" path="); muos_puts(path); muos_puts("\n");
        if (fd < 0) {
            muos_puts("VFS-BAD\n");
            return 0;
        }

        int n = muos_read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = '\0';
        muos_puts("vfs: read="); muos_putdec(n); muos_puts("\n");
        if (arg_is(buf, "FAT-HELLO-OK\n"))       muos_puts("VFS-FAT-OK\n");
        else if (arg_is(buf, "EXT2-READ-OK\n"))  muos_puts("VFS-EXT2-OK\n");
        else ok = 0;
        muos_close(fd);

        /* /ext2/big.bin is 300KB and its tail is behind a double-indirect
         * block, so this read exercises the multi-level indirect path. */
        if (path[1] == 'e') {
            int bfd = muos_open("/ext2/big.bin", 0);
            int big_ok = 0;
            if (bfd >= 0) {
                muos_lseek(bfd, 307200 - 12, 0);
                char tb[16];
                int tn = muos_read(bfd, tb, 12);
                tb[tn > 0 ? tn : 0] = '\0';
                big_ok = (tn == 12 && arg_is(tb, "EXT2-BIG-OK\n"));
                muos_close(bfd);
            }
            muos_puts("vfs: big="); muos_putdec(big_ok); muos_puts("\n");
            if (big_ok) muos_puts("EXT2-BIG-OK\n");
            ok = ok && big_ok;

            /* >64KB VFS write: 128KB through the mounted ext2 volume, then
             * read the tail marker back to prove the whole file landed. */
            for (unsigned i = 0; i < sizeof(vfs_big); i++)
                vfs_big[i] = (unsigned char)('a' + (i % 26));
            const char bwmsg[] = "VFS-BIG-OK\n";
            for (unsigned i = 0; i < sizeof(bwmsg) - 1; i++)
                vfs_big[sizeof(vfs_big) - sizeof(bwmsg) + 1 + i] = bwmsg[i];
            int bwfd = muos_open("/ext2/bigwrite.bin",
                                 MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
            int bw = bwfd >= 0 ? muos_write(bwfd, vfs_big, sizeof(vfs_big)) : -1;
            if (bwfd >= 0) muos_close(bwfd);

            int bw_ok = 0;
            if (bw == (int)sizeof(vfs_big)) {
                int brfd = muos_open("/ext2/bigwrite.bin", 0);
                if (brfd >= 0) {
                    muos_lseek(brfd, sizeof(vfs_big) - sizeof(bwmsg) + 1, 0);
                    char bt[16];
                    int bn = muos_read(brfd, bt, sizeof(bwmsg) - 1);
                    bt[bn > 0 ? bn : 0] = '\0';
                    bw_ok = (bn == (int)sizeof(bwmsg) - 1 && arg_is(bt, bwmsg));
                    muos_close(brfd);
                }
            }
            muos_puts("vfs: big-write="); muos_putdec(bw);
            muos_puts(bw_ok ? " big-write-ok\n" : " big-write-bad\n");
            if (bw_ok) muos_puts("VFS-BIGWRITE-OK\n");
            ok = ok && bw_ok;
        }

        int rc = muos_fstatat64(MUOS_AT_FDCWD, path, st.raw, 0);
        muos_puts("vfs: stat="); muos_putdec(rc);
        muos_puts(" size="); muos_putdec((int)st.word[11]); muos_puts("\n");
        if (rc != 0 || st.word[11] == 0) ok = 0;

        /* Create/overwrite through the mount, then read it back. */
        const char* wpath = (path[1] == 'f') ? "/fat/VFS.TXT" : "/ext2/vfs.txt";
        const char wmsg[] = "VFS-WRITE-OK\n";
        int wfd = muos_open(wpath, MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        muos_puts("vfs: write-open="); muos_putdec(wfd); muos_puts("\n");
        if (wfd < 0) {
            ok = 0;
        } else {
            int wn = muos_write(wfd, wmsg, sizeof(wmsg) - 1);
            muos_close(wfd);
            char rb[32];
            int rfd = muos_open(wpath, 0);
            int rn = rfd >= 0 ? muos_read(rfd, rb, sizeof(rb) - 1) : -1;
            if (rfd >= 0) muos_close(rfd);
            rb[rn > 0 ? rn : 0] = '\0';
            muos_puts("vfs: write="); muos_putdec(wn);
            muos_puts(" read="); muos_putdec(rn); muos_puts("\n");
            if (wn == (int)sizeof(wmsg) - 1 && arg_is(rb, "VFS-WRITE-OK\n"))
                muos_puts("VFS-WRITE-OK\n");
            else
                ok = 0;
        }

        /* Directory creation, rename and delete through the mount. */
        int is_fat = (path[1] == 'f');
        const char* mdir = is_fat ? "/fat/VFSDIR" : "/ext2/vfsdir";
        const char* mfile = is_fat ? "/fat/VFSDIR/A.TXT" : "/ext2/vfsdir/a.txt";
        const char* rfile = is_fat ? "/fat/VFSDIR/B.TXT" : "/ext2/vfsdir/b.txt";
        int mk = muos_mkdirat(MUOS_AT_FDCWD, mdir, 0755);
        muos_puts("vfs: mkdir="); muos_putdec(mk); muos_puts("\n");
        if (mk == 0 || mk == -17) muos_puts("VFS-MKDIR-OK\n");
        else ok = 0;

        const char rmsg[] = "VFS-RENAME-OK\n";
        int mfd = muos_open(mfile, MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        if (mfd >= 0) {
            muos_write(mfd, rmsg, sizeof(rmsg) - 1);
            muos_close(mfd);
        }
        int rr = muos_renameat(MUOS_AT_FDCWD, mfile, MUOS_AT_FDCWD, rfile);
        muos_puts("vfs: rename="); muos_putdec(rr); muos_puts("\n");
        int renamed = 0;
        if (rr == 0) {
            int rfd = muos_open(rfile, 0);
            char rb2[24];
            int rn2 = rfd >= 0 ? muos_read(rfd, rb2, sizeof(rb2) - 1) : -1;
            if (rfd >= 0) muos_close(rfd);
            rb2[rn2 > 0 ? rn2 : 0] = '\0';
            if (rn2 == (int)sizeof(rmsg) - 1 && arg_is(rb2, "VFS-RENAME-OK\n"))
                renamed = 1;
        }
        if (renamed) muos_puts("VFS-RENAME-OK\n");
        else ok = 0;

        int urc = muos_unlinkat(MUOS_AT_FDCWD, rfile, 0);
        int still = muos_open(rfile, 0);
        if (still >= 0) muos_close(still);
        muos_puts("vfs: unlink="); muos_putdec(urc); muos_puts("\n");
        if (urc == 0 && still < 0) muos_puts("VFS-UNLINK-OK\n");
        else ok = 0;

        /* Directory create, non-empty rejection, rename and rmdir. */
        const char* d1 = is_fat ? "/fat/VFSD2" : "/ext2/vfsd2";
        const char* d2 = is_fat ? "/fat/VFSD3" : "/ext2/vfsd3";
        const char* df = is_fat ? "/fat/VFSD2/K.TXT" : "/ext2/vfsd2/k.txt";
        int dk = muos_mkdirat(MUOS_AT_FDCWD, d1, 0755);
        if (dk != 0 && dk != -17) ok = 0;
        int kfd = muos_open(df, MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        if (kfd >= 0) {
            muos_write(kfd, "K", 1);
            muos_close(kfd);
        } else ok = 0;
        int nr = muos_unlinkat(MUOS_AT_FDCWD, d1, MUOS_AT_REMOVEDIR);
        muos_puts("vfs: rmdir-nonempty="); muos_putdec(nr); muos_puts("\n");
        if (nr == -39) muos_puts("VFS-RMDIR-NONEMPTY-OK\n");
        else ok = 0;

        muos_unlinkat(MUOS_AT_FDCWD, df, 0);
        int drn = muos_renameat(MUOS_AT_FDCWD, d1, MUOS_AT_FDCWD, d2);
        muos_puts("vfs: dir-rename="); muos_putdec(drn); muos_puts("\n");
        if (drn == 0) muos_puts("VFS-DIRRENAME-OK\n");
        else ok = 0;

        int drm = muos_unlinkat(MUOS_AT_FDCWD, d2, MUOS_AT_REMOVEDIR);
        muos_puts("vfs: rmdir="); muos_putdec(drm); muos_puts("\n");
        if (drm == 0) muos_puts("VFS-RMDIR-OK\n");
        else ok = 0;

        /* The mount root must enumerate through getdents64 too. */
        int dfd = muos_open("/fat", MUOS_O_DIRECTORY);
        if (dfd < 0) dfd = muos_open("/ext2", MUOS_O_DIRECTORY);
        if (dfd >= 0) {
            char db[256];
            int dn = muos_getdents64(dfd, db, sizeof(db));
            muos_close(dfd);
            muos_puts("vfs: getdents="); muos_putdec(dn); muos_puts("\n");
            int found = 0;
            for (int off = 0; off < dn; ) {
                unsigned short rl = *(unsigned short*)(db + off + 16);
                if (!rl || off + (int)rl > dn) break;
                const char* nm = db + off + 19;
                if (arg_is(nm, "HELLO.TXT") || arg_is(nm, "hello.txt") ||
                    arg_is(nm, "SUBDIR")    || arg_is(nm, "subdir")) found = 1;
                off += (int)rl;
            }
            if (found) muos_puts("VFS-LIST-OK\n");
            else ok = 0;
        } else {
            ok = 0;
        }

        muos_puts(ok ? "VFS-OK\n" : "VFS-BAD\n");
        return 0;
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
        /* 1) Two concurrent connections to the kernel echo service on
         *    127.0.0.1:7 (multi-socket + congestion window).
         * 2) getsockname/getpeername must report the real 4-tuple.
         * 3) User-space listen/accept with two forked clients. */
        int ok = 1;
        muos_sockaddr_in_t sa;
        muos_sockaddr_in_t ga;
        int glen;
        char rb[64];

        int c = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        int c2 = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        muos_puts("tcp: clients="); muos_putdec(c); muos_puts(","); muos_putdec(c2);
        muos_puts("\n");
        ok = ok && c >= 0 && c2 >= 0;

        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(7);
        sa.sin_addr = IP4(127,0,0,1);
        sa.pad[0] = sa.pad[1] = 0;
        int rc = muos_connect(c, &sa, 16);
        int rc2 = muos_connect(c2, &sa, 16);
        muos_puts("tcp: connect="); muos_putdec(rc); muos_puts(",");
        muos_putdec(rc2); muos_puts("\n");
        ok = ok && rc == 0 && rc2 == 0;

        /* getpeername on c: 127.0.0.1:7; getsockname: a bound local port. */
        glen = 16;
        rc = muos_getpeername(c, &ga, &glen);
        int peer_ok = rc == 0 && ga.sin_addr == IP4(127,0,0,1) &&
                      ga.sin_port == htons16(7);
        glen = 16;
        rc2 = muos_getsockname(c, &ga, &glen);
        int local_ok = rc2 == 0 && ga.sin_addr == IP4(127,0,0,1) &&
                       ga.sin_port != 0;
        muos_puts("tcp: names="); muos_putdec(peer_ok); muos_puts(",");
        muos_putdec(local_ok); muos_puts("\n");
        ok = ok && peer_ok && local_ok;

        const char msg[] = "TCP-ECHO-REQ";
        const char msg2[] = "TCP-ECHO-2";
        int n1 = muos_send(c, msg, sizeof(msg) - 1, 0);
        int n2 = muos_send(c2, msg2, sizeof(msg2) - 1, 0);
        muos_puts("tcp: send="); muos_putdec(n1); muos_puts(",");
        muos_putdec(n2); muos_puts("\n");
        ok = ok && n1 == (int)sizeof(msg) - 1 && n2 == (int)sizeof(msg2) - 1;

        int rn1 = muos_recv(c, rb, sizeof(rb) - 1, 0);
        int same1 = (rn1 == (int)sizeof(msg) - 1) ? 1 : 0;
        for (int i = 0; same1 && i < rn1; i++) if (rb[i] != msg[i]) same1 = 0;
        int rn2 = muos_recv(c2, rb, sizeof(rb) - 1, 0);
        int same2 = (rn2 == (int)sizeof(msg2) - 1) ? 1 : 0;
        for (int i = 0; same2 && i < rn2; i++) if (rb[i] != msg2[i]) same2 = 0;
        muos_puts("tcp: recv="); muos_putdec(rn1); muos_puts(",");
        muos_putdec(rn2); muos_puts(same1 && same2 ? " echo-ok\n" : " echo-bad\n");
        ok = ok && same1 && same2;
        muos_close(c);
        muos_close(c2);

        /* ---- user-space listen/accept with two concurrent clients ---- */
        int lfd = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(9000);
        sa.sin_addr = 0;                    /* any local address */
        sa.pad[0] = sa.pad[1] = 0;
        int brc = muos_bind(lfd, &sa, 16);
        int lrc = muos_listen(lfd, 2);
        muos_puts("tcp: listen="); muos_putdec(brc); muos_puts(",");
        muos_putdec(lrc); muos_puts("\n");
        ok = ok && brc == 0 && lrc == 0;

        /* Self-connect: the kernel completes the handshake synchronously
         * over loopback, then accept() must return the server-side socket.
         * (Kernel-side wakeups cannot preempt a Ring0 syscall poll loop, so
         * a two-process client/server test is not possible yet.) */
        int cli = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(9000);
        sa.sin_addr = IP4(127,0,0,1);
        sa.pad[0] = sa.pad[1] = 0;
        int crc = muos_connect(cli, &sa, 16);
        int a = muos_accept(lfd);
        muos_puts("tcp: self-connect="); muos_putdec(crc);
        muos_puts(" accept="); muos_putdec(a); muos_puts("\n");
        ok = ok && crc == 0 && a >= 0;

        if (a >= 0) {
            const char rq[] = "ACCEPT-REQ";
            muos_send(cli, rq, sizeof(rq) - 1, 0);
            char ab[32];
            int an = muos_recv(a, ab, sizeof(ab) - 1, 0);
            int got = (an == (int)sizeof(rq) - 1);
            for (int i = 0; got && i < an; i++) if (ab[i] != rq[i]) got = 0;
            muos_puts("tcp: accept-recv="); muos_putdec(an);
            muos_puts(got ? " data-ok\n" : " data-bad\n");
            ok = ok && got;

            muos_send(a, "OK", 2, 0);
            int rn = muos_recv(cli, ab, sizeof(ab) - 1, 0);
            muos_puts("tcp: client-recv="); muos_putdec(rn);
            muos_puts(rn == 2 && ab[0] == 'O' && ab[1] == 'K' ? " reply-ok\n"
                                                              : " reply-bad\n");
            ok = ok && rn == 2 && ab[0] == 'O' && ab[1] == 'K';
            muos_close(a);
        }
        muos_close(cli);
        muos_close(lfd);

        /* Nonblocking connect: SYN goes out, connect returns EINPROGRESS, and
         * poll(POLLOUT) turns ready once the handshake completes. */
        {
            int nc = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
            muos_sockaddr_in_t nsa;
            nsa.sin_family = MUOS_AF_INET;
            nsa.sin_port = htons16(7);
            nsa.sin_addr = IP4(127,0,0,1);
            nsa.pad[0] = nsa.pad[1] = 0;
            muos_fcntl(nc, MUOS_F_SETFL, MUOS_O_NONBLOCK);
            int crc = nc >= 0 ? muos_connect(nc, &nsa, 16) : -1;

            muos_pollfd_t cp;
            cp.fd = nc; cp.events = MUOS_POLLOUT; cp.revents = 0;
            int praw = muos_poll(&cp, 1, 500);
            int wrev = cp.revents;

            char nb[] = "NB"; char rbuf[4];
            int sn = muos_send(nc, nb, 2, 0);
            cp.events = MUOS_POLLIN; cp.revents = 0;
            int prin = muos_poll(&cp, 1, 500);
            int rn = prin == 1 ? muos_recv(nc, rbuf, 4, 0) : -1;
            int nbok = (crc == -115 && praw == 1 && (wrev & MUOS_POLLOUT) &&
                        sn == 2 && rn == 2 && rbuf[0] == 'N' && rbuf[1] == 'B');
            muos_puts("tcp: nb-connect="); muos_putdec(crc);
            muos_puts(" ready="); muos_putdec(praw);
            muos_puts("/"); muos_putdec(wrev);
            muos_puts(" reply="); muos_putdec(rn);
            muos_puts(nbok ? " nb-ok\n" : " nb-bad\n");
            ok = ok && nbok;
            if (nc >= 0) muos_close(nc);
        }

        /* AF_UNIX socketpair + getsockopt(SO_TYPE) + sendmsg/recvmsg. */
        {
            int sv[2];
            if (muos_socketpair(MUOS_SOCK_STREAM, sv) == 0) {
                unsigned so_type = 0, so_len = 4;
                int grc = muos_getsockopt(sv[0], MUOS_SOL_SOCKET,
                                          MUOS_SO_TYPE, &so_type, &so_len);
                muos_puts("tcp: socketpair="); muos_putdec(sv[0]);
                muos_puts(","); muos_putdec(sv[1]);
                muos_puts(" so-type="); muos_putdec((int)so_type); muos_puts("\n");
                ok = ok && grc == 0 && so_type == MUOS_SOCK_STREAM;

                const char p1[] = "PAIR-";
                const char p2[] = "OK";
                char r1[3], r2[16];
                muos_iovec_t iv[2], riv[2];
                iv[0].base = (unsigned)p1; iv[0].len = 5;
                iv[1].base = (unsigned)p2; iv[1].len = 2;
                riv[0].base = (unsigned)r1; riv[0].len = 3;
                riv[1].base = (unsigned)r2; riv[1].len = sizeof(r2);

                muos_msghdr_t mh;
                mh.name = 0; mh.namelen = 0;
                mh.iov = (unsigned)iv; mh.iovlen = 2;
                mh.control = 0; mh.controllen = 0; mh.flags = 0;
                int sent = muos_sendmsg(sv[0], &mh, 0);
                mh.iov = (unsigned)riv; mh.iovlen = 2;
                int got = muos_recvmsg(sv[1], &mh, 0);

                int pair_ok = (sent == 7 && got == 7 &&
                               r1[0] == 'P' && r1[1] == 'A' && r1[2] == 'I' &&
                               r2[0] == 'R' && r2[1] == '-' &&
                               r2[2] == 'O' && r2[3] == 'K');
                muos_puts("tcp: pair-send="); muos_putdec(sent);
                muos_puts(" pair-recv="); muos_putdec(got);
                muos_puts(pair_ok ? " pair-ok\n" : " pair-bad\n");
                ok = ok && pair_ok;

                /* The AF_UNIX ring holds 8KB, so a 2KB transfer must fit. */
                for (unsigned i = 0; i < sizeof(unix_big); i++)
                    unix_big[i] = (unsigned char)('a' + (i % 26));
                int bs = muos_send(sv[0], unix_big, sizeof(unix_big), 0);
                int br = muos_recv(sv[1], unix_big2, sizeof(unix_big2), 0);
                int big_ok = (bs == (int)sizeof(unix_big) &&
                              br == (int)sizeof(unix_big2));
                for (unsigned i = 0; big_ok && i < sizeof(unix_big); i++)
                    if (unix_big2[i] != unix_big[i]) big_ok = 0;
                muos_puts("tcp: unix-big="); muos_putdec(bs);
                muos_puts("/"); muos_putdec(br);
                muos_puts(big_ok ? " big-ok\n" : " big-bad\n");
                ok = ok && big_ok;

                /* Socket fcntl(F_SETFL, O_NONBLOCK): an empty read then
                 * returns EAGAIN immediately instead of polling. */
                muos_fcntl(sv[1], MUOS_F_SETFL, MUOS_O_NONBLOCK);
                int nfl = muos_fcntl(sv[1], MUOS_F_GETFL, 0);
                int nbr = muos_recv(sv[1], r1, sizeof(r1), 0);
                int nb_ok = (nfl == (MUOS_O_NONBLOCK | 2) && nbr == -11);
                muos_puts("tcp: sock-nonblock="); muos_putdec(nfl);
                muos_puts("/"); muos_putdec(nbr);
                muos_puts(nb_ok ? " nonblock-ok\n" : " nonblock-bad\n");
                ok = ok && nb_ok;
                muos_fcntl(sv[1], MUOS_F_SETFL, 0);   /* back to blocking */

                /* poll(2) on a socket fd: empty = 0, data = POLLIN. */
                muos_pollfd_t pfd;
                pfd.fd = sv[1]; pfd.events = MUOS_POLLIN; pfd.revents = 0;
                int pr0 = muos_poll(&pfd, 1, 0);
                int ps = muos_send(sv[0], "POLLTST", 7, 0);
                pfd.revents = 0;
                int pr1 = muos_poll(&pfd, 1, 0);
                int prd = muos_recv(sv[1], unix_big2, 7, 0);
                int poll_ok = (pr0 == 0 && ps == 7 && pr1 == 1 &&
                               (pfd.revents & MUOS_POLLIN) && prd == 7);
                muos_puts("tcp: sock-poll="); muos_putdec(pr0);
                muos_puts("/"); muos_putdec(pr1);
                muos_puts("/"); muos_putdec(pfd.revents);
                muos_puts(poll_ok ? " poll-ok\n" : " poll-bad\n");
                ok = ok && poll_ok;

                /* Fill the 8KB ring, then a 512-byte send blocks until a
                 * thread drains 4KB; the kernel completes the pending send. */
                for (unsigned i = 0; i < sizeof(unix_fill); i++)
                    unix_fill[i] = (unsigned char)(i & 0xFF);
                for (unsigned i = 0; i < sizeof(unix_small); i++)
                    unix_small[i] = (unsigned char)('A' + (i % 26));
                int fill_rc = muos_send(sv[0], unix_fill,
                                        sizeof(unix_fill), 0);
                int th = muos_thread_start(unix_drainer, (void*)sv[1],
                                           &thread_stacks[0][0] + 4096);
                int small_rc = muos_send(sv[0], unix_small,
                                         sizeof(unix_small), 0);
                int wst = 0;
                int wth = muos_waitpid(th, &wst, 0);
                int block_ok = (fill_rc == (int)sizeof(unix_fill) &&
                                small_rc == (int)sizeof(unix_small) &&
                                wth == th);
                muos_puts("tcp: unix-fill="); muos_putdec(fill_rc);
                muos_puts(" blocked-send="); muos_putdec(small_rc);
                muos_puts(block_ok ? " block-ok\n" : " block-bad\n");
                ok = ok && block_ok;

                /* Drain what is left so the shutdown EOF test starts empty. */
                int dr;
                do {
                    dr = muos_recv(sv[1], unix_drainbuf,
                                   sizeof(unix_drainbuf), 0);
                } while (dr > 0);

                /* shutdown(SHUT_WR) on one end: the partner drains what is
                 * buffered (already read above), then recv reports EOF. */
                int sdc = muos_shutdown(sv[0], MUOS_SHUT_WR);
                char eob[4];
                int eof = muos_recv(sv[1], eob, sizeof(eob), 0);
                int shut_ok = (sdc == 0 && eof == 0);
                muos_puts("tcp: shutdown="); muos_putdec(sdc);
                muos_puts(" peer-recv="); muos_putdec(eof);
                muos_puts(shut_ok ? " shutdown-ok\n" : " shutdown-bad\n");
                ok = ok && shut_ok;

                /* EOF also shows up as POLLIN|POLLHUP on the socket. */
                pfd.fd = sv[1]; pfd.events = MUOS_POLLIN; pfd.revents = 0;
                int peof = muos_poll(&pfd, 1, 0);
                int eof_poll = (peof == 1 && (pfd.revents & MUOS_POLLIN) &&
                                (pfd.revents & MUOS_POLLHUP));
                muos_puts("tcp: eof-poll="); muos_putdec(peof);
                muos_puts("/"); muos_putdec(pfd.revents);
                muos_puts(eof_poll ? " eof-poll-ok\n" : " eof-poll-bad\n");
                ok = ok && eof_poll;

                muos_close(sv[0]);
                muos_close(sv[1]);
            } else {
                muos_puts("tcp: socketpair-bad\n");
                ok = 0;
            }
        }

        muos_puts(ok ? "ACCEPT-OK\n" : "ACCEPT-BAD\n");
        if (!ok) { muos_puts("TCP-BAD\n"); return 0; }

        muos_puts("TCP-OK\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "baccept")) {
        /* Blocking accept: fork a delayed client, accept() must yield the
         * CPU until the child's SYN/ACK completes the handshake. */
        int ok = 1;
        muos_sockaddr_in_t sa;
        int lfd = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
        sa.sin_family = MUOS_AF_INET;
        sa.sin_port = htons16(9002);
        sa.sin_addr = 0;
        sa.pad[0] = sa.pad[1] = 0;
        int brc = muos_bind(lfd, &sa, 16);
        int lrc = muos_listen(lfd, 2);
        ok = ok && brc == 0 && lrc == 0;

        int pid = muos_fork();
        if (pid == 0) {
            unsigned ts[2] = { 0, 80000000 };   /* 80ms */
            muos_nanosleep(ts, 0);
            int cli = muos_socket(MUOS_AF_INET, MUOS_SOCK_STREAM, 0);
            sa.sin_family = MUOS_AF_INET;
            sa.sin_port = htons16(9002);
            sa.sin_addr = IP4(127,0,0,1);
            sa.pad[0] = sa.pad[1] = 0;
            int crc = cli >= 0 ? muos_connect(cli, &sa, 16) : -1;
            const char msg[] = "BACC";
            if (crc == 0) muos_send(cli, msg, sizeof(msg) - 1, 0);
            muos_exit(crc == 0 ? 0 : 1);
        }

        int a = muos_accept(lfd);
        muos_puts("baccept: accept="); muos_putdec(a); muos_puts("\n");
        ok = ok && a >= 0;
        if (a >= 0) {
            char ab[8];
            int n = muos_recv(a, ab, sizeof(ab) - 1, 0);
            int got = (n == 4);
            for (int i = 0; got && i < n; i++) if (ab[i] != "BACC"[i]) got = 0;
            if (got) muos_puts("baccept: data-ok\n");
            else ok = 0;
            muos_close(a);
        }
        int status = 0;
        muos_waitpid(pid, &status, 0);
        ok = ok && ((status >> 8) & 0xFF) == 0;
        muos_close(lfd);
        muos_puts(ok ? "BACC-OK\n" : "BACC-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "perm")) {
        /* uid/gid enforcement: a non-root child may read a 0644 file but
         * must be denied a 0600 file owned by root. */
        int ok = 1;
        int fd = muos_open("/permpriv", MUOS_O_CREAT | MUOS_O_WRONLY);
        if (fd < 0) { muos_puts("PERM-BAD\n"); return 0; }
        muos_write(fd, "SECRET", 6);
        muos_close(fd);
        ok = ok && muos_chmod("/permpriv", 0600) == 0;

        fd = muos_open("/permpublic", MUOS_O_CREAT | MUOS_O_WRONLY);
        if (fd < 0) { muos_puts("PERM-BAD\n"); return 0; }
        muos_write(fd, "PUBLIC", 6);
        muos_close(fd);
        ok = ok && muos_chmod("/permpublic", 0644) == 0;

        int pid = muos_fork();
        if (pid == 0) {
            muos_setuid(1000);
            muos_setgid(1000);
            int bad = muos_open("/permpriv", 0);      /* 0600 root: denied */
            int good = muos_open("/permpublic", 0);   /* 0644: allowed */
            if (good >= 0) muos_close(good);
            if (bad >= 0) muos_close(bad);
            muos_exit((bad < 0 && good >= 0) ? 0 : 1);
        }
        int status = 0;
        muos_waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xFF;
        muos_puts("perm: child="); muos_putdec(code); muos_puts("\n");
        ok = ok && code == 0;
        muos_puts(ok ? "PERM-OK\n" : "PERM-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "append")) {
        /* O_APPEND: reopening without O_TRUNC and writing must extend the
         * file; fcntl(F_SETFL) toggles the same behavior at runtime, and a
         * read-only descriptor rejects write(2) with EBADF. */
        int ok = 1;
        int fd = muos_open("append.txt",
                           MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        int w1 = fd >= 0 ? muos_write(fd, "AB", 2) : -1;
        if (fd >= 0) muos_close(fd);

        int fa = muos_open("append.txt", MUOS_O_WRONLY | MUOS_O_APPEND);
        int w2 = fa >= 0 ? muos_write(fa, "CD", 2) : -1;
        int fl = fa >= 0 ? muos_fcntl(fa, MUOS_F_GETFL, 0) : -1;
        if (fa >= 0) muos_close(fa);

        char rb[16];
        int rf = muos_open("append.txt", 0);
        int rn = rf >= 0 ? muos_read(rf, rb, sizeof(rb)) : -1;
        if (rf >= 0) muos_close(rf);
        int appended = (w1 == 2 && w2 == 2 && fl >= 0 &&
                        (fl & MUOS_O_APPEND) && rn == 4 &&
                        rb[0] == 'A' && rb[1] == 'B' &&
                        rb[2] == 'C' && rb[3] == 'D');

        int fd2 = muos_open("append.txt", MUOS_O_WRONLY);
        int fset = -1, fget = -1, w3 = -1;
        if (fd2 >= 0) {
            muos_lseek(fd2, 0, 0);
            fset = muos_fcntl(fd2, MUOS_F_SETFL, MUOS_O_APPEND);
            fget = muos_fcntl(fd2, MUOS_F_GETFL, 0);
            w3 = muos_write(fd2, "EF", 2);
            muos_close(fd2);
        }
        char rb2[16];
        int rf2 = muos_open("append.txt", 0);
        int rn2 = rf2 >= 0 ? muos_read(rf2, rb2, sizeof(rb2)) : -1;
        if (rf2 >= 0) muos_close(rf2);
        int toggled = (fset == 0 && fget >= 0 && (fget & MUOS_O_APPEND) &&
                       w3 == 2 && rn2 == 6 &&
                       rb2[4] == 'E' && rb2[5] == 'F');

        int ro = muos_open("append.txt", 0);
        int ro_w = ro >= 0 ? muos_write(ro, "XX", 2) : 0;
        if (ro >= 0) muos_close(ro);

        /* RAM FS capacity is 64KB per file: a 70KB write must report the
         * actual (short) byte count instead of silently claiming success. */
        static unsigned char big_write[71680];
        for (unsigned i = 0; i < sizeof(big_write); i++)
            big_write[i] = (unsigned char)(i & 0xFF);
        int bfd = muos_open("trunc.txt",
                            MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        int bw = bfd >= 0 ? muos_write(bfd, big_write, sizeof(big_write)) : -1;
        if (bfd >= 0) muos_close(bfd);
        union {
            char raw[MUOS_STAT64_SIZE];
            unsigned word[MUOS_STAT64_SIZE / 4];
        } tst;
        int trc = muos_fstatat64(MUOS_AT_FDCWD, "trunc.txt", tst.raw, 0);
        int trunc_ok = (bw == 65536 && trc == 0 && tst.word[11] == 65536);

        ok = ok && appended && toggled && ro_w == -9 && trunc_ok;
        muos_puts("append: write="); muos_putdec(w1);
        muos_puts("/"); muos_putdec(w2);
        muos_puts(" read="); muos_putdec(rn);
        muos_puts(" getfl="); muos_putdec(fl); muos_puts("\n");
        muos_puts("append: fcntl-set="); muos_putdec(fset);
        muos_puts(" getfl="); muos_putdec(fget);
        muos_puts(" read="); muos_putdec(rn2);
        muos_puts(" ro-write="); muos_putdec(ro_w); muos_puts("\n");
        muos_puts("append: short-write="); muos_putdec(bw);
        muos_puts(" size="); muos_putdec((int)tst.word[11]);
        muos_puts(trunc_ok ? " trunc-ok\n" : " trunc-bad\n");
        muos_puts(ok ? "APPEND-OK\n" : "APPEND-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "nullpage")) {
        /* Linux keeps the first page unmapped so a NULL dereference faults.
         * MAP_FIXED at 0 (or anywhere below the sandbox base) must fail,
         * /proc/self/maps must not list 00000000, and a child that touches
         * NULL must die with SIGSEGV without harming its parent. */
        int ok = 1;
        unsigned at0 = muos_mmap_at(0, 4096, MUOS_PROT_READ | MUOS_PROT_WRITE,
                                    MUOS_MAP_FIXED | MUOS_MAP_ANONYMOUS);
        unsigned below = muos_mmap_at(0x08047000u, 4096,
                                      MUOS_PROT_READ | MUOS_PROT_WRITE,
                                      MUOS_MAP_FIXED | MUOS_MAP_ANONYMOUS);
        int maps_null = maps_count("00000000-");
        int at0_err = (int)at0 < 0;
        int below_err = (int)below < 0;
        muos_puts("null: mmap0="); muos_putdec(at0_err);
        muos_puts(" below-base="); muos_putdec(below_err);
        muos_puts(" maps-null="); muos_putdec(maps_null); muos_puts("\n");
        ok = ok && at0_err && below_err && maps_null == 0;

        int pid = muos_fork();
        if (pid == 0) {
            *(volatile int*)0 = 1;      /* must fault, not corrupt the kernel */
            muos_exit(7);               /* unreachable */
        }
        int st = 0;
        int wp = muos_waitpid(pid, &st, 0);
        int code = (st >> 8) & 0xFF;
        int child_ok = (pid > 0 && wp == pid && code == 139);
        muos_puts("null: fork="); muos_putdec(pid);
        muos_puts(" wait="); muos_putdec(wp);
        muos_puts(" code="); muos_putdec(code); muos_puts("\n");
        ok = ok && child_ok;
        muos_puts(ok ? "NULLPAGE-OK\n" : "NULLPAGE-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "rand")) {
        /* getrandom(2) and /dev/urandom must return non-trivial, distinct
         * byte streams; unknown flags are rejected with EINVAL. */
        unsigned char a[64], b[64], c[32];
        int r1 = muos_getrandom(a, sizeof(a), 0);
        int r2 = muos_getrandom(b, sizeof(b), 0);
        int diff = 0, nz = 0;
        for (unsigned i = 0; i < sizeof(a); i++) {
            if (a[i] != b[i]) diff++;
            if (a[i]) nz++;
        }

        int bad_flags = muos_getrandom(a, 4, 0x8);
        int zero_len = muos_getrandom(a, 0, 0);

        int fd = muos_open("/dev/urandom", 0);
        int rn = fd >= 0 ? muos_read(fd, c, sizeof(c)) : -1;
        int c_diff = 0, c_nz = 0;
        for (unsigned i = 0; i < sizeof(c); i++) {
            if (c[i] != a[i % sizeof(a)]) c_diff++;
            if (c[i]) c_nz++;
        }
        if (fd >= 0) muos_close(fd);

        /* /dev/random is an alias of the same entropy source. */
        int rf = muos_open("/dev/random", 0);
        int rfr = rf >= 0 ? muos_read(rf, c, 4) : -1;
        if (rf >= 0) muos_close(rf);

        int ok = (r1 == 64 && r2 == 64 && diff >= 48 && nz >= 24 &&
                  bad_flags == -22 && zero_len == 0 &&
                  rn == 32 && c_diff >= 24 && c_nz >= 12 && rfr == 4);
        muos_puts("rand: getrandom="); muos_putdec(r1);
        muos_puts("/"); muos_putdec(r2);
        muos_puts(" diff="); muos_putdec(diff);
        muos_puts(" nonzero="); muos_putdec(nz); muos_puts("\n");
        muos_puts("rand: bad-flags="); muos_putdec(bad_flags);
        muos_puts(" zero-len="); muos_putdec(zero_len);
        muos_puts(" urandom="); muos_putdec(rn);
        muos_puts(" diff="); muos_putdec(c_diff);
        muos_puts(" nonzero="); muos_putdec(c_nz); muos_puts("\n");
        muos_puts(ok ? "RAND-OK\n" : "RAND-BAD\n");
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

    if (argc > 1 && arg_is(argv[1], "futex")) {
        /* FUTEX_WAIT/WAKE across two CLONE_VM threads, plus the
         * value-mismatch (EAGAIN) and timed-wait (ETIMEDOUT) paths. */
        int ok = 1;
        int st = 0;

        futex_word = 5;
        int eagain = muos_futex((void*)&futex_word, MUOS_FUTEX_WAIT, 4, 0);
        ok = ok && eagain == -11;

        futex_word = 0;
        futex_wake_ret = -1;
        int tw = muos_thread_start(futex_waker, 0,
                                   &thread_stacks[0][0] + 4096);
        int rc = muos_futex((void*)&futex_word, MUOS_FUTEX_WAIT, 0, 0);
        int ww = muos_waitpid(tw, &st, 0);
        int woke = (rc == 0 && ww == tw && ((st >> 8) & 0xFF) == 0 &&
                    futex_word == 1 && futex_wake_ret == 1);

        futex_word = 0;
        int tsleep = muos_thread_start(futex_sleeper, 0,
                                       &thread_stacks[0][0] + 4096);
        unsigned ts[2] = { 0, 30000000 };       /* 30ms */
        int trc = muos_futex((void*)&futex_word, MUOS_FUTEX_WAIT, 0, ts);
        int ws = muos_waitpid(tsleep, &st, 0);
        int timed = (trc == -110 && ws == tsleep);

        futex_req_a = 0; futex_req_b = 0;
        futex_req_started = 0; futex_req_ret = 9;
        int rqth = muos_thread_start(futex_requeue_waiter, 0,
                                     &thread_stacks[0][0] + 4096);
        /* Busy-wait (staying runnable) so the waiter can block and the PIT
         * can switch back to us; nanosleep here leaves no runnable process
         * for the waiter's block to switch to. */
        for (volatile unsigned spin = 0; spin < 20000000u; spin++) { }
        int rq = muos_futex_full(&futex_req_a, 3, 0, 1, &futex_req_b, 0);
        int rqw = muos_futex(&futex_req_b, MUOS_FUTEX_WAKE, 1, 0);
        int rqst = 0;
        int rqww = muos_waitpid(rqth, &rqst, 0);
        int requeue_ok = (rq == 0 && rqw == 1 && rqww == rqth &&
                          ((rqst >> 8) & 0xFF) == 0);

        robust_word = 0;
        int rbth = muos_thread_start(robust_thread, 0,
                                     &thread_stacks[0][0] + 4096);
        int rbww = muos_waitpid(rbth, &st, 0);
        int robust_ok = (rbww == rbth && robust_word == 0);

        ok = ok && woke && timed && requeue_ok && robust_ok;
        muos_puts("futex: eagain="); muos_putdec(eagain);
        muos_puts(" wait="); muos_putdec(rc);
        muos_puts(" wake="); muos_putdec(futex_wake_ret);
        muos_puts(" timeout="); muos_putdec(trc);
        muos_puts(" requeue="); muos_putdec(rq);
        muos_puts("/"); muos_putdec(rqw);
        muos_puts("/"); muos_putdec(rqww);
        muos_puts(" robust="); muos_putdec(robust_word);
        muos_puts("/"); muos_putdec(rbww);
        muos_puts(" rqm="); muos_putdec(futex_req_started);
        muos_puts("/"); muos_putdec(futex_req_ret);
        muos_puts("/"); muos_putdec(futex_req_seen);
        muos_puts(" flags="); muos_putdec(woke);
        muos_putdec(timed); muos_putdec(requeue_ok); muos_putdec(robust_ok);
        muos_puts("\n");
        muos_puts(ok ? "FUTEX-OK\n" : "FUTEX-BAD\n");
        return 0;
    }

    if (argc > 1 && arg_is(argv[1], "abi")) {
        /* Small libc-startup ABI batch: prctl names, madvise/fadvise64,
         * single-CPU affinity, tkill/tgkill and sched_yield. */
        int ok = 1;
        char nm[16];
        nm[0] = 0;
        int ps = muos_prctl(15, (unsigned)"muabi", 0, 0, 0);
        int pg = muos_prctl(16, (unsigned)nm, 0, 0, 0);
        int name_ok = (ps == 0 && pg == 0 && nm[0] == 'm' && nm[1] == 'u' &&
                       nm[2] == 'a' && nm[3] == 'b' && nm[4] == 'i');

        unsigned char* mp = (unsigned char*)muos_mmap(4096);
        int ma = mp ? muos_madvise((unsigned)mp, 4096, 0) : -1;
        if (mp) muos_munmap((unsigned)mp, 4096);

        int fd = muos_open("abi.txt",
                           MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        int fa = -1;
        if (fd >= 0) {
            muos_write(fd, "x", 1);
            fa = muos_fadvise64(fd, 0, 0, 4096, 0);
            muos_close(fd);
        }

        unsigned m1 = 1, m2 = 0;
        int as = muos_sched_setaffinity(0, 4, &m1);
        int ag = muos_sched_getaffinity(0, 4, &m2);
        int tk = muos_tkill(muos_getpid(), 0);
        int tg = muos_tgkill(muos_getpid(), muos_gettid(), 0);
        int sy = muos_sched_yield();

        int fdc = muos_open("abi2.txt",
                            MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        int fdset = fdc >= 0 ? muos_fcntl(fdc, MUOS_F_SETFD, MUOS_FD_CLOEXEC) : -1;
        int fdget = fdc >= 0 ? muos_fcntl(fdc, MUOS_F_GETFD, 0) : -1;
        if (fdc >= 0) muos_close(fdc);

        int pfds[2];
        int pr = muos_pipe(pfds);
        int nbset = pr == 0 ? muos_fcntl(pfds[0], MUOS_F_SETFL,
                                         MUOS_O_NONBLOCK) : -1;
        char nbbuf[1];
        int nbrd = pr == 0 ? muos_read(pfds[0], nbbuf, 1) : -1;
        if (pr == 0) { muos_close(pfds[0]); muos_close(pfds[1]); }

        unsigned tsleep[2] = { 0, 10000000 };       /* 10ms */
        int cn = muos_clock_nanosleep(1, 0, tsleep, 0);

        unsigned char mcvec[2] = { 0, 0 };
        unsigned mc_addr = muos_mmap(8192);
        int minc = -1;
        if (mc_addr) {
            *(volatile unsigned char*)mc_addr = 1;  /* make page resident */
            minc = muos_mincore(mc_addr, 8192, mcvec);
            muos_munmap(mc_addr, 8192);
        }

        unsigned char* rm = (unsigned char*)muos_mmap(4096);
        int remap_ok = 0;
        if (rm) {
            rm[0] = 'A'; rm[4095] = 'Z';
            unsigned nr = muos_mremap((unsigned)rm, 4096, 8192, 1, 0);
            if ((int)nr > 0) {
                unsigned char* np = (unsigned char*)nr;
                remap_ok = (np[0] == 'A' && np[4095] == 'Z' && np[4096] == 0);
                muos_munmap(nr, 8192);
            }
        }

        unsigned cpu = 9, node = 9;
        int gc = muos_getcpu(&cpu, &node);

        unsigned cres[2] = { 0, 0 };
        int cr = muos_clock_getres(1, cres);

        /* statx flag/mask validation and AT_EMPTY_PATH semantics. */
        int sx_bad_mask = muos_statx(MUOS_AT_FDCWD, "abi.txt", 0,
                                     0x80000000u, statx_buf);
        int sx_bad_flag = muos_statx(MUOS_AT_FDCWD, "abi.txt",
                                     0x80000000u, 0, statx_buf);
        int sx_empty = muos_statx(MUOS_AT_FDCWD, "", 0, 0, statx_buf);
        int sfd = muos_open("abi.txt", 0);
        unsigned sx_mask = 0;
        int sx_ok = -1;
        if (sfd >= 0) {
            sx_ok = muos_statx(sfd, "", MUOS_AT_EMPTY_PATH, MUOS_STATX_ALL,
                               statx_buf);
            if (sx_ok == 0) sx_mask = ((unsigned*)statx_buf)[0];
            muos_close(sfd);
        }

        /* sendfile boundaries: past EOF -> 0, negative offset -> EINVAL. */
        int sfo = muos_open("abi-copy.txt",
                            MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_WRONLY);
        int sfi = muos_open("abi.txt", 0);
        int sf_eof = -1, sf_neg = -1, sf_ok = -1;
        if (sfo >= 0 && sfi >= 0) {
            int off_eof = 8;
            sf_eof = muos_sendfile(sfo, sfi, &off_eof, 16);
            int off_neg = -1;
            sf_neg = muos_sendfile(sfo, sfi, &off_neg, 16);
            int off_zero = 0;
            sf_ok = muos_sendfile(sfo, sfi, &off_zero, 1);
        }
        if (sfo >= 0) muos_close(sfo);
        if (sfi >= 0) muos_close(sfi);

        ok = ok && name_ok && ma == 0 && fa == 0 && as == 0 && ag == 0 &&
             m2 == 1 && tk == 0 && tg == 0 && sy == 0 &&
             fdset == 0 && fdget == 1 && nbset == 0 && nbrd == -11 &&
             cn == 0 && minc == 0 && (mcvec[0] & 1) &&
             remap_ok && gc == 0 && cpu == 0 && node == 0 &&
             cr == 0 && cres[0] == 0 && cres[1] == 10000000 &&
             sx_bad_mask == -22 && sx_bad_flag == -22 && sx_empty == -2 &&
             sx_ok == 0 && sx_mask == 0x7ff &&
             sf_eof == 0 && sf_neg == -22 && sf_ok == 1;
        muos_puts("abi: prctl="); muos_putdec(ps);
        muos_puts("/"); muos_putdec(pg);
        muos_puts(" name="); muos_puts(nm);
        muos_puts("\nabi: madvise="); muos_putdec(ma);
        muos_puts(" fadvise="); muos_putdec(fa);
        muos_puts(" affinity="); muos_putdec(as);
        muos_puts("/"); muos_putdec(ag);
        muos_puts(" mask="); muos_putdec((int)m2);
        muos_puts(" tkill="); muos_putdec(tk);
        muos_puts(" tgkill="); muos_putdec(tg);
        muos_puts(" yield="); muos_putdec(sy); muos_puts("\n");
        muos_puts("abi: cloexec="); muos_putdec(fdset);
        muos_puts("/"); muos_putdec(fdget);
        muos_puts(" nonblock="); muos_putdec(nbset);
        muos_puts("/"); muos_putdec(nbrd);
        muos_puts(" clock_ns="); muos_putdec(cn);
        muos_puts(" mincore="); muos_putdec(minc);
        muos_puts("/"); muos_putdec((int)mcvec[0]);
        muos_puts(" mremap="); muos_putdec(remap_ok);
        muos_puts(" getcpu="); muos_putdec(gc);
        muos_puts("/"); muos_putdec((int)cpu);
        muos_puts("/"); muos_putdec((int)node);
        muos_puts(" getres="); muos_putdec(cr);
        muos_puts("/"); muos_putdec((int)cres[1]);
        muos_puts(" statx="); muos_putdec(sx_bad_mask);
        muos_puts("/"); muos_putdec(sx_bad_flag);
        muos_puts("/"); muos_putdec(sx_empty);
        muos_puts("/"); muos_putdec(sx_ok);
        muos_puts("/"); muos_putdec((int)sx_mask);
        muos_puts(" sendfile="); muos_putdec(sf_eof);
        muos_puts("/"); muos_putdec(sf_neg);
        muos_puts("/"); muos_putdec(sf_ok); muos_puts("\n");
        muos_puts(ok ? "ABI-OK\n" : "ABI-BAD\n");
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
        fd = muos_open("p1file", MUOS_O_CREAT | MUOS_O_TRUNC | MUOS_O_RDWR);
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

            /* MAP_SHARED write-back: memory stores must reach the file
             * through msync (and munmap). The file currently holds 64
             * zero bytes, so the first 8 bytes are rewritten here. */
            char* sh = (char*)muos_mmap_file(fd, 4096,
                                             MUOS_PROT_READ | MUOS_PROT_WRITE,
                                             MUOS_MAP_SHARED, 0);
            int shared_ok = 0;
            if (sh && (unsigned)sh >= 0x08048000u) {
                for (unsigned i = 0; i < 8; i++) sh[i] = (char)('A' + i);
                rc = muos_msync((unsigned)sh, 4096, 0);
                char rb[8];
                muos_lseek(fd, 0, 0);
                int n = muos_read(fd, rb, 8);
                shared_ok = (rc == 0 && n == 8);
                for (unsigned i = 0; shared_ok && i < 8; i++)
                    if (rb[i] != (char)('A' + i)) shared_ok = 0;
                muos_puts(shared_ok ? "mfile: shared-ok\n" : "mfile: shared-bad\n");
                ok = ok && shared_ok;
                rc = muos_munmap((unsigned)sh, 4096);
                ok = ok && rc == 0;
            }

            /* mprotect: drop to read-only, then re-enable writes. */
            char* mp = (char*)muos_mmap(4096);
            int prot_ok = 0;
            int maps_ok = 0;
            if (mp && (unsigned)mp >= 0x08048000u) {
                mp[0] = 'X';
                int maps_anon = maps_count("[anon]");
                rc = muos_mprotect((unsigned)mp, 4096, MUOS_PROT_READ);
                int ro_ok = (rc == 0 && mp[0] == 'X');
                int maps_ro = maps_count("r--p");
                rc = muos_mprotect((unsigned)mp, 4096,
                                   MUOS_PROT_READ | MUOS_PROT_WRITE);
                mp[1] = 'Y';
                int maps_rw = maps_count("rw-p");
                prot_ok = (ro_ok && rc == 0 && mp[1] == 'Y');
                muos_puts(prot_ok ? "mfile: mprotect-ok\n" : "mfile: mprotect-bad\n");
                ok = ok && prot_ok;
                muos_munmap((unsigned)mp, 4096);
                int maps_after = maps_count("[anon]");
                /* /proc/self/maps must track the VMA table across mmap,
                 * mprotect and munmap. */
                maps_ok = (maps_anon >= 1 && maps_ro == 1 && maps_rw == 1 &&
                           maps_after == 0);
                muos_puts(maps_ok ? "mfile: maps-ok\n" : "mfile: maps-bad\n");
                ok = ok && maps_ok;
            }

            /* mprotect over a just-munmapped page must fault it back in and
             * return 0; the fresh page is readable zero-filled memory. */
            char* mr = (char*)muos_mmap(4096);
            if (mr && (unsigned)mr >= 0x08048000u) {
                muos_munmap((unsigned)mr, 4096);
                rc = muos_mprotect((unsigned)mr, 4096,
                                   MUOS_PROT_READ | MUOS_PROT_WRITE);
                int remap_ok = (rc == 0 && mr[0] == 0 && mr[4095] == 0);
                muos_puts(remap_ok ? "mfile: mprotect-remap-ok\n"
                                   : "mfile: mprotect-remap-bad\n");
                ok = ok && remap_ok;
                muos_munmap((unsigned)mr, 4096);
            }

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
