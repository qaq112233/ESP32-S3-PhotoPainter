#pragma once
#include <cstddef>
#include <cstdint>
#include "mbedtls/x509_crt.h"

namespace photopull {
// Require an iPAddress SAN for numeric hosts. A numeric DNS SAN or CN alone
// must not satisfy the PhotoPull protocol even if the TLS library accepts it.
bool CertificateHasIpSan(const mbedtls_x509_crt* certificate,
                         const uint8_t* address, size_t address_size);
}
