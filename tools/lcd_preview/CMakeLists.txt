cmake_minimum_required(VERSION 3.16)
project(psk_lcd_preview C)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LVGL "${REPO}/managed_components/lvgl__lvgl")
file(GLOB_RECURSE LVGL_SOURCES "${LVGL}/src/*.c")
add_executable(lcd_preview preview.c "${REPO}/main/psk_modes.c"
    "${REPO}/main/modem_image.c" "${REPO}/main/spectrum.c" ${LVGL_SOURCES})
target_include_directories(lcd_preview PRIVATE stubs . "${REPO}/main"
    "${REPO}/components/psk_rtty/include" "${LVGL}")
target_compile_definitions(lcd_preview PRIVATE LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_compile_options(lcd_preview PRIVATE -O1)
target_link_libraries(lcd_preview PRIVATE m)
