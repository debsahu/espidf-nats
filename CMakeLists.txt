cmake_minimum_required(VERSION 3.10)

# Check if esp_websocket_client component is available
# Note: Managed components use namespace prefix (espressif__esp_websocket_client)
# while built-in components use plain name (esp_websocket_client)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("esp_websocket_client" IN_LIST build_components)
    set(WEBSOCKET_AVAILABLE TRUE)
    set(OPTIONAL_REQUIRES esp_websocket_client)
elseif("espressif__esp_websocket_client" IN_LIST build_components)
    set(WEBSOCKET_AVAILABLE TRUE)
    set(OPTIONAL_REQUIRES espressif__esp_websocket_client)
else()
    set(WEBSOCKET_AVAILABLE FALSE)
    set(OPTIONAL_REQUIRES "")
endif()

idf_component_register(
    INCLUDE_DIRS include
    REQUIRES esp-tls lwip ${OPTIONAL_REQUIRES}
)

# Define CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE if WebSocket component is available
if(WEBSOCKET_AVAILABLE)
    target_compile_definitions(${COMPONENT_LIB} INTERFACE CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE=1)
    message(STATUS "espidf-nats: WebSocket support ENABLED (esp_websocket_client found)")
else()
    message(STATUS "espidf-nats: WebSocket support DISABLED (esp_websocket_client not found)")
endif()
