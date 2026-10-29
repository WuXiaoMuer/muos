/* hello.c - first C program running in Ring3 under MuOS.
 * Built freestanding for the Linux i386 syscall ABI (int 0x80). */
#include "muos.h"

#define USER_MARKER "USER-HELLO-OK"

static unsigned read_cs(void) {
    unsigned cs;
    __asm__ volatile ("push %%cs\n\tpop %0" : "=r"(cs));
    return cs & 0xFFFF;
}

int main(int argc, char** argv) {
    unsigned cs = read_cs();

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
