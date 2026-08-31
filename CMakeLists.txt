cmake_minimum_required(VERSION 3.22)

project(uhf_61850 VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(UHF_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(UHF_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(UHF_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(UHF_BUILD_FORMAT_TARGET "Provide the format-check target" ON)

set(UHF_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(
    "${PROJECT_SOURCE_DIR}/src/app/build_info.hpp.in"
    "${UHF_GENERATED_INCLUDE_DIR}/app/build_info.hpp"
    @ONLY
)

add_executable(uhf-gatewayd
    src/main.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
    src/modbus/tcp_server.cpp
    src/modbus/rtu_server.cpp
    src/web/auth_store.cpp
    src/web/crypto.cpp
    src/web/http_server.cpp
)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
target_link_libraries(uhf-gatewayd PRIVATE OpenSSL::Crypto)

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

add_executable(uhf_domain_smoke
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

add_executable(uhf_snapshot_smoke
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

add_executable(uhf_acquisition_smoke
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

add_executable(uhf_acquisition_fault_smoke
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

add_executable(uhf_modbus_tcp_smoke
    tests/modbus_tcp_smoke.cpp
    src/modbus/tcp_server.cpp
    src/acquisition/acquisition.cpp
    src/domain/modbus.cpp
    src/domain/snapshot.cpp
)
target_compile_features(uhf_modbus_tcp_smoke PRIVATE cxx_std_17)
target_include_directories(uhf_modbus_tcp_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
find_package(Threads REQUIRED)
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

add_executable(uhf_modbus_rtu_smoke
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
            NAME uhf_pd1000_sim_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/pd1000_sim_smoke.py"
                "${PROJECT_SOURCE_DIR}/tools/pd1000-sim/pd1000_sim.py"
                "${PROJECT_SOURCE_DIR}/tests/fixtures/pd1000_request_plan.txt"
        )
        add_test(
            NAME uhf_acquisition_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/acquisition_smoke.py"
                "$<TARGET_FILE:uhf_acquisition_smoke>"
                "${PROJECT_SOURCE_DIR}/tools/pd1000-sim/pd1000_sim.py"
                "${PROJECT_SOURCE_DIR}/tests/fixtures/pd1000_request_plan.txt"
        )
    endif()
    add_test(NAME uhf_acquisition_fault_smoke COMMAND uhf_acquisition_fault_smoke)
    add_test(NAME uhf_modbus_tcp_smoke COMMAND uhf_modbus_tcp_smoke)
    add_test(NAME uhf_modbus_rtu_smoke COMMAND uhf_modbus_rtu_smoke)
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

install(TARGETS uhf-gatewayd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
