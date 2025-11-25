idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espidf-nats nvs_flash esp_wifi
)
# Note: esp_websocket_client is auto-detected by espidf-nats component
# If available, CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE is automatically defined
