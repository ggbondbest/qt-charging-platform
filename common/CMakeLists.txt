add_library(charging_common STATIC
    src/model/enums.cpp
    src/model/model_json.cpp
    src/protocol/frame_codec.cpp
    src/protocol/protocol.cpp
    src/protocol/user_api_contract.cpp
)

add_library(ChargingPlatform::Common ALIAS charging_common)

target_include_directories(charging_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(charging_common
    PUBLIC
        Qt6::Core
    PRIVATE
        ChargingPlatform::ProjectOptions
)

target_compile_features(charging_common PUBLIC cxx_std_17)
