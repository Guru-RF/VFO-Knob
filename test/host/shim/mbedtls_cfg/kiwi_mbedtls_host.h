/* mbedTLS on the PC as the knob's is built (sdkconfig: no
 * CONFIG_MBEDTLS_SSL_PROTO_TLS1_3): TLS 1.2 alone, so the test servers --
 * Python's, OpenSSL behind it -- shake hands with kiwi_tls.c as a front
 * would with the knob. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_SSL_RECORD_SIZE_LIMIT
/* ...and with no calendar for its certificates (sdkconfig: no
 * CONFIG_MBEDTLS_HAVE_TIME_DATE): their dates are not checked, on the knob
 * nor here. */
#undef MBEDTLS_HAVE_TIME_DATE
