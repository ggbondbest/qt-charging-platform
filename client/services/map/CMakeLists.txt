add_library(charging_client_map_services STATIC
    map_geo_service.cpp
    map_geo_service.h
)
add_library(ChargingPlatform::ClientMapServices ALIAS charging_client_map_services)

target_include_directories(charging_client_map_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_map_services
    PUBLIC
    Qt6::Core
    Qt6::Network
    PRIVATE
    ChargingPlatform::ProjectOptions
)

# 2026-09-08 用户拍板：演示 key 入库（client/config/map_services.json），
# 拉代码即测。env 任一变量已定义（含空）→ env 权威、本文件被忽略，
# 故 ctest 全量注入空 env 变量即与真实网络绝缘（tests/CMakeLists.txt 尾部）。
target_compile_definitions(charging_client_map_services PRIVATE
    CHARGING_MAP_CONFIG_FILE="${CMAKE_SOURCE_DIR}/client/config/map_services.json"
)
