#include "tls_identity.h"
#include <cstring>
#include "mbedtls/asn1.h"

namespace photopull {
bool CertificateHasIpSan(const mbedtls_x509_crt* certificate,
                         const uint8_t* address, size_t address_size) {
    if (!certificate || !address || (address_size != 4 && address_size != 16)) return false;
    for (const mbedtls_x509_sequence* san = &certificate->subject_alt_names; san; san = san->next) {
        if ((san->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK) == MBEDTLS_X509_SAN_IP_ADDRESS &&
            san->buf.len == address_size && san->buf.p && memcmp(san->buf.p, address, address_size) == 0) return true;
    }
    return false;
}
}
