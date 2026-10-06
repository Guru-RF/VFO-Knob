#ifndef SHIM_ESP_CRT_BUNDLE_H
#define SHIM_ESP_CRT_BUNDLE_H
#include "esp_err.h"
/* The certificate bundle, on the PC: the test's own CA alone -- the PEM file
 * KIWI_TEST_CA names, made for the run -- or none at all, when every
 * certificate is refused (shim_tls.c). */
esp_err_t esp_crt_bundle_attach(void *conf);
#endif
