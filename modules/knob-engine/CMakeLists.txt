if(CONFIG_KNOB_ENGINE)
  zephyr_library()
  zephyr_library_sources(src/knob_engine.c)

  # ZMK app headers (<zmk/keymap.h>, <zmk/activity.h>, events).
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

  # badjeff/zmk-drv2605-driver attribute API header, when present.
  if(DEFINED ZEPHYR_ZMK_DRV2605_DRIVER_MODULE_DIR)
    zephyr_library_include_directories(${ZEPHYR_ZMK_DRV2605_DRIVER_MODULE_DIR}/include)
  endif()
endif()
