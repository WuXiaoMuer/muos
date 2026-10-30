/* tlslib.c - shared library with thread-local storage (TLS) for `run tls`.
 *
 * Two TLS models are exercised by the same object:
 *  - initial-exec: the GOT slot is a TP-relative offset resolved by
 *    ld-mu.so (R_386_TLS_TPOFF).
 *  - global-dynamic: access goes through __tls_get_addr() with a
 *    (module, offset) pair (R_386_TLS_DTPMOD32/DTPOFF32). */
int tls_add(int v) {
    static __thread int ie_counter __attribute__((tls_model("initial-exec"))) = 100;
    ie_counter += v;
    return ie_counter;
}

__attribute__((visibility("default")))
int tls_add_gd(int v) {
    static __thread int gd_counter = 200;
    gd_counter += v;
    return gd_counter;
}
