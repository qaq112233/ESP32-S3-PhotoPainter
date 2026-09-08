#include "tls_identity.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <arpa/inet.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    uint8_t v4[4], v6[16];
    assert(inet_pton(AF_INET, "127.0.0.1", v4) == 1);
    assert(inet_pton(AF_INET6, "2001:db8::1", v6) == 1);
    for (const char* name : {"ip", "wrong", "cn", "dns", "v6"}) {
        mbedtls_x509_crt certificate;
        mbedtls_x509_crt_init(&certificate);
        auto path = std::string(argv[1]) + "/" + name + ".pem";
        assert(mbedtls_x509_crt_parse_file(&certificate, path.c_str()) == 0);
        bool ipv4 = photopull::CertificateHasIpSan(&certificate, v4, sizeof(v4));
        bool ipv6 = photopull::CertificateHasIpSan(&certificate, v6, sizeof(v6));
        assert(ipv4 == (std::string(name) == "ip"));
        assert(ipv6 == (std::string(name) == "v6"));
        mbedtls_x509_crt_free(&certificate);
    }
    assert(!photopull::CertificateHasIpSan(nullptr, v4, sizeof(v4)));
    puts("real certificate IP SAN cases passed");
}
