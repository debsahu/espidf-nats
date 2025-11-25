idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espidf-nats nvs_flash esp_wifi
)
