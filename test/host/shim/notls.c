/* Host shim, without mbedTLS's sources (no ESP-IDF tree): kiwi_tls.h with
 * no TLS behind it -- an https:// receiver is a handshake that fails, said
 * so, and the harnesses speak in the clear only. */
#include <stdio.h>
#include <stdlib.h>

#include "kiwi_tls.h"

struct kiwi_tls { int fd; };

kiwi_tls_t *kiwi_tls_new(int fd, const char *host, uint32_t key)
{
    (void)host;
    (void)key;
    kiwi_tls_t *t = calloc(1, sizeof *t);
    if (t) t->fd = fd;
    return t;
}

int    kiwi_tls_shake(kiwi_tls_t *t, bool *wr) { (void)t; (void)wr; return -1; }
int    kiwi_tls_read(kiwi_tls_t *t, void *b, size_t n) { (void)t; (void)b; (void)n; return -1; }
int    kiwi_tls_write(kiwi_tls_t *t, const void *b, size_t n, bool *wr) { (void)t; (void)b; (void)n; (void)wr; return -1; }
size_t kiwi_tls_pending(kiwi_tls_t *t) { (void)t; return 0; }
bool   kiwi_tls_cert_refused(const kiwi_tls_t *t) { (void)t; return false; }
void   kiwi_tls_why(const kiwi_tls_t *t, char *out, size_t cap) { (void)t; snprintf(out, cap, "not built in"); }
const char *kiwi_tls_suite(const kiwi_tls_t *t) { (void)t; return ""; }
int64_t kiwi_tls_work_us(const kiwi_tls_t *t) { (void)t; return 0; }
int64_t kiwi_tls_turn_us(const kiwi_tls_t *t) { (void)t; return 0; }
void   kiwi_tls_free(kiwi_tls_t *t) { free(t); }
