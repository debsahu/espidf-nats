idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espidf-nats nvs_flash esp_wifi esp_driver_gpio
    # Plain TLS
    # EMBED_TXTFILES  ../docker/certs/ca-cert.pem
    # Client mTLS
    # EMBED_TXTFILES ../docker/certs/client-cert.pem ../docker/certs/client-key.pem
)
# Note: esp_websocket_client is auto-detected by espidf-nats component
# If available, CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE is automatically defined
