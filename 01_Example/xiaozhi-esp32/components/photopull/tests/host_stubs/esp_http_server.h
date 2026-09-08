#pragma once
#include <cstddef>
#include "esp_err.h"
struct httpd_req_t { const char* uri; size_t content_len; };
using httpd_handle_t = void*;
using httpd_method_t = int;
constexpr int HTTP_GET = 0, HTTP_POST = 1, HTTPD_RESP_USE_STRLEN = -1, HTTPD_SOCK_ERR_TIMEOUT = -2;
constexpr int HTTPD_404_NOT_FOUND = 404, HTTPD_500_INTERNAL_SERVER_ERROR = 500,
    HTTPD_400_BAD_REQUEST = 400, HTTPD_413_CONTENT_TOO_LARGE = 413;
inline bool httpd_uri_match_wildcard(const char*, const char*, size_t) { return false; }
struct httpd_config_t { bool (*uri_match_fn)(const char*, const char*, size_t); int recv_wait_timeout, send_wait_timeout, max_uri_handlers; };
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
struct httpd_uri_t { const char* uri; int method; int (*handler)(httpd_req_t*); };
inline int httpd_resp_set_status(httpd_req_t*, const char*) { return 0; }
inline int httpd_resp_set_type(httpd_req_t*, const char*) { return 0; }
inline int httpd_resp_send(httpd_req_t*, const char*, int) { return 0; }
inline int httpd_resp_send_err(httpd_req_t*, int, const char*) { return 0; }
inline int httpd_resp_send_408(httpd_req_t*) { return 0; }
inline int httpd_resp_send_chunk(httpd_req_t*, const char*, size_t) { return 0; }
inline int httpd_req_recv(httpd_req_t*, char*, size_t) { return -1; }
inline int httpd_start(void**, httpd_config_t*) { return 0; }
inline int httpd_stop(void*) { return 0; }
inline int httpd_register_uri_handler(void*, httpd_uri_t*) { return 0; }
