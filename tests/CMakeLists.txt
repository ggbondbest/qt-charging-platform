add_executable(test_qt_baseline tst_qt_baseline.cpp)
target_link_libraries(test_qt_baseline PRIVATE
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Test
)
target_compile_definitions(test_qt_baseline PRIVATE
    CHARGING_PLATFORM_QT_BASELINE_VERSION="${CHARGING_PLATFORM_QT_BASELINE_VERSION}"
)
add_test(NAME qt_baseline COMMAND test_qt_baseline)

add_executable(test_server_smoke tst_server_smoke.cpp)
target_link_libraries(test_server_smoke PRIVATE
    ChargingPlatform::ProjectOptions
    ChargingPlatform::ServerCore
    Qt6::Test
)
add_test(NAME server_smoke COMMAND test_server_smoke)

add_executable(test_database_resources tst_database_resources.cpp)
target_link_libraries(test_database_resources PRIVATE
    ChargingPlatform::DatabaseResources
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Test
)
add_test(NAME database_resources COMMAND test_database_resources)

add_executable(test_protocol tst_protocol.cpp)
target_link_libraries(test_protocol PRIVATE
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Test
)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_model_json tst_model_json.cpp)
target_link_libraries(test_model_json PRIVATE
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Test
)
add_test(NAME model_json COMMAND test_model_json)

add_executable(test_login_integration tst_login_integration.cpp)
target_link_libraries(test_login_integration PRIVATE
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ServerCore
    ChargingPlatform::ServerServices
    ChargingPlatform::DatabaseResources
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)
add_test(NAME login_integration COMMAND test_login_integration)
set_tests_properties(login_integration PROPERTIES TIMEOUT 20)

add_executable(test_billing_service tst_billing_service.cpp)
target_link_libraries(test_billing_service PRIVATE
    ChargingPlatform::ServerServices
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Test
)
add_test(NAME billing_service COMMAND test_billing_service)

add_executable(test_charging_workflow tst_charging_workflow.cpp)
target_link_libraries(test_charging_workflow PRIVATE
    ChargingPlatform::ServerServices
    ChargingPlatform::ServerRepositories
    ChargingPlatform::ServerDatabase
    ChargingPlatform::DatabaseResources
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)
add_test(NAME charging_workflow COMMAND test_charging_workflow)
set_tests_properties(charging_workflow PROPERTIES TIMEOUT 20)

add_executable(test_charging_tcp_integration tst_charging_tcp_integration.cpp)
target_link_libraries(test_charging_tcp_integration PRIVATE
    ChargingPlatform::ClientNetwork
    ChargingPlatform::ServerCore
    ChargingPlatform::ServerServices
    ChargingPlatform::ServerRepositories
    ChargingPlatform::ServerDatabase
    ChargingPlatform::DatabaseResources
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)
add_test(NAME charging_tcp_integration COMMAND test_charging_tcp_integration)
set_tests_properties(charging_tcp_integration PROPERTIES TIMEOUT 20)

add_executable(test_network_resilience tst_network_resilience.cpp)
target_link_libraries(test_network_resilience PRIVATE
    ChargingPlatform::ClientNetwork
    ChargingPlatform::ServerCore
    ChargingPlatform::ServerServices
    ChargingPlatform::ServerRepositories
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)
add_test(NAME network_resilience COMMAND test_network_resilience)
set_tests_properties(network_resilience PROPERTIES TIMEOUT 20)

add_executable(test_wallet_service tst_wallet_service.cpp)
target_link_libraries(test_wallet_service PRIVATE
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Test
)
add_test(NAME wallet_service COMMAND test_wallet_service)

add_executable(test_order_service tst_order_service.cpp)
target_link_libraries(test_order_service PRIVATE
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Test
)
add_test(NAME order_service COMMAND test_order_service)

add_executable(test_charging_service tst_charging_service.cpp)
target_link_libraries(test_charging_service PRIVATE
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Test
)
add_test(NAME charging_service COMMAND test_charging_service)

find_program(CHARGING_BASH_EXECUTABLE NAMES bash)
find_program(CHARGING_SQLITE3_EXECUTABLE NAMES sqlite3)
if(CHARGING_BASH_EXECUTABLE AND CHARGING_SQLITE3_EXECUTABLE)
    add_test(
        NAME database_schema
        COMMAND ${CHARGING_BASH_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/verify_database.sh
    )
endif()
