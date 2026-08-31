cmake_minimum_required(VERSION 3.22)

project(uhf_61850 VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(UHF_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(UHF_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(UHF_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(UHF_BUILD_FORMAT_TARGET "Provide the format-check target" ON)
option(UHF_ENABLE_IEC61850 "Build the bundled libIEC61850 protocol stack" ON)

if(UHF_ENABLE_IEC61850)
    add_executable(uhf_iec61850_association_limit_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_association_limit_smoke.cpp
    )
    target_compile_features(uhf_iec61850_association_limit_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_association_limit_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/iso_mms/asn1c"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/logging"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
    target_link_libraries(uhf_iec61850_association_limit_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_association_limit_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    # Keep the target protocol stack self-contained and bounded.  The upstream
    # project exposes these as cache options, so set them before entering its
    # subdirectory and make accidental host-wide defaults impossible.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    set(CONFIG_MMS_MAXIMUM_PDU_SIZE 16384 CACHE STRING "" FORCE)
    set(CONFIG_MAXIMUM_TCP_CLIENT_CONNECTIONS 4 CACHE STRING "" FORCE)
    set(CONFIG_IEC61850_MEMORY_LIMIT_BYTES 8388608)
    set(CONFIG_MMS_SINGLE_THREADED ON CACHE BOOL "" FORCE)
    set(CONFIG_MMS_MAX_NUMBER_OF_DATA_SET_MEMBERS 512 CACHE STRING "" FORCE)
    set(CONFIG_MMS_MAX_NUMBER_OF_REQUEST_ELEMENTS 512 CACHE STRING "" FORCE)
    set(CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLING 2)
    set(CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLED 2)
    set(CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL 32)
    set(CONFIG_INCLUDE_GOOSE_SUPPORT OFF)
    set(CONFIG_IEC61850_SAMPLED_VALUES_SUPPORT OFF)
    set(CONFIG_IEC61850_CONTROL_SERVICE OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_SETTING_GROUPS OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_LOG_SERVICE OFF CACHE BOOL "" FORCE)
    # libIEC61850's private IED header uses this internal type unconditionally.
    set(CONFIG_IEC61850_SERVICE_TRACKING ON CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_L2_GOOSE OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_L2_SMV OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_R_GOOSE OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_R_SMV OFF CACHE BOOL "" FORCE)
    set(CONFIG_IEC61850_SNTP_CLIENT OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libiec61850 EXCLUDE_FROM_ALL)
endif()

set(UHF_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(
    "${PROJECT_SOURCE_DIR}/src/app/build_info.hpp.in"
    "${UHF_GENERATED_INCLUDE_DIR}/app/build_info.hpp"
    @ONLY
)

add_executable(uhf-gatewayd
    src/main.cpp
    src/config/config_store.cpp
    src/app/gateway_runtime.cpp
    src/acquisition/acquisition.cpp
    src/acquisition/loopback_pd1000.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
    src/modbus/tcp_server.cpp
    src/modbus/rtu_server.cpp
    src/iec61850/model.cpp
    src/iec61850/server.cpp
    src/storage/frame_store.cpp
    src/storage/event_detector.cpp
    src/storage/event_store.cpp
    src/storage/storage_cleaner.cpp
    src/storage/persistence_runtime.cpp
    src/logging/logger.cpp
    src/health/health.cpp
    src/web/auth_store.cpp
    src/web/crypto.cpp
    src/web/tls_context.cpp
    src/web/http_server.cpp
    src/web/snapshot_json.cpp
    src/platform/systemd/notify.cpp
    src/platform/privileged/unix_socket.cpp
    src/platform/network/network_config.cpp
)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)
target_link_libraries(uhf-gatewayd PRIVATE OpenSSL::Crypto OpenSSL::SSL Threads::Threads)

if(UHF_ENABLE_IEC61850)
    target_link_libraries(uhf-gatewayd PRIVATE iec61850)
    target_include_directories(uhf-gatewayd PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/iso_mms/asn1c"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/logging"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
endif()

target_compile_features(uhf-gatewayd PRIVATE cxx_std_17)
target_include_directories(uhf-gatewayd PRIVATE
    "${UHF_GENERATED_INCLUDE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
)

if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf-gatewayd PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

if(BUILD_TESTING)
    set(UHF_TEST_TARGET_OPTIONS)
else()
    set(UHF_TEST_TARGET_OPTIONS EXCLUDE_FROM_ALL)
endif()

add_executable(uhf_domain_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/domain_smoke.cpp
    src/domain/modbus.cpp
)
target_compile_features(uhf_domain_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_domain_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_domain_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_snapshot_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/snapshot_smoke.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_snapshot_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_snapshot_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_snapshot_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_web_snapshot_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/web_snapshot_smoke.cpp
    src/web/snapshot_json.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_web_snapshot_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_web_snapshot_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_web_snapshot_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_acquisition_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/acquisition_smoke.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_acquisition_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_acquisition_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_acquisition_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_acquisition_fault_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/acquisition_fault_smoke.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_acquisition_fault_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_acquisition_fault_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_acquisition_fault_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_acquisition_reload_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/acquisition_reload_smoke.cpp
    src/acquisition/loopback_pd1000.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_acquisition_reload_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_acquisition_reload_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_acquisition_reload_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_loopback_acquisition_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/loopback_acquisition_smoke.cpp
    src/acquisition/loopback_pd1000.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_loopback_acquisition_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_loopback_acquisition_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_loopback_acquisition_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_serial_port_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/serial_port_smoke.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_serial_port_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_serial_port_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
find_library(UHF_SERIAL_UTIL_LIBRARY NAMES util)
if(UHF_SERIAL_UTIL_LIBRARY)
    target_link_libraries(uhf_serial_port_smoke PRIVATE "${UHF_SERIAL_UTIL_LIBRARY}")
endif()

add_executable(uhf_network_config_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/network_config_smoke.cpp
    src/platform/network/network_config.cpp
)
target_compile_features(uhf_network_config_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_network_config_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_network_config_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_dhcp_client_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/dhcp_client_smoke.cpp
    src/platform/network/dhcp_client.cpp
)
target_compile_features(uhf_dhcp_client_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_dhcp_client_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_dhcp_client_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_network_transaction_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/network_transaction_smoke.cpp
    src/platform/network/network_transaction.cpp
    src/platform/network/network_config.cpp
)
target_compile_features(uhf_network_transaction_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_network_transaction_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_network_transaction_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_linux_backend_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/linux_backend_smoke.cpp
    src/platform/network/linux_backend.cpp
    src/platform/network/dhcp_client.cpp
    src/platform/network/network_transaction.cpp
    src/platform/network/network_config.cpp
)
target_compile_features(uhf_linux_backend_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_linux_backend_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_linux_backend_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_linux_status_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/linux_status_smoke.cpp
    src/platform/network/linux_status.cpp
)
target_compile_features(uhf_linux_status_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_linux_status_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_linux_status_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_unix_socket_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/unix_socket_smoke.cpp
    src/platform/privileged/unix_socket.cpp
    src/platform/network/network_config.cpp
)
target_compile_features(uhf_unix_socket_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_unix_socket_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf_unix_socket_smoke PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_unix_socket_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_network_service_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/network_service_smoke.cpp
    src/platform/privileged/network_service.cpp
    src/platform/privileged/maintenance.cpp
    src/platform/privileged/unix_socket.cpp
    src/platform/network/linux_backend.cpp
    src/platform/network/dhcp_client.cpp
    src/platform/network/network_transaction.cpp
    src/platform/network/network_config.cpp
    src/platform/network/linux_status.cpp
)
target_compile_features(uhf_network_service_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_network_service_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf_network_service_smoke PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_network_service_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf-privilegedd
    src/privileged_main.cpp
    src/platform/privileged/network_service.cpp
    src/platform/privileged/maintenance.cpp
    src/platform/privileged/unix_socket.cpp
    src/platform/network/linux_backend.cpp
    src/platform/network/dhcp_client.cpp
    src/platform/network/network_transaction.cpp
    src/platform/network/network_config.cpp
    src/platform/network/linux_status.cpp
)
target_compile_features(uhf-privilegedd PRIVATE cxx_std_17)
target_include_directories(uhf-privilegedd PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf-privilegedd PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf-privilegedd PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf-network-recovery
    src/network_recovery_main.cpp
    src/platform/privileged/network_service.cpp
    src/platform/privileged/maintenance.cpp
    src/platform/privileged/unix_socket.cpp
    src/platform/network/linux_backend.cpp
    src/platform/network/dhcp_client.cpp
    src/platform/network/network_transaction.cpp
    src/platform/network/network_config.cpp
    src/platform/network/linux_status.cpp
)
target_compile_features(uhf-network-recovery PRIVATE cxx_std_17)
target_include_directories(uhf-network-recovery PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf-network-recovery PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf-network-recovery PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf-auth-init
    src/auth_init_main.cpp
    src/web/auth_store.cpp
    src/web/crypto.cpp
)
target_compile_features(uhf-auth-init PRIVATE cxx_std_17)
target_include_directories(uhf-auth-init PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf-auth-init PRIVATE OpenSSL::Crypto)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf-auth-init PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf-tls-init
    src/tls_init_main.cpp
    src/web/tls_context.cpp
)
target_compile_features(uhf-tls-init PRIVATE cxx_std_17)
target_include_directories(uhf-tls-init PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf-tls-init PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf-tls-init PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_systemd_notify_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/systemd_notify_smoke.cpp
    src/platform/systemd/notify.cpp
)
target_compile_features(uhf_systemd_notify_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_systemd_notify_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_systemd_notify_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_serial_port_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_modbus_tcp_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/modbus_tcp_smoke.cpp
    src/modbus/tcp_server.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_modbus_tcp_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_modbus_tcp_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf_modbus_tcp_smoke PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_modbus_tcp_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_modbus_rtu_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/modbus_rtu_smoke.cpp
    src/modbus/rtu_server.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_modbus_rtu_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_modbus_rtu_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
find_library(UHF_UTIL_LIBRARY NAMES util)
if(UHF_UTIL_LIBRARY)
    target_link_libraries(uhf_modbus_rtu_smoke PRIVATE "${UHF_UTIL_LIBRARY}")
endif()

add_executable(uhf_frame_store_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/frame_store_smoke.cpp
    src/storage/frame_store.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_frame_store_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_frame_store_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_frame_store_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_event_detector_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/event_detector_smoke.cpp
    src/storage/event_detector.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_event_detector_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_event_detector_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_event_detector_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_event_store_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/event_store_smoke.cpp
    src/storage/event_store.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_event_store_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_event_store_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_event_store_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_logger_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/logger_smoke.cpp
    src/logging/logger.cpp
)
target_compile_features(uhf_logger_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_logger_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_logger_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_health_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/health_smoke.cpp
    src/health/health.cpp
)
target_compile_features(uhf_health_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_health_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_health_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_storage_cleaner_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/storage_cleaner_smoke.cpp
    src/storage/storage_cleaner.cpp
)
target_compile_features(uhf_storage_cleaner_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_storage_cleaner_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_storage_cleaner_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(uhf_persistence_runtime_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/persistence_runtime_smoke.cpp
    src/config/config_store.cpp
    src/storage/persistence_runtime.cpp
    src/storage/frame_store.cpp
    src/storage/event_detector.cpp
    src/storage/event_store.cpp
    src/storage/storage_cleaner.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
    src/logging/logger.cpp
)
target_compile_features(uhf_persistence_runtime_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_persistence_runtime_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(uhf_persistence_runtime_smoke PRIVATE Threads::Threads)
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_persistence_runtime_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

    add_executable(uhf_config_store_smoke ${UHF_TEST_TARGET_OPTIONS}
    tests/config_store_smoke.cpp
    src/config/config_store.cpp
)
target_compile_features(uhf_config_store_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_config_store_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_config_store_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

if(UHF_ENABLE_IEC61850)
    add_executable(uhf_iec61850_ber_limit_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_ber_limit_smoke.cpp
    )
    target_compile_features(uhf_iec61850_ber_limit_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_ber_limit_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
    target_link_libraries(uhf_iec61850_ber_limit_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_ber_limit_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    add_executable(uhf_iec61850_request_limit_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_request_limit_smoke.cpp
        src/iec61850/server.cpp
        src/iec61850/model.cpp
        src/acquisition/acquisition.cpp
        src/domain/modbus.cpp
        src/domain/snapshot.cpp
    )
    target_compile_features(uhf_iec61850_request_limit_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_request_limit_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/iso_mms/asn1c"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/logging"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
    target_link_libraries(uhf_iec61850_request_limit_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_request_limit_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    add_executable(uhf_iec61850_memory_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_memory_smoke.cpp
    )
    target_compile_features(uhf_iec61850_memory_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_memory_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
    )
    target_link_libraries(uhf_iec61850_memory_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_memory_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    add_executable(uhf_iec61850_model_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_model_smoke.cpp
        src/iec61850/model.cpp
    )
    target_compile_features(uhf_iec61850_model_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_model_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/iso_mms/asn1c"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/logging"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
    target_link_libraries(uhf_iec61850_model_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_model_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    add_executable(uhf_iec61850_server_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/iec61850_server_smoke.cpp
        src/iec61850/server.cpp
        src/iec61850/model.cpp
        src/acquisition/acquisition.cpp
        src/domain/modbus.cpp
        src/domain/snapshot.cpp
    )
    target_compile_features(uhf_iec61850_server_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_iec61850_server_smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/hal/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/common/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/iec61850/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/inc_private"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/mms/iso_mms/asn1c"
        "${PROJECT_SOURCE_DIR}/third_party/libiec61850/src/logging"
        "${PROJECT_BINARY_DIR}/third_party/libiec61850/config"
    )
    target_link_libraries(uhf_iec61850_server_smoke PRIVATE iec61850)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_iec61850_server_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()

    add_executable(uhf_tls_context_smoke ${UHF_TEST_TARGET_OPTIONS}
        tests/tls_context_smoke.cpp
        src/web/tls_context.cpp
    )
    target_compile_features(uhf_tls_context_smoke PRIVATE cxx_std_17)
    target_include_directories(uhf_tls_context_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(uhf_tls_context_smoke PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(uhf_tls_context_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()
endif()

if(UHF_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhf_modbus_rtu_smoke PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

if(UHF_ENABLE_ASAN OR UHF_ENABLE_UBSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require a GNU or Clang compiler")
    endif()

    set(UHF_SANITIZER_FLAGS "")
    if(UHF_ENABLE_ASAN)
        list(APPEND UHF_SANITIZER_FLAGS address)
    endif()
    if(UHF_ENABLE_UBSAN)
        list(APPEND UHF_SANITIZER_FLAGS undefined)
    endif()
    string(JOIN "," UHF_SANITIZER_LIST ${UHF_SANITIZER_FLAGS})

    target_compile_options(uhf-gatewayd PRIVATE "-fsanitize=${UHF_SANITIZER_LIST}")
    target_link_options(uhf-gatewayd PRIVATE "-fsanitize=${UHF_SANITIZER_LIST}")
endif()

set_target_properties(uhf-gatewayd PROPERTIES
    OUTPUT_NAME uhf-gatewayd
)

if(BUILD_TESTING)
    add_test(
        NAME uhf_gatewayd_smoke
        COMMAND uhf-gatewayd --self-test
    )
    set_tests_properties(uhf_gatewayd_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "uhf-gatewayd self-test: OK"
    )
    add_test(
        NAME uhf_domain_smoke
        COMMAND uhf_domain_smoke
            "${PROJECT_SOURCE_DIR}/tests/fixtures/pd1000_request_plan.txt"
    )
    add_test(NAME uhf_snapshot_smoke COMMAND uhf_snapshot_smoke)
    add_test(NAME uhf_web_snapshot_smoke COMMAND uhf_web_snapshot_smoke)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME uhf_web_preview_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/web_preview_smoke.py"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_web_server_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/web_server_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_web_tls_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/web_tls_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_web_live_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/web_live_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_runtime_modbus_rtu_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/runtime_modbus_rtu_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_websocket_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/websocket_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_config_runtime_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/config_runtime_smoke.py"
                "$<TARGET_FILE:uhf-gatewayd>"
                "${PROJECT_SOURCE_DIR}/web"
        )
        add_test(
            NAME uhf_pd1000_sim_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/pd1000_sim_smoke.py"
                "${PROJECT_SOURCE_DIR}/tools/pd1000-sim/pd1000_sim.py"
                "${PROJECT_SOURCE_DIR}/tests/fixtures/pd1000_request_plan.txt"
        )
        add_test(
            NAME uhf_iec61850_icd_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/iec61850_icd_smoke.py"
                "${PROJECT_SOURCE_DIR}/config/UHFPD1.icd"
        )
        if(UHF_ENABLE_IEC61850)
            add_test(
                NAME uhf_iec61850_stack_config_smoke
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/iec61850_stack_config_smoke.py"
                    "${CMAKE_BINARY_DIR}/third_party/libiec61850/config/stack_config.h"
            )
        endif()
        add_test(
            NAME uhf_acquisition_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/acquisition_smoke.py"
                "$<TARGET_FILE:uhf_acquisition_smoke>"
                "${PROJECT_SOURCE_DIR}/tools/pd1000-sim/pd1000_sim.py"
                "${PROJECT_SOURCE_DIR}/tests/fixtures/pd1000_request_plan.txt"
        )
        add_test(
            NAME uhf_packaging_policy_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/packaging_policy_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_privileged_service_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/privileged_service_smoke.py"
        )
        add_test(
            NAME uhf_install_tree_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/install_tree_smoke.py"
                "${PROJECT_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        add_test(
            NAME uhf_network_recovery_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/network_recovery_smoke.py"
                "$<TARGET_FILE:uhf-network-recovery>"
        )
        add_test(
            NAME uhf_tls_init_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tls_init_smoke.py"
                "$<TARGET_FILE:uhf-tls-init>"
        )
        add_test(
            NAME uhf_systemd_policy_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/systemd_policy_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_release_build_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/release_build_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_preflight_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/preflight_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_installer_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/installer_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_release_guard_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/release_guard_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME uhf_legacy_cutover_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/legacy_cutover_smoke.py"
                "${PROJECT_SOURCE_DIR}"
        )
    endif()
    add_test(NAME uhf_acquisition_fault_smoke COMMAND uhf_acquisition_fault_smoke)
    add_test(NAME uhf_acquisition_reload_smoke COMMAND uhf_acquisition_reload_smoke)
    add_test(NAME uhf_loopback_acquisition_smoke COMMAND uhf_loopback_acquisition_smoke)
    add_test(NAME uhf_serial_port_smoke COMMAND uhf_serial_port_smoke)
    add_test(NAME uhf_network_config_smoke COMMAND uhf_network_config_smoke)
    add_test(NAME uhf_dhcp_client_smoke COMMAND uhf_dhcp_client_smoke)
    add_test(NAME uhf_network_transaction_smoke COMMAND uhf_network_transaction_smoke)
    add_test(NAME uhf_linux_backend_smoke COMMAND uhf_linux_backend_smoke)
    add_test(NAME uhf_linux_status_smoke COMMAND uhf_linux_status_smoke)
    add_test(NAME uhf_unix_socket_smoke COMMAND uhf_unix_socket_smoke)
    add_test(NAME uhf_network_service_smoke COMMAND uhf_network_service_smoke)
    add_test(NAME uhf_modbus_tcp_smoke COMMAND uhf_modbus_tcp_smoke)
    add_test(NAME uhf_modbus_rtu_smoke COMMAND uhf_modbus_rtu_smoke)
    add_test(NAME uhf_frame_store_smoke COMMAND uhf_frame_store_smoke)
    add_test(NAME uhf_event_detector_smoke COMMAND uhf_event_detector_smoke)
    add_test(NAME uhf_event_store_smoke COMMAND uhf_event_store_smoke)
    add_test(NAME uhf_logger_smoke COMMAND uhf_logger_smoke)
    add_test(NAME uhf_health_smoke COMMAND uhf_health_smoke)
    add_test(NAME uhf_storage_cleaner_smoke COMMAND uhf_storage_cleaner_smoke)
    add_test(NAME uhf_persistence_runtime_smoke COMMAND uhf_persistence_runtime_smoke)
    add_test(NAME uhf_config_store_smoke COMMAND uhf_config_store_smoke)
    add_test(NAME uhf_systemd_notify_smoke COMMAND uhf_systemd_notify_smoke)
    add_test(NAME uhf_tls_context_smoke COMMAND uhf_tls_context_smoke)
    if(UHF_ENABLE_IEC61850)
        add_test(NAME uhf_iec61850_association_limit_smoke COMMAND uhf_iec61850_association_limit_smoke)
        add_test(NAME uhf_iec61850_ber_limit_smoke COMMAND uhf_iec61850_ber_limit_smoke)
        add_test(NAME uhf_iec61850_memory_smoke COMMAND uhf_iec61850_memory_smoke)
        add_test(NAME uhf_iec61850_request_limit_smoke COMMAND uhf_iec61850_request_limit_smoke)
        add_test(NAME uhf_iec61850_model_smoke COMMAND uhf_iec61850_model_smoke)
        add_test(NAME uhf_iec61850_server_smoke COMMAND uhf_iec61850_server_smoke)
    endif()
endif()

if(UHF_BUILD_FORMAT_TARGET)
    find_program(UHF_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
    if(UHF_CLANG_FORMAT_EXECUTABLE)
        file(GLOB_RECURSE UHF_FORMAT_SOURCES CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/src/*.cpp"
            "${PROJECT_SOURCE_DIR}/src/*.hpp"
            "${PROJECT_SOURCE_DIR}/tests/*.cpp"
            "${PROJECT_SOURCE_DIR}/tests/*.hpp"
        )
        add_custom_target(format-check
            COMMAND "${UHF_CLANG_FORMAT_EXECUTABLE}"
                --dry-run
                --Werror
                ${UHF_FORMAT_SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Checking C++ formatting"
            VERBATIM
        )
    else()
        add_custom_target(format-check
            COMMAND "${CMAKE_COMMAND}" -E echo
                "clang-format is not installed; format-check skipped"
            COMMENT "Checking C++ formatting"
            VERBATIM
        )
    endif()
endif()

install(TARGETS uhf-gatewayd uhf-privilegedd uhf-network-recovery uhf-auth-init uhf-tls-init
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES
    config/defaults.json
    config/schema.json
    config/network.json
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/uhf-gateway")
install(DIRECTORY web/ DESTINATION web)
install(FILES
    config/defaults.json
    config/schema.json
    config/network.json
    config/UHFPD1.icd
    third_party/manifest.json
    DESTINATION config
)
install(PROGRAMS packaging/dhclient/uhf-gateway-hook DESTINATION libexec/uhf-gateway)
install(PROGRAMS packaging/release-guard.sh DESTINATION libexec/uhf-gateway)
install(PROGRAMS
    packaging/legacy-cutover.sh
    packaging/legacy-recovery.sh
    DESTINATION libexec/uhf-gateway
)
install(DIRECTORY packaging/systemd/ DESTINATION share/uhf-gateway/systemd)
install(FILES packaging/rsyslog/uhf-gateway.conf DESTINATION share/uhf-gateway/rsyslog)
install(FILES packaging/logrotate/uhf-gateway DESTINATION share/uhf-gateway/logrotate)
