/* tls.c - exercises TLS in the main executable and in libtls.so. */
#include "muos.h"

extern int tls_add(int v);
extern int tls_add_gd(int v);

static __thread int main_tls = 7;

int main(void) {
    int ok = 1;

    int a = tls_add(1);
    muos_puts("tls: ie counter="); muos_putdec(a); muos_puts("\n");
    ok = ok && a == 101;

    int b = tls_add(2);
    muos_puts("tls: ie counter="); muos_putdec(b); muos_puts("\n");
    ok = ok && b == 103;

    int c = tls_add_gd(1);
    muos_puts("tls: gd counter="); muos_putdec(c); muos_puts("\n");
    ok = ok && c == 201;

    int d = tls_add_gd(2);
    muos_puts("tls: gd counter="); muos_putdec(d); muos_puts("\n");
    ok = ok && d == 203;

    main_tls += 5;
    muos_puts("tls: main var="); muos_putdec(main_tls); muos_puts("\n");
    ok = ok && main_tls == 12;

    muos_puts(ok ? "TLS-OK\n" : "TLS-BAD\n");
    return 0;
}
