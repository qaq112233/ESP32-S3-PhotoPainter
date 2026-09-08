set(USER_APP_BSP_SOURCES
  "mode_src/Network_mode.cpp"
  "mode_src/Basic_mode.cpp"
  "mode_src/basic_timer.cpp"
  "mode_src/Mode_Selection.cpp"
  "user_app.cpp")
if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
  list(APPEND USER_APP_BSP_SOURCES "mode_src/xiaozhi_mode.cpp")
endif()

set(USER_APP_BSP_PRIV_REQUIRES
  driver
  nvs_flash
  esp_wifi
  main
  app_bsp
  pmicpower
  port_bsp
  json
  photopull)

if(CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI)
  list(APPEND USER_APP_BSP_PRIV_REQUIRES codec_board)
endif()

idf_component_register(
  SRCS ${USER_APP_BSP_SOURCES}
  PRIV_REQUIRES
  ${USER_APP_BSP_PRIV_REQUIRES}
  INCLUDE_DIRS "./")

# The vendor's board, storage, decoding and application components refer to
# one another. Rescan that archive group without forcing unused code into Lite.
set_property(TARGET ${COMPONENT_LIB} PROPERTY LINK_INTERFACE_MULTIPLICITY 4)
