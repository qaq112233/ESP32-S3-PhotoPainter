# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "2.0.1")
# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# The legacy codec_board component is a local component, so ESP-IDF discovers
# it even when no Lite source requires it.  Keep it out of the Lite graph at
# component discovery time; the Full build includes it when the persisted
# Kconfig value enables the Xiaozhi application.  SDKCONFIG is read here
# because CONFIG_* values are not available until project.cmake has loaded
# Kconfig.
set(_photopainter_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
if(DEFINED SDKCONFIG AND NOT IS_ABSOLUTE "${SDKCONFIG}")
    get_filename_component(_photopainter_sdkconfig "${SDKCONFIG}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
elseif(DEFINED SDKCONFIG)
    set(_photopainter_sdkconfig "${SDKCONFIG}")
endif()
set(_photopainter_xiaozhi_enabled OFF)
if(EXISTS "${_photopainter_sdkconfig}")
    file(STRINGS "${_photopainter_sdkconfig}" _photopainter_xiaozhi_line
         REGEX "^CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI=y$")
    if(_photopainter_xiaozhi_line)
        set(_photopainter_xiaozhi_enabled ON)
    endif()
endif()
if(NOT _photopainter_xiaozhi_enabled)
    list(APPEND EXCLUDE_COMPONENTS codec_board)
    set(COMPONENTS main)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)
