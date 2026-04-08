idf_component_register(
    SRCS
        "src/service_weather.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_timer
)
