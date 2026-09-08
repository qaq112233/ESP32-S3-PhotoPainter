#pragma once
#include <cstddef>
struct mbedtls_x509_crt {};
inline void mbedtls_x509_crt_init(mbedtls_x509_crt*) {}
inline void mbedtls_x509_crt_free(mbedtls_x509_crt*) {}
inline int mbedtls_x509_crt_parse(mbedtls_x509_crt*, const unsigned char*, size_t) { return 0; }
