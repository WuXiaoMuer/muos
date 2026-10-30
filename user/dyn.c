/* dyn.c - dynamically linked Ring3 program.
 *
 * Linked as a PIE with PT_INTERP=/lib/ld-mu.so and DT_NEEDED libmu.so. The
 * kernel maps this image, maps the interpreter and jumps to ld-mu.so, which
 * maps libmu.so, resolves mu_hello() and only then transfers control here.
 */
#include "muos.h"

extern int mu_hello(void);

int main(void) {
    int v = mu_hello();          /* undefined symbol in this object */
    muos_puts("dyn: lib returned "); muos_putdec(v); muos_puts("\n");
    muos_puts(v == 7 ? "DYN-OK\n" : "DYN-BAD\n");
    return 0;
}
