/* libmu.c - the first MuOS shared library (libmu.so).
 *
 * Built standalone with -fPIC and linked as ET_DYN. It has no dependencies:
 * the user-mode dynamic linker (ld-mu.so) maps it, applies its RELATIVE
 * relocations and resolves its exported symbols for the program that needs
 * it (DT_NEEDED libmu.so).
 */

__attribute__((visibility("default")))
int mu_magic = 0x4D754F53;      /* 'MuOS': COPY-relocation test data */

__attribute__((visibility("default")))
int mu_hello(void) {
    /* A pointer initialised with a link-time address needs an R_386_RELATIVE
     * relocation, which is what proves the user-mode linker relocated us. */
    static const char* volatile banner = "DYN-LIB-OK\n";
    const char* msg = banner;
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(4), "b"(1),
                        "c"((int)msg), "d"(11)
                      : "memory");
    return 7;
}
