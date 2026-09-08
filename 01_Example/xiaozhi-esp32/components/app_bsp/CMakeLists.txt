if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
    set(APP_BSP_SOURCES
        "ai_app.cpp"
        "imgdecode_app.cpp"
        "client_app.c"
        "server_app.cpp"
        "./list_src/list_iterator.c"
        "./list_src/list_node.c"
        "./list_src/list.c"
        "weather_app.cpp"
        "./jpg_src/image_io.c"
        "./jpg_src/test_decoder.c")
    set(APP_BSP_EMBED_FILES
        "./certs/ark_vol.pem"
        "./certs/volces_chain.pem")
    set(APP_BSP_PRIV_REQUIRES
        espressif__esp_new_jpeg
        esp_http_client
        espressif__mdns
        port_bsp
        esp_wifi
        esp_http_server
        json
        photopull)
else()
    # Lite retains local image decoding, list support and the maintenance web
    # server. AI generation, weather and their certificate payloads are not
    # part of the image/network product.
    set(APP_BSP_SOURCES
        "imgdecode_app.cpp"
        "server_app.cpp"
        "./list_src/list_iterator.c"
        "./list_src/list_node.c"
        "./list_src/list.c"
        "./jpg_src/test_decoder.c")
    set(APP_BSP_PRIV_REQUIRES
        espressif__esp_new_jpeg
        espressif__mdns
        port_bsp
        esp_wifi
        esp_http_server
        photopull)
endif()

idf_component_register(
    SRCS ${APP_BSP_SOURCES}
    PRIV_REQUIRES
    user_app_bsp
    ${APP_BSP_PRIV_REQUIRES}
    REQUIRES
    espressif__libpng
    INCLUDE_DIRS
    "./"
    "./jpg_src"
    "./json_inc"
    "./list_src"
    EMBED_TXTFILES ${APP_BSP_EMBED_FILES})
