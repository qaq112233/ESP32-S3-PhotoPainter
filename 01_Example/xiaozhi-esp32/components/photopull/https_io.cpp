#include "https_io.h"
#include "tls_identity.h"
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <new>
#include <cerrno>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "mbedtls/ssl.h"
#include "lwip/inet.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"

namespace photopull {
namespace {
int64_t now_ms() { return esp_timer_get_time() / 1000; }
struct VerificationPolicy {
    mbedtls_x509_crt private_ca;
    bool use_private_ca = false, attached = false;
    uint8_t ip[16] = {};
    size_t ip_size = 0;
    int (*bundle_verify)(void*, mbedtls_x509_crt*, int, uint32_t*) = nullptr;
    void* bundle_context = nullptr;
    VerificationPolicy() { mbedtls_x509_crt_init(&private_ca); }
    ~VerificationPolicy() { mbedtls_x509_crt_free(&private_ca); }
};
// Only photopull_https invokes this attach callback, synchronously during its
// single in-flight handshake. Other Full-mode TLS users use their own callback.
VerificationPolicy* attaching_policy = nullptr;
struct DnsLookup {
    // Static lifetime permits the lwIP callback to finish after a timed-out
    // request. At most one lookup can remain outstanding for this fixed origin.
    std::atomic<bool> in_flight{false}, done{false};
    char host[254] = {}, address[48] = {};
    static void Found(const char*, const ip_addr_t* address, void* context) {
        auto* self = static_cast<DnsLookup*>(context);
        self->address[0] = 0;
        if (address) ipaddr_ntoa_r(address, self->address, sizeof(self->address));
        self->done = true;
        self->in_flight = false;
    }
    static void Start(void* context) {
        auto* self = static_cast<DnsLookup*>(context);
        ip_addr_t address;
        err_t result = dns_gethostbyname(self->host, &address, Found, self);
        if (result != ERR_INPROGRESS) Found(self->host, result == ERR_OK ? &address : nullptr, self);
    }
};
DnsLookup dns_lookup;
struct DeadlineTransport {
    esp_transport_handle_t ssl = nullptr;
    int64_t deadline;
    const std::atomic<bool>* stopping;
    const std::atomic<bool>* connected;
    const std::atomic<uint32_t>* accepted;
    uint32_t serial;
    int Budget(int maximum) const {
        int64_t remaining = deadline - now_ms();
        if (*stopping || !*connected || *accepted != serial || remaining <= 0) { errno = ETIMEDOUT; return -1; }
        return static_cast<int>(std::min<int64_t>(maximum, remaining));
    }
    static DeadlineTransport* Get(esp_transport_handle_t t) { return static_cast<DeadlineTransport*>(esp_transport_get_context_data(t)); }
    static int Connect(esp_transport_handle_t t, const char* host, int port, int timeout) {
        auto* self = Get(t); int budget = self->Budget(std::min(timeout, 10000));
        if (budget <= 0 || strlen(host) >= sizeof(dns_lookup.host)) return -1;
        int64_t connect_deadline = now_ms() + budget;
        char numeric[48];
        ip_addr_t literal;
        if (ipaddr_aton(host, &literal)) {
            if (!ipaddr_ntoa_r(&literal, numeric, sizeof(numeric))) return -1;
        } else {
            if (!dns_lookup.in_flight) {
                dns_lookup.done = false;
                strcpy(dns_lookup.host, host);
                dns_lookup.in_flight = true;
                if (tcpip_callback(DnsLookup::Start, &dns_lookup) != ERR_OK) { dns_lookup.in_flight = false; return -1; }
            }
            while (!dns_lookup.done && self->Budget(10000) > 0 && now_ms() < connect_deadline) vTaskDelay(pdMS_TO_TICKS(10));
            if (!dns_lookup.done || !dns_lookup.address[0]) return -1;
            strcpy(numeric, dns_lookup.address);
        }
        budget = std::min(self->Budget(10000), static_cast<int>(connect_deadline - now_ms()));
        if (budget <= 0) { errno = ETIMEDOUT; return -1; }
        // The socket uses the resolved address; TLS still checks and sends SNI
        // for the configured host. DNS cannot consume an unbounded connect call.
        esp_transport_ssl_set_common_name(self->ssl, host);
        return esp_transport_connect(self->ssl, numeric, port, budget);
    }
    static int Read(esp_transport_handle_t t, char* buffer, int length, int timeout) {
        auto* self = Get(t); int budget = self->Budget(std::min(timeout, 5000));
        return budget <= 0 ? -1 : esp_transport_read(self->ssl, buffer, length, budget);
    }
    static int Write(esp_transport_handle_t t, const char* buffer, int length, int timeout) {
        auto* self = Get(t); int budget = self->Budget(std::min(timeout, 5000));
        return budget <= 0 ? -1 : esp_transport_write(self->ssl, buffer, length, budget);
    }
    static int Close(esp_transport_handle_t t) { return esp_transport_close(Get(t)->ssl); }
    static int PollRead(esp_transport_handle_t t, int timeout) {
        auto* self = Get(t); int budget = self->Budget(std::min(timeout, 5000));
        return budget <= 0 ? -1 : esp_transport_poll_read(self->ssl, budget);
    }
    static int PollWrite(esp_transport_handle_t t, int timeout) {
        auto* self = Get(t); int budget = self->Budget(std::min(timeout, 5000));
        return budget <= 0 ? -1 : esp_transport_poll_write(self->ssl, budget);
    }
    static int Destroy(esp_transport_handle_t) { return 0; } // The worker owns the underlying TLS transport.
    static esp_transport_handle_t Parent(esp_transport_handle_t t) { return Get(t)->ssl; }
};
int verify_identity(void* context, mbedtls_x509_crt* cert, int depth, uint32_t* flags) {
    auto* policy = static_cast<VerificationPolicy*>(context);
    int ret = 0;
    if (policy->bundle_verify) ret = policy->bundle_verify(policy->bundle_context, cert, depth, flags);
    if (!policy->attached || (depth == 0 && policy->ip_size && !CertificateHasIpSan(cert, policy->ip, policy->ip_size)))
        *flags |= MBEDTLS_X509_BADCERT_CN_MISMATCH;
    return ret;
}
esp_err_t attach_verification(void* configuration) {
    auto* conf = static_cast<mbedtls_ssl_config*>(configuration);
    auto* policy = attaching_policy;
    if (!policy) return ESP_FAIL;
    esp_err_t result = ESP_OK;
    if (policy->use_private_ca) {
        mbedtls_ssl_conf_ca_chain(conf, &policy->private_ca, nullptr);
    } else {
        result = esp_crt_bundle_attach(conf);
        // Preserve the pinned IDF 5.5.1 bundle verifier; never replace chain
        // verification with identity verification alone.
        policy->bundle_verify = conf->MBEDTLS_PRIVATE(f_vrfy);
        policy->bundle_context = conf->MBEDTLS_PRIVATE(p_vrfy);
    }
    policy->attached = result == ESP_OK;
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(conf, verify_identity, policy);
    return result;
}
struct Headers {
    char etag[257] = {};
    bool invalid = false;
};
esp_err_t on_http(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    auto* headers = static_cast<Headers*>(event->user_data);
    if (!event->header_key || !event->header_value) return ESP_OK;
    if (strcasecmp(event->header_key, "ETag") == 0) {
        size_t n = strlen(event->header_value);
        if (n > 256) { headers->invalid = true; return ESP_FAIL; }
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = event->header_value[i];
            if (c < 0x20 || c > 0x7e) { headers->invalid = true; return ESP_FAIL; }
        }
        memcpy(headers->etag, event->header_value, n + 1);
    }
    if (strcasecmp(event->header_key, "Content-Encoding") == 0 &&
        strcasecmp(event->header_value, "identity") != 0) headers->invalid = true;
    return ESP_OK;
}
}

bool HttpsIo::start(const std::string& origin, const std::string& token, const std::string& ca) {
    origin_ = origin;
    token_ = token;
    ca_ = ca;
    requests_ = xQueueCreate(1, sizeof(HttpRequest));
    events_ = xQueueCreate(2, sizeof(HttpEvent));
    if (!requests_ || !events_) {
        if (requests_) vQueueDelete(requests_);
        if (events_) vQueueDelete(events_);
        requests_ = events_ = nullptr;
        return false;
    }
    stopped_ = false;
    if (xTaskCreate(task_entry, "photopull_https", 8192, this, 2, &task_) != pdPASS) {
        stopped_ = true;
        vQueueDelete(requests_);
        vQueueDelete(events_);
        requests_ = events_ = nullptr;
        task_ = nullptr;
        return false;
    }
    return true;
}
bool HttpsIo::submit(const HttpRequest& request) {
    if (stopping_ || !requests_) return false;
    accepted_ = request.serial;
    read_credits_ = 2;
    return xQueueSend(requests_, &request, 0) == pdTRUE;
}
bool HttpsIo::receive(HttpEvent& event) {
    if (!events_ || xQueueReceive(events_, &event, 0) != pdTRUE) return false;
    if (event.kind == HttpEventKind::Data && event.serial == accepted_) read_credits_.fetch_add(1);
    return true;
}
void HttpsIo::cancel() { accepted_.fetch_add(1); }
void HttpsIo::stop() { stopping_ = true; cancel(); }
bool HttpsIo::alive(uint32_t serial, int64_t deadline) const {
    return !stopping_ && connected_ && accepted_ == serial && now_ms() < deadline;
}
bool HttpsIo::emit(const HttpEvent& event, int64_t deadline) {
    while (alive(event.serial, deadline)) {
        if (xQueueSend(events_, &event, pdMS_TO_TICKS(100)) == pdTRUE) return true;
    }
    return false;
}
bool HttpsIo::time_ready() {
    // SNTP is independent of display scheduling, which only uses esp_timer.
    if (!sntp_started_) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, const_cast<char*>("pool.ntp.org"));
        esp_sntp_set_sync_interval(6 * 60 * 60 * 1000);
        esp_sntp_init();
        sntp_started_ = true;
    }
    if (time_valid_) return true; // lwIP continues background synchronization every six hours.
    int64_t deadline = now_ms() + 10000;
    while (!stopping_ && connected_ && now_ms() < deadline) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            time_valid_ = true;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}
void HttpsIo::task_entry(void* ctx) { static_cast<HttpsIo*>(ctx)->run(); }
void HttpsIo::run() {
    // Keep the 4 KiB event off the task stack during TLS.
    while (!stopping_) {
        HttpRequest request;
        if (xQueueReceive(requests_, &request, pdMS_TO_TICKS(100)) == pdTRUE) perform(request);
    }
    if (sntp_started_) esp_sntp_stop();
    stopped_ = true;
    vTaskDelete(nullptr);
}
void HttpsIo::perform(const HttpRequest& request) {
    auto* event = new (std::nothrow) HttpEvent;
    if (!event) return; // manager's operation deadline detects an allocation failure.
    event->serial = request.serial;
    int64_t deadline = now_ms() + request.budget_ms;
    esp_http_client_handle_t client = nullptr;
    VerificationPolicy verification;
    DeadlineTransport transport{nullptr, deadline, &stopping_, &connected_, &accepted_, request.serial};
    esp_transport_handle_t bounded_transport = nullptr;
    bool success = false;
    do {
        if (!alive(request.serial, deadline) || !time_ready()) break;
        std::string url = origin_ + request.path;
        if (url.size() > 512) break;
        std::string host = origin_.substr(8);
        if (!host.empty() && host[0] == '[') host = host.substr(1, host.find(']') - 1);
        else host = host.substr(0, host.find(':'));
        if (inet_pton(AF_INET, host.c_str(), verification.ip) == 1) verification.ip_size = 4;
        else if (inet_pton(AF_INET6, host.c_str(), verification.ip) == 1) verification.ip_size = 16;
        if (!ca_.empty()) {
            if (mbedtls_x509_crt_parse(&verification.private_ca, reinterpret_cast<const unsigned char*>(ca_.c_str()), ca_.size() + 1) != 0) break;
            verification.use_private_ca = true;
        }
        attaching_policy = &verification;
        // Enforce the absolute budget at every underlying read, including the
        // header parser's internal loop (a peer may trickle partial headers).
        transport.ssl = esp_transport_ssl_init();
        bounded_transport = esp_transport_init();
        if (!transport.ssl || !bounded_transport) break;
        esp_transport_ssl_crt_bundle_attach(transport.ssl, attach_verification);
        esp_transport_set_default_port(bounded_transport, 443);
        esp_transport_set_context_data(bounded_transport, &transport);
        esp_transport_set_func(bounded_transport, DeadlineTransport::Connect, DeadlineTransport::Read,
                               DeadlineTransport::Write, DeadlineTransport::Close,
                               DeadlineTransport::PollRead, DeadlineTransport::PollWrite, DeadlineTransport::Destroy);
        esp_transport_set_parent_transport_func(bounded_transport, DeadlineTransport::Parent);
        Headers headers;
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.timeout_ms = 10000;
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.skip_cert_common_name_check = false;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.buffer_size = 4096;
        config.buffer_size_tx = 4096;
        config.event_handler = on_http;
        config.user_data = &headers;
        config.crt_bundle_attach = attach_verification;
        config.transport = bounded_transport;
        client = esp_http_client_init(&config);
        if (!client) break;
        if (esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK) break;
        if (!token_.empty()) {
            std::string authorization = "Bearer " + token_;
            if (esp_http_client_set_header(client, "Authorization", authorization.c_str()) != ESP_OK) break;
        }
        if (request.etag[0] && esp_http_client_set_header(client, "If-None-Match", request.etag) != ESP_OK) break;
        if (esp_http_client_open(client, 0) != ESP_OK || !alive(request.serial, deadline)) break;
        esp_http_client_set_timeout_ms(client, 5000);
        int64_t length = esp_http_client_fetch_headers(client);
        if (length < 0 || headers.invalid || !alive(request.serial, deadline)) break;
        event->kind = HttpEventKind::Headers;
        event->status = esp_http_client_get_status_code(client);
        event->content_length = esp_http_client_is_chunked_response(client) ? -1 : length;
        memcpy(event->etag, headers.etag, sizeof(event->etag));
        if (event->status != 200 && event->status != 304) break;
        if (length > request.limit) break;
        if (!emit(*event, deadline)) break;
        if (event->status == 304) { success = true; break; }
        size_t received = 0;
        bool complete = false;
        while (alive(request.serial, deadline)) {
            while (read_credits_ == 0 && alive(request.serial, deadline)) vTaskDelay(pdMS_TO_TICKS(20));
            if (!alive(request.serial, deadline)) break;
            read_credits_.fetch_sub(1);
            esp_http_client_set_timeout_ms(client, std::max(1, static_cast<int>(std::min<int64_t>(5000, deadline - now_ms()))));
            int n = esp_http_client_read(client, reinterpret_cast<char*>(event->data), sizeof(event->data));
            if (n < 0) break;
            if (n == 0) { complete = esp_http_client_is_complete_data_received(client); break; }
            if (received + static_cast<size_t>(n) > request.limit) break;
            received += n;
            event->size = n;
            event->kind = HttpEventKind::Data;
            if (!emit(*event, deadline)) break;
        }
        success = complete && !headers.invalid;
    } while (false);
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    if (bounded_transport) esp_transport_destroy(bounded_transport);
    if (transport.ssl) esp_transport_destroy(transport.ssl);
    attaching_policy = nullptr;
    event->size = 0;
    event->kind = success ? HttpEventKind::Complete : HttpEventKind::Failed;
    // Failure reporting has its own short queue budget; manager also owns a deadline.
    emit(*event, now_ms() + 1000);
    delete event;
}
}
