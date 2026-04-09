add_library(user_layers STATIC)

target_sources(user_layers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/Src/drv_keys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/Src/drv_oled_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/Src/drv_uart_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Src/comp_oled.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/app_main.c
)

target_include_directories(user_layers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Inc
)

target_link_libraries(user_layers PUBLIC
    stm32cubemx
)
