set(PORT_BSP_SOURCES
    "i2c_bsp.cpp"
    "display_bsp.cpp"
    "sdcard_bsp.cpp"
    "./src/multi_button/multi_button.c"
    "button_bsp.c"
    "led_bsp.c"
    "./i2c_equipment.cpp"
    "./src/fonts/font24.c"
    "traverse_nvs.cpp")

set(PORT_BSP_EMBED_FILES)
if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
    list(APPEND PORT_BSP_SOURCES "./src/fonts/font14CN.c" "./src/fonts/font18CN.c" "./src/fonts/font22CN.c")
    list(APPEND PORT_BSP_SOURCES "./codec_bsp.cpp")
    list(APPEND PORT_BSP_EMBED_FILES
        "pcm_src/mode.pcm"
        "pcm_src/mode_1.pcm"
        "pcm_src/mode_2.pcm"
        "pcm_src/mode_3.pcm")
endif()

set(PORT_BSP_PRIV_REQUIRES
    nvs_flash
    esp_psram
    esp_timer)
set(PORT_BSP_REQUIRES)
if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
    list(APPEND PORT_BSP_REQUIRES codec_board)
endif()

idf_component_register(
    SRCS ${PORT_BSP_SOURCES}
    PRIV_REQUIRES
    ${PORT_BSP_PRIV_REQUIRES}
    REQUIRES
    ${PORT_BSP_REQUIRES}
    esp_driver_sdmmc
    fatfs
    app_bsp
    REQUIRES
    driver
    INCLUDE_DIRS 
    "./" 
    "./src/multi_button"
    "./src/fonts"
    EMBED_FILES ${PORT_BSP_EMBED_FILES})
if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
    idf_component_optional_requires(PUBLIC codec_board)
endif()
set_source_files_properties(
    sdcard_bsp.cpp
    PROPERTIES
    COMPILE_OPTIONS "-Wno-format-truncation"
)
