/* Host shim: the certificate bundle kiwi_tls.c attaches, on the PC, the way
 * ESP-IDF's esp_crt_bundle_attach() does it (v5.5.5): a dummy CA chain, and a
 * verify callback that takes the top of the server's chain for trusted where
 * an authority in the bundle signed it. The bundle here is the one CA a test
 * run trusts, the PEM file KIWI_TEST_CA names, made for that run alone;
 * without it, none, and every certificate is refused. So a chain no
 * authority in it signed fails as it does on the knob: mbedTLS makes the
 * callback's refusal a fatal error, its flags all set. Read once, whichever
 * task asks first. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static pthread_once_t   s_once = PTHREAD_ONCE_INIT;
static mbedtls_x509_crt s_ca, s_dummy;

static void load(void)
{
    mbedtls_x509_crt_init(&s_ca);
    mbedtls_x509_crt_init(&s_dummy);
    const char *p = getenv("KIWI_TEST_CA");
    if (p && *p && mbedtls_x509_crt_parse_file(&s_ca, p) != 0)
        fprintf(stderr, "shim_tls: %s is no CA certificate\n", p);
}

/* esp_crt_verify_callback's way: a certificate that is wrong only in being
 * untrusted is looked up by its issuer, and its signature checked with that
 * authority's key; anything else wrong with it is mbedTLS's to say. */
static int verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    (void)depth;
    if ((*flags & ~MBEDTLS_X509_BADCERT_BAD_MD) != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return 0;
    if (s_ca.raw.len && crt->issuer_raw.len == s_ca.subject_raw.len &&
        !memcmp(crt->issuer_raw.p, s_ca.subject_raw.p, crt->issuer_raw.len)) {
        unsigned char hash[MBEDTLS_MD_MAX_SIZE];
        const mbedtls_md_type_t mt = crt->MBEDTLS_PRIVATE(sig_md);
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(mt);
        if (md && mbedtls_md(md, crt->tbs.p, crt->tbs.len, hash) == 0 &&
            mbedtls_pk_verify_ext(crt->MBEDTLS_PRIVATE(sig_pk), crt->MBEDTLS_PRIVATE(sig_opts), &s_ca.pk, mt, hash,
                                  mbedtls_md_get_size(md), crt->MBEDTLS_PRIVATE(sig).p,
                                  crt->MBEDTLS_PRIVATE(sig).len) == 0) {
            *flags = 0;
            return 0;
        }
    }
    return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    pthread_once(&s_once, load);
    mbedtls_ssl_conf_ca_chain(conf, &s_dummy, NULL);
    mbedtls_ssl_conf_verify(conf, verify, NULL);
    return ESP_OK;
}
