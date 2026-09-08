idf_component_register(
    SRCS "core.cpp" "store.cpp" "bmp_validator.cpp" "carousel.cpp"
         "fatfs_store.cpp" "https_io.cpp" "tls_identity.cpp" "service.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client tcp_transport mbedtls esp_timer fatfs lwip user_app_bsp port_bsp pmicpower
)
