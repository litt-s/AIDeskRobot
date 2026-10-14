idf_component_register(
    SRCS
        "aiui_api_esp.c"
        "aiui_http_api_esp.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/aiui_list.c"
        "aiui_message_parse_esp.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/aiui_queue.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/aiui_split_synthesize.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/aiui_websocket_api.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/base64.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/cjson.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/hmac_sha256.c"
        "${AIUI_VENDOR_ROOT}/src/aiui/sha256.c"
    INCLUDE_DIRS
        "${AIUI_VENDOR_ROOT}/include"
        "${AIUI_VENDOR_ROOT}/include/aiui"
    PRIV_INCLUDE_DIRS
        "${AIUI_VENDOR_ROOT}/src/aiui"
    PRIV_REQUIRES
        aiui_port
        mbedtls
)

# The upstream open-source SDK contains two harmless legacy warning patterns.
# Keep the vendor files byte-for-byte intact and scope the compatibility flags
# to this component only.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=parentheses
    -Wno-error=type-limits
)

# ESP-IDF's WPA supplicant exports the same generic symbol name. Keep the
# bundled AIUI implementation component-private at link time.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    hmac_sha256=aiui_hmac_sha256
    LOG_DEFAULT_LEVEL=LOG_INFO_LEVEL
)
