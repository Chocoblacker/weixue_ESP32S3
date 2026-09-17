idf_component_register(
    SRCS "main.c" "app_wifi.c" "app_ota.c" "app_display.c"
    INCLUDE_DIRS "."
)

# LVGL 需要这个宏才能用 <lvgl.h> 这种简写包含方式（沿用官方 BSP 示例的写法）
idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-error=attributes
        -Wno-error=cpp
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)
