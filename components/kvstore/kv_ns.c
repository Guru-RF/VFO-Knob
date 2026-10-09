/* The namespaces kvstore keeps: the same table in every firmware, so each
 * one moves them all. A namespace not listed stays in NVS, read there
 * directly -- `btlink` (the Bluetooth records), with WiFi. */
#include "kv_priv.h"

#define L(...) (const char *const[]){ __VA_ARGS__, NULL }

const kv_ns_def_t KV_NS[] = {
    /* WiFi, the boot counter and provisioning's mark stay in NVS. */
    { .name = "vfo", .flags = KVF_FROZEN, .nvs_keep = L("ssid", "pass", "wifis", "boots", "sdwipe") },
    /* SVXConnect: the talkgroups' names are a cache now, and the CA bundle
     * is fetched before every TLS start, so neither is kept in NVS. */
    { .name = "svx", .flags = KVF_FROZEN, .drop = L("tghost", "tgjson") },
    { .name = "svxpki", .flags = KVF_FROZEN, .identity = L("key"),
      .group = L("key", "csr", "csrfor", "crt", "refused", "pending", "reqtime"), .drop = L("ca") },
    { .name = "svxtg", .flags = KVF_CACHE },
    { .name = "sl", .flags = KVF_FROZEN, .identity = L("pins") },
    { .name = "flex", .flags = KVF_FROZEN, .identity = L("uuid") },
    { .name = "phone", .flags = KVF_FROZEN },
    { .name = "icom", .flags = KVF_FROZEN },
    { .name = "xiegu", .flags = KVF_FROZEN },
#if VFO_KV_TEST
    { .name = "kvtest" },                 /* kv_test.c's writer: a test build's only */
#endif
};

const int KV_NS_N = sizeof KV_NS / sizeof KV_NS[0];
