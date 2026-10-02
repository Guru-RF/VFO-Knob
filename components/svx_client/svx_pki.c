/* The station's credentials in NVS, with mbedTLS: SVXConnect-CLI's pki.c and
 * cert.c, from files and OpenSSL to the knob. See svx_pki.h for the rules. */
#include "svx_pki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"

#include "common/proto.h"
#include "svx_flash.h"

static const char *TAG = "svx-pki";

#define NS       "svxpki"
#define PEM_MAX  4096               /* a key, a request or a certificate */
#define CA_MAX   16384              /* the reflector's whole bundle */
#define DAY      86400

static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;
static portMUX_TYPE      s_mx_init = portMUX_INITIALIZER_UNLOCKED;
static bool              s_loaded;

/* PEM text in PSRAM, NUL-terminated; NULL when absent. */
static struct {
    char    *key, *csr, *crt, *ca;
    char     csr_for[168];          /* "CALL email" the request was made for */
    uint8_t  refused[32];           /* SHA-256 of the refused certificate */
    bool     have_refused;
    bool     pending;
    int64_t  requested;
    /* Whether the stored certificate carries the stored key: an RSA check,
     * made once per certificate rather than on every look at the page. */
    uint8_t  match_fp[32];
    bool     match_known, match_ok;
} P;

static void lock(void);
#define LOCK()   lock()
#define UNLOCK() xSemaphoreGive(s_mx)

/* ------------------------------------------------------------------ misc */

static int rng(void *p, unsigned char *out, size_t len)
{
    (void)p;
    esp_fill_random(out, len);
    return 0;
}

/* For key generation, which runs for seconds without ever blocking: every
 * candidate prime asks for randomness, and giving the scheduler a tick there
 * keeps the idle task, and with it the task watchdog, alive. */
static int rng_yield(void *p, unsigned char *out, size_t len)
{
    (void)p;
    esp_fill_random(out, len);
    vTaskDelay(1);
    return 0;
}

static char *dup_psram(const char *s, size_t n)
{
    char *p = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *copy_of(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm): the
 * knob has no timegm(), and mktime() would drag the time zone in. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t x509_epoch(const mbedtls_x509_time *t)
{
    return days_from_civil(t->year, t->mon, t->day) * DAY
         + t->hour * 3600 + t->min * 60 + t->sec;
}

/* ------------------------------------------------------------------- NVS */

static char *nvs_load(nvs_handle_t h, const char *k)
{
    size_t n = 0;
    if (nvs_get_blob(h, k, NULL, &n) != ESP_OK || n == 0) return NULL;
    char *p = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!p) return NULL;
    if (nvs_get_blob(h, k, p, &n) != ESP_OK) { free(p); return NULL; }
    p[n] = 0;
    return p;
}

/* n == 0 erases the key. */
static esp_err_t nvs_save_now(const char *k, const void *v, size_t n)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = n ? nvs_set_blob(h, k, v, n) : nvs_erase_key(h, k);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "cannot store %s: %s", k, esp_err_to_name(e));
    return e;
}

typedef struct {
    const char *k;
    const void *v;
    size_t      n;
    esp_err_t   r;
} save_job_t;

static void save_job(void *p)
{
    save_job_t *j = p;
    j->r = nvs_save_now(j->k, j->v, j->n);
}

static esp_err_t nvs_save(const char *k, const void *v, size_t n)
{
    save_job_t j = { .k = k, .v = v, .n = n, .r = ESP_FAIL };
    svx_flash_safe(save_job, &j);
    return j.r;
}

static esp_err_t save_str(const char *k, const char *s)
{
    return nvs_save(k, s, s ? strlen(s) : 0);
}

/* What NVS holds, into P; the mutex is held. */
static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;     /* nothing yet */
    P.key = nvs_load(h, "key");
    P.csr = nvs_load(h, "csr");
    P.crt = nvs_load(h, "crt");
    size_t n = sizeof P.csr_for - 1;
    if (nvs_get_blob(h, "csrfor", P.csr_for, &n) == ESP_OK) P.csr_for[n] = 0;
    n = sizeof P.refused;
    P.have_refused = nvs_get_blob(h, "refused", P.refused, &n) == ESP_OK && n == 32;
    uint8_t pend = 0;
    P.pending = nvs_get_u8(h, "pending", &pend) == ESP_OK && pend;
    nvs_get_i64(h, "reqtime", &P.requested);
    /* The CA bundle is kept in memory only (svx_pki_store_ca): the reflector
     * sends it before every TLS start, and it is no trust anchor. A copy an
     * older firmware stored goes, once -- some 3 kB, and room the
     * certificate needs in a shared NVS that had filled up (2026-10-02:
     * "cannot store crt: ESP_ERR_NVS_NOT_ENOUGH_SPACE"). */
    size_t ca_n = 0;
    const bool old_ca = nvs_get_blob(h, "ca", NULL, &ca_n) == ESP_OK;
    nvs_close(h);
    if (old_ca && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, "ca") == ESP_OK && nvs_commit(h) == ESP_OK)
            ESP_LOGI(TAG, "the stored CA bundle erased (%u bytes): it is kept in memory now",
                     (unsigned)ca_n);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "key %s, request %s, certificate %s%s",
             P.key ? "yes" : "no", P.csr ? "yes" : "no", P.crt ? "yes" : "no",
             P.pending ? "; a request is pending" : "");
}

static void load_job(void *p)
{
    (void)p;
    load();
}

/* The first caller -- the reflector task, or the web page before WiFi has
 * brought the task up -- makes the mutex and loads NVS. The mutex is static,
 * so making it allocates nothing and may happen under a spinlock. */
static void lock(void)
{
    taskENTER_CRITICAL(&s_mx_init);
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    taskEXIT_CRITICAL(&s_mx_init);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (!s_loaded) {
        s_loaded = true;
        svx_flash_safe(load_job, NULL);
    }
}

esp_err_t svx_pki_init(void)
{
    LOCK();
    UNLOCK();
    return ESP_OK;
}

/* -------------------------------------------------------- certificates */

/* The first certificate of a PEM text: the client's own. What follows it, if
 * anything, is the reflector's CA, and the CLI presents only the first. */
static int crt_parse_first(mbedtls_x509_crt *c, const char *pem)
{
    mbedtls_x509_crt_init(c);
    if (!pem) return -1;
    const char *end = strstr(pem, "-----END CERTIFICATE-----");
    if (!end) return -1;
    size_t n = (size_t)(end - pem) + strlen("-----END CERTIFICATE-----");
    char *one = dup_psram(pem, n);
    if (!one) return -1;
    int r = mbedtls_x509_crt_parse(c, (const unsigned char *)one, n + 1);
    free(one);
    return r;
}

static void crt_cn(const mbedtls_x509_crt *c, char *out, size_t cap)
{
    out[0] = 0;
    for (const mbedtls_x509_name *n = &c->subject; n; n = n->next) {
        if (n->oid.p && MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) {
            size_t len = n->val.len < cap - 1 ? n->val.len : cap - 1;
            memcpy(out, n->val.p, len);
            out[len] = 0;
            return;
        }
    }
}

static void crt_fp(const mbedtls_x509_crt *c, uint8_t fp[32])
{
    mbedtls_sha256(c->raw.p, c->raw.len, fp, 0);
}

/* Does the certificate carry our public key? */
static bool crt_matches_key(mbedtls_x509_crt *c, const char *key_pem)
{
    if (!key_pem) return false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_key(&pk, (const unsigned char *)key_pem,
                                   strlen(key_pem) + 1, NULL, 0, rng, NULL) == 0 &&
              mbedtls_pk_check_pair(&c->pk, &pk, rng, NULL) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

/* The state of the stored certificate; P is locked. */
static pki_state_t judge(const char *call, time_t now, pki_info_t *info)
{
    if (!P.key) return PKI_NO_KEY;
    mbedtls_x509_crt c;
    if (!P.crt || crt_parse_first(&c, P.crt) < 0) {
        if (P.crt) mbedtls_x509_crt_free(&c);
        return PKI_NO_CERT;
    }
    char cn[40];
    crt_cn(&c, cn, sizeof cn);
    const int64_t nb = x509_epoch(&c.valid_from), na = x509_epoch(&c.valid_to);
    if (info) {
        snprintf(info->cn, sizeof info->cn, "%s", cn);
        mbedtls_x509_dn_gets(info->issuer, sizeof info->issuer, &c.issuer);
        info->not_before = nb;
        info->not_after  = na;
    }
    uint8_t fp[32];
    crt_fp(&c, fp);
    if (!P.match_known || memcmp(fp, P.match_fp, 32) != 0) {
        P.match_ok = crt_matches_key(&c, P.key);
        memcpy(P.match_fp, fp, 32);
        P.match_known = true;
    }
    const bool mine = strcmp(cn, call) == 0 && P.match_ok;
    mbedtls_x509_crt_free(&c);

    if (!mine) return PKI_WRONG_CALL;
    if (P.have_refused && memcmp(fp, P.refused, 32) == 0) return PKI_REFUSED;
    if (!svx_time_known(now)) return PKI_VALID;    /* no clock yet: assume so */
    if (now >= na) return PKI_EXPIRED;
    if (now < nb)  return PKI_NOT_YET_VALID;
    /* The reflector renews at two thirds of the lifetime; warn half-way from
     * there to the end, or a fortnight before it, whichever comes later. */
    const int64_t renew_at = nb + (na - nb) * 2 / 3;
    int64_t warn_at = na - 14 * DAY;
    if (warn_at < renew_at + (na - renew_at) / 2) warn_at = renew_at + (na - renew_at) / 2;
    if (now >= warn_at)  return PKI_EXPIRING;
    if (now >= renew_at) return PKI_RENEW_DUE;
    return PKI_VALID;
}

void svx_pki_info(const char *call, time_t now, pki_info_t *out)
{
    memset(out, 0, sizeof *out);
    LOCK();
    out->state     = judge(call, now, out);
    out->have_key  = P.key != NULL;
    out->have_csr  = P.csr != NULL;
    out->have_ca   = P.ca != NULL;
    out->pending   = P.pending;
    out->requested = P.requested;
    UNLOCK();
}

bool svx_pki_present(const char *call, time_t now)
{
    LOCK();
    pki_state_t s = judge(call, now, NULL);
    UNLOCK();
    /* Expired or refused: log in without it, so the reflector asks for a new
     * request from the same key. Not yet valid is presented anyway -- it is
     * this clock or the reflector's that is wrong, and only it can say. */
    return s == PKI_VALID || s == PKI_NOT_YET_VALID || s == PKI_RENEW_DUE ||
           s == PKI_EXPIRING;
}

bool svx_pki_have_key(void)
{
    LOCK();
    bool k = P.key != NULL;
    UNLOCK();
    return k;
}

char *svx_pki_csr_pem(void) { LOCK(); char *p = copy_of(P.csr); UNLOCK(); return p; }
char *svx_pki_key_pem(void) { LOCK(); char *p = copy_of(P.key); UNLOCK(); return p; }
char *svx_pki_crt_pem(void) { LOCK(); char *p = copy_of(P.crt); UNLOCK(); return p; }
char *svx_pki_ca_pem(void)  { LOCK(); char *p = copy_of(P.ca);  UNLOCK(); return p; }

/* ------------------------------------------------------------- the key */

esp_err_t svx_pki_make_key(void)
{
    if (svx_pki_have_key()) return ESP_OK;      /* never replaced */

    ESP_LOGI(TAG, "making an RSA-2048 key; this takes a while");
    const int64_t t0 = esp_log_timestamp();
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    char *pem = heap_caps_malloc(PEM_MAX, MALLOC_CAP_SPIRAM);
    esp_err_t err = ESP_FAIL;
    int r = pem ? mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA))
                : MBEDTLS_ERR_PK_ALLOC_FAILED;
    if (r == 0) r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), rng_yield, NULL, 2048, 65537);
    if (r == 0) r = mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem, PEM_MAX);
    if (r == 0) {
        LOCK();
        if (!P.key && save_str("key", pem) == ESP_OK) {
            P.key = dup_psram(pem, strlen(pem));
            P.match_known = false;
            err = P.key ? ESP_OK : ESP_ERR_NO_MEM;
        } else if (P.key) {
            err = ESP_OK;                        /* someone was quicker */
        }
        UNLOCK();
        ESP_LOGI(TAG, "key made in %lu ms", (unsigned long)(esp_log_timestamp() - t0));
    } else {
        ESP_LOGE(TAG, "key generation failed: -0x%04x", (unsigned)-r);
    }
    if (pem) {
        memset(pem, 0, PEM_MAX);                 /* it held a private key */
        free(pem);
    }
    mbedtls_pk_free(&pk);
    return err;
}

/* ---------------------------------------------------------- the request */

/* The extensions the CLI's request carries, as DER. mbedTLS marks its own
 * key-usage extension non-critical, so all four are set by hand. */
static const uint8_t EXT_BASIC[] = { 0x30, 0x00 };                   /* CA:FALSE */
/* digitalSignature, keyEncipherment, keyAgreement: bits 0, 2 and 4. */
static const uint8_t EXT_KU[]    = { 0x03, 0x02, 0x03, 0xA8 };
static const uint8_t EXT_EKU[]   = { 0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01,
                                     0x05, 0x05, 0x07, 0x03, 0x02 }; /* clientAuth */

esp_err_t svx_pki_make_csr(const char *call, const char *email)
{
    char want[sizeof P.csr_for];
    snprintf(want, sizeof want, "%s %s", call, email ? email : "");

    LOCK();
    if (P.csr && strcmp(P.csr_for, want) == 0) { UNLOCK(); return ESP_OK; }
    char *key = copy_of(P.key);
    UNLOCK();
    if (!key) return ESP_ERR_INVALID_STATE;

    mbedtls_pk_context pk;
    mbedtls_x509write_csr req;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&req);
    char *pem = heap_caps_malloc(PEM_MAX, MALLOC_CAP_SPIRAM);
    esp_err_t err = ESP_FAIL;

    char subject[48];
    snprintf(subject, sizeof subject, "CN=%s", call);
    const size_t elen = email ? strlen(email) : 0;
    uint8_t san[4 + 120];
    int r = pem ? 0 : MBEDTLS_ERR_X509_ALLOC_FAILED;
    if (r == 0) r = mbedtls_pk_parse_key(&pk, (const unsigned char *)key, strlen(key) + 1,
                                         NULL, 0, rng, NULL);
    if (r == 0) {
        mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&req, &pk);
        r = mbedtls_x509write_csr_set_subject_name(&req, subject);
    }
    if (r == 0) r = mbedtls_x509write_csr_set_extension(&req, MBEDTLS_OID_BASIC_CONSTRAINTS,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS), 1, EXT_BASIC, sizeof EXT_BASIC);
    if (r == 0) r = mbedtls_x509write_csr_set_extension(&req, MBEDTLS_OID_KEY_USAGE,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_KEY_USAGE), 1, EXT_KU, sizeof EXT_KU);
    if (r == 0) r = mbedtls_x509write_csr_set_extension(&req, MBEDTLS_OID_EXTENDED_KEY_USAGE,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, EXT_EKU, sizeof EXT_EKU);
    /* subjectAltName email:<address> -- the address only ever goes here, never
     * into the subject. Short-form DER lengths, so at most 120 characters. */
    if (r == 0 && elen > 0 && elen <= 120) {
        san[0] = 0x30; san[1] = (uint8_t)(elen + 2);
        san[2] = 0x81; san[3] = (uint8_t)elen;        /* [1] rfc822Name */
        memcpy(san + 4, email, elen);
        r = mbedtls_x509write_csr_set_extension(&req, MBEDTLS_OID_SUBJECT_ALT_NAME,
                MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, elen + 4);
    }
    if (r == 0) r = mbedtls_x509write_csr_pem(&req, (unsigned char *)pem, PEM_MAX, rng, NULL);
    if (r == 0) {
        LOCK();
        if (save_str("csr", pem) == ESP_OK && nvs_save("csrfor", want, strlen(want)) == ESP_OK) {
            free(P.csr);
            P.csr = dup_psram(pem, strlen(pem));
            snprintf(P.csr_for, sizeof P.csr_for, "%s", want);
            err = P.csr ? ESP_OK : ESP_ERR_NO_MEM;
        }
        UNLOCK();
        ESP_LOGI(TAG, "certificate request made for %s <%s>", call, email ? email : "");
    } else {
        ESP_LOGE(TAG, "certificate request failed: -0x%04x", (unsigned)-r);
    }
    mbedtls_x509write_csr_free(&req);
    mbedtls_pk_free(&pk);
    memset(key, 0, strlen(key));
    free(key);
    free(pem);
    return err;
}

/* ------------------------------------------------ what the reflector sends */

void svx_pki_store_ca(const char *pem)
{
    if (!pem || !pem[0]) return;
    size_t n = strlen(pem);
    if (n > CA_MAX) { ESP_LOGW(TAG, "CA bundle of %u bytes not kept", (unsigned)n); return; }
    LOCK();
    /* In memory only: it comes again with every connection (load()). */
    if (!P.ca || strcmp(P.ca, pem) != 0) {
        free(P.ca);
        P.ca = dup_psram(pem, n);
        ESP_LOGI(TAG, "CA bundle kept (%u bytes, in memory)", (unsigned)n);
    }
    UNLOCK();
}

pki_push_t svx_pki_store_cert(const uint8_t *body, size_t len, const char *call, time_t now)
{
    const size_t cap = len + len / 16 + 16;
    char *pem = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!pem) return PKI_PUSH_FAILED;
    if (proto_parse_pem_blob(body, len, pem, cap) != 0) {
        free(pem);
        return PKI_PUSH_EMPTY;                  /* nothing signed yet */
    }

    pki_push_t res = PKI_PUSH_REJECTED;
    mbedtls_x509_crt c;
    LOCK();
    if (crt_parse_first(&c, pem) < 0) {
        ESP_LOGE(TAG, "the reflector sent a certificate that does not parse");
        goto out;
    }
    char cn[40];
    crt_cn(&c, cn, sizeof cn);
    const int64_t nb = x509_epoch(&c.valid_from), na = x509_epoch(&c.valid_to);
    uint8_t fp[32];
    crt_fp(&c, fp);
    if (strcmp(cn, call) != 0) {
        ESP_LOGE(TAG, "the reflector sent a certificate for \"%s\", not %s", cn, call);
        goto out;
    }
    if (!crt_matches_key(&c, P.key)) {
        ESP_LOGE(TAG, "the reflector sent a certificate for another key");
        goto out;
    }
    if (na <= nb) {
        ESP_LOGE(TAG, "the reflector sent a certificate that is never valid");
        goto out;
    }

    /* Compare with what is stored: the same one, or an older one, is not an
     * improvement -- unless the stored one was refused. */
    mbedtls_x509_crt old;
    if (P.crt && crt_parse_first(&old, P.crt) >= 0) {
        uint8_t ofp[32];
        crt_fp(&old, ofp);
        const int64_t ona = x509_epoch(&old.valid_to);
        const bool old_refused = P.have_refused && memcmp(ofp, P.refused, 32) == 0;
        mbedtls_x509_crt_free(&old);
        if (memcmp(fp, ofp, 32) == 0) { res = PKI_PUSH_SAME; goto out; }
        if (!old_refused && na < ona) {
            ESP_LOGE(TAG, "the reflector sent a certificate older than the stored one");
            goto out;
        }
    } else if (P.crt) {
        mbedtls_x509_crt_free(&old);
    }
    if (svx_time_known(now) && now >= na) {
        ESP_LOGE(TAG, "the reflector sent a certificate that has already expired");
        goto out;
    }

    if (save_str("crt", pem) != ESP_OK) { res = PKI_PUSH_FAILED; goto out; }
    free(P.crt);
    P.crt = pem;
    pem = NULL;
    P.have_refused = false;
    nvs_save("refused", NULL, 0);
    P.pending = false;
    nvs_save("pending", NULL, 0);
    res = PKI_PUSH_STORED;
    ESP_LOGI(TAG, "certificate stored for %s", cn);

out:
    mbedtls_x509_crt_free(&c);
    UNLOCK();
    free(pem);
    return res;
}

void svx_pki_refused(void)
{
    mbedtls_x509_crt c;
    LOCK();
    if (P.crt && crt_parse_first(&c, P.crt) >= 0) {
        crt_fp(&c, P.refused);
        P.have_refused = true;
        nvs_save("refused", P.refused, sizeof P.refused);
        ESP_LOGW(TAG, "the reflector refused the certificate; the next login asks for a new one");
    }
    mbedtls_x509_crt_free(&c);
    UNLOCK();
}

static void pending_job(void *p)
{
    (void)p;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (P.pending) {
        nvs_set_u8(h, "pending", 1);
        nvs_set_i64(h, "reqtime", P.requested);
    } else {
        nvs_erase_key(h, "pending");
    }
    nvs_commit(h);
    nvs_close(h);
}

void svx_pki_set_pending(bool on, time_t now)
{
    LOCK();
    if (P.pending != on) {
        P.pending = on;
        if (on) P.requested = svx_time_known(now) ? (int64_t)now : 0;
        svx_flash_safe(pending_job, NULL);
    }
    UNLOCK();
}

bool svx_pki_pending(void)
{
    LOCK();
    bool p = P.pending;
    UNLOCK();
    return p;
}

static void forget_job(void *p)
{
    (void)p;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    static const char *k[] = { "key", "csr", "csrfor", "crt", "refused", "pending", "reqtime" };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) nvs_erase_key(h, k[i]);
    nvs_commit(h);
    nvs_close(h);
}

void svx_pki_forget(void)
{
    LOCK();
    free(P.key); free(P.csr); free(P.crt);
    P.key = P.csr = P.crt = NULL;
    P.csr_for[0] = 0;
    P.match_known = false;
    P.have_refused = false;
    P.pending = false;
    svx_flash_safe(forget_job, NULL);
    UNLOCK();
    ESP_LOGW(TAG, "key, request and certificate deleted");
}
