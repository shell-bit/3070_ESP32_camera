file(GLOB sources CONFIGURE_DEPENDS "*.cpp")
idf_component_register(SRCS ${sources} INCLUDE_DIRS "." REQUIRES arduino esp32-camera esp_http_server json esp_timer esp_wifi fatfs wear_levelling mbedtls human_face_detect human_face_recognition)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_FACE_ENABLED=1 PROJECT_UPLOAD_V2=1)
