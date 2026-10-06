/* TLS for a receiver behind a front -- the kiwisdr.com proxy, Cloudflare --
 * which speaks KiwiSDR's protocol only over https:// and wss://: mbedTLS on a
 * socket kiwi_sess.c has connected, the server's certificate checked against
 * the certificate bundle for the name the address gives, which is also the
 * name asked for (SNI) -- its chain and its name, not its dates: the
 * firmware's mbedTLS has no calendar for them (no
 * CONFIG_MBEDTLS_HAVE_TIME_DATE). kiwi_sess.c's own; not for anything else.
 *
 * Nothing here waits on the network: the socket is non-blocking, and the
 * caller waits on it with select() a slice at a time, its go_on asked in
 * between. A whole handshake is done in software on the S3, a second or two
 * of it, so it goes a step at a time (mbedtls_ssl_handshake_step), the caller
 * back in between: its heaviest step -- the server's certificates checked, or
 * the key reckoned -- is the longest it is not asked, half the whole at most
 * on the PC, up to a second or so on the S3. And one connection's step at a
 * time, whichever task it is on (the two ears, the web SDR, a Test), the
 * idle task's turn after each long one: the receivers' handshakes keep the
 * session tasks' core from its idle task for one such step at most, however
 * many shake at once -- never near the task watchdog's 5 s. The newest few
 * receivers' sessions are kept, so a connection after the first -- the
 * session after its /status, a Test's login after its read, the same
 * receiver again after a switch or a drop -- resumes with none of that,
 * where the front takes it back.
 *
 * Everything is in PSRAM: the contexts in the block kiwi_tls_new() takes, and
 * mbedTLS's own records, certificates and handshake by
 * CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC. No internal RAM beyond the socket's. */
#ifndef KIWI_TLS_H
#define KIWI_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kiwi_tls kiwi_tls_t;

/* A client of `host` on the connected socket `fd`, which it makes
 * non-blocking; a session kept for `key` (the receiver's address) offered
 * for resumption. NULL without the memory. The socket stays the caller's. */
kiwi_tls_t *kiwi_tls_new(int fd, const char *host, uint32_t key);

/* The handshake a step further: 1 done; 0 more once the socket is readable
 * -- or writable, *wr; 2 more at once, the caller's go_on asked first (a step
 * done, or another connection's under way, waited for a moment); -1
 * failed. */
int kiwi_tls_shake(kiwi_tls_t *t, bool *wr);

/* >0 bytes read; 0 none yet; -2 the far end closed its side (a close_notify,
 * the connection's end, a reset); -1 failed. */
int kiwi_tls_read(kiwi_tls_t *t, void *b, size_t n);

/* >0 bytes written; 0 none -- the socket full: the same bytes again once it
 * is writable (*wr) or, should mbedTLS want to read first, readable; -1
 * failed. */
int kiwi_tls_write(kiwi_tls_t *t, const void *b, size_t n, bool *wr);

/* Bytes decrypted and not read yet: select() on the socket will not say so. */
size_t kiwi_tls_pending(kiwi_tls_t *t);

/* After a failure: whether its certificate is what was refused -- signed by
 * no authority in the bundle, or not for that name -- and in words, for the
 * log: why (what the certificate failed on, or mbedTLS's code). After a
 * handshake: its cipher suite. And of its handshake so far, µs: how long its
 * steps took -- the knob's own computing, a resumed one's next to nothing --
 * and how long it waited for other connections' steps; neither the front's
 * time. */
bool        kiwi_tls_cert_refused(const kiwi_tls_t *t);
void        kiwi_tls_why(const kiwi_tls_t *t, char *out, size_t cap);
const char *kiwi_tls_suite(const kiwi_tls_t *t);
int64_t     kiwi_tls_work_us(const kiwi_tls_t *t);
int64_t     kiwi_tls_turn_us(const kiwi_tls_t *t);

/* A close_notify, if the socket takes it now, and all of it freed -- the
 * socket left for the caller to close. A session that failed its handshake
 * is not offered again. */
void kiwi_tls_free(kiwi_tls_t *t);

#endif /* KIWI_TLS_H */
