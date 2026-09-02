cmake_minimum_required(VERSION 3.25)
project(rerevved VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.11.0 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sdk.lock.json" REREVVED_SDK_LOCK)
foreach(_property repository commit version dirty)
    string(JSON REREVVED_SDK_LOCK_${_property} ERROR_VARIABLE _json_error
        GET "${REREVVED_SDK_LOCK}" ${_property})
    if(_json_error)
        message(FATAL_ERROR "rexglue-sdk.lock.json is missing '${_property}'")
    endif()
endforeach()
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(REREVVED_SDK_PLATFORM "win-amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(REREVVED_SDK_PLATFORM "win-arm64")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(REREVVED_SDK_PLATFORM "linux-amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(REREVVED_SDK_PLATFORM "linux-arm64")
    endif()
endif()
if(NOT DEFINED REREVVED_SDK_PLATFORM)
    message(FATAL_ERROR "Unsupported ReRevved target platform")
endif()
if(NOT REREVVED_SDK_LOCK_commit STREQUAL REXGLUE_GIT_REVISION OR
   NOT REREVVED_SDK_LOCK_version STREQUAL REXGLUE_VERSION_STRING OR
   NOT REREVVED_SDK_LOCK_dirty STREQUAL REXGLUE_GIT_DIRTY OR
   NOT REREVVED_SDK_PLATFORM STREQUAL REXGLUE_BUILD_PLATFORM)
    message(FATAL_ERROR
        "Installed ReXGlue package does not match the SDK lock or target platform")
endif()

include(cmake/build_info.cmake)
rerevved_write_build_info("${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h")

set(REREVVED_SOURCES
    src/rerevved_app.cpp
    src/rerevved_hooks.cpp
    src/game_content.cpp
    src/great_general_attachment.cpp
    src/great_general_attachment.h
    src/gameplay_state.cpp
    src/gameplay_state.h
    src/gpu/guest_gpu_service.cpp
    src/gpu/guest_gpu_service.h
    src/usb_camera_stubs.cpp
    src/main.cpp
    src/gpu/d3d12/native_renderer_d3d12.cpp
    src/gpu/d3d12/native_renderer_d3d12.h
    src/gpu/diagnostics/native_renderer_guest_state.cpp
    src/gpu/diagnostics/native_renderer_guest_state.h
    src/gpu/diagnostics/native_renderer_passive_trace.cpp
    src/gpu/diagnostics/native_renderer_passive_trace.h
    src/native_renderer_coverage.cpp
    src/native_renderer_coverage.h
    src/native_renderer_coverage_hooks.inc
    src/gpu/renderer_backend.cpp
    src/gpu/renderer_backend.h
    src/unique_era_abilities.cpp
    src/unique_era_abilities_hooks.cpp
    src/unique_era_abilities_registry.h
    src/unique_unit_rules.cpp
    src/unique_unit_rules_hooks.cpp
    src/unique_unit_rules_registry.h
    src/presence.cpp
    src/presence.h
    src/presence_model.cpp
    src/presence_model.h
    src/rush_cost.cpp
    src/rush_cost.h
    src/unit_catalog_api.cpp
    src/unit_catalog_api.h
)

if(WIN32)
    set(REREVVED_RC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/rerevved.ico")
    configure_file(res/rerevved.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc" @ONLY)
    list(APPEND REREVVED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc")
endif()

if(WIN32)
    add_executable(rerevved WIN32 ${REREVVED_SOURCES})
else()
    add_executable(rerevved ${REREVVED_SOURCES})
endif()

if(WIN32)
    target_link_libraries(rerevved PRIVATE d3d12 dxgi dxguid)
endif()

target_include_directories(rerevved PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
)
target_compile_definitions(rerevved PRIVATE
    REREVVED_GAMEPLAY_API_EXPORTS
    REREVVED_UNIQUE_ERA_ABILITIES_API_EXPORTS
    REREVVED_UNIQUE_UNIT_RULES_API_EXPORTS
    REREVVED_UNIT_CATALOG_API_EXPORTS
    REREVVED_VERSION="${PROJECT_VERSION}"
)
if(APPLE)
    target_link_options(rerevved PRIVATE -Wl,-export_dynamic)
elseif(UNIX)
    target_link_options(rerevved PRIVATE -Wl,--export-dynamic)
endif()
rexglue_setup_recompiled_target(rerevved
    MANIFEST rerevved_manifest.toml
    OUTPUT_DIR generated/default
    GPU_PLUGINS xenos)
add_custom_target(native_renderer_coverage_codegen_check
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-native-renderer-coverage.py --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking native renderer coverage generation"
    VERBATIM)
add_dependencies(rerevved_codegen native_renderer_coverage_codegen_check)

include(CTest)
if(BUILD_TESTING)
    add_executable(presence_model_tests
        src/presence_model.cpp
        tests/presence_model_test.cpp
    )
    target_include_directories(presence_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME presence_model COMMAND presence_model_tests)

    add_executable(rush_cost_tests
        src/rush_cost.cpp
        tests/rush_cost_test.cpp
    )
    target_include_directories(rush_cost_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME rush_cost_producer_consumer COMMAND rush_cost_tests)

    add_executable(great_general_attachment_tests
        src/great_general_attachment.cpp
        tests/great_general_attachment_test.cpp
    )
    target_include_directories(great_general_attachment_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(great_general_attachment_tests PRIVATE rex::runtime)
    add_test(NAME great_general_attachment COMMAND great_general_attachment_tests)

    add_executable(unit_catalog_api_tests
        src/unit_catalog_api.cpp
        tests/unit_catalog_api_test.cpp
    )
    target_include_directories(unit_catalog_api_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_catalog_api_contract COMMAND unit_catalog_api_tests)

    add_executable(unit_catalog_api_c_header_tests
        src/unit_catalog_api.cpp
        tests/unit_catalog_api_c_header_test.c
    )
    target_include_directories(unit_catalog_api_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_catalog_api_c_header COMMAND unit_catalog_api_c_header_tests)

    add_executable(unique_unit_rules_tests
        src/unique_unit_rules.cpp
        src/unit_catalog_api.cpp
        tests/unique_unit_rules_test.cpp
    )
    target_include_directories(unique_unit_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unique_unit_rules_contract COMMAND unique_unit_rules_tests)

    add_executable(unique_unit_rules_c_header_tests
        src/unique_unit_rules.cpp
        src/unit_catalog_api.cpp
        tests/unique_unit_rules_c_header_test.c
    )
    target_include_directories(unique_unit_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unique_unit_rules_c_header COMMAND unique_unit_rules_c_header_tests)

    add_executable(unique_era_abilities_tests
        src/unique_era_abilities.cpp
        src/unique_era_abilities_hooks.cpp
        tests/unique_era_abilities_test.cpp
    )
    target_include_directories(unique_era_abilities_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(unique_era_abilities_tests PRIVATE rex::runtime)
    add_test(NAME unique_era_abilities_contract COMMAND unique_era_abilities_tests)

    add_executable(unique_era_abilities_c_header_tests
        src/unique_era_abilities.cpp
        tests/unique_era_abilities_c_header_test.c
    )
    target_include_directories(unique_era_abilities_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unique_era_abilities_c_header COMMAND unique_era_abilities_c_header_tests)

    add_executable(native_renderer_coverage_tests
        src/native_renderer_coverage.cpp
        tests/native_renderer_coverage_test.cpp
    )
    target_include_directories(native_renderer_coverage_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(native_renderer_coverage_tests PRIVATE
        REREVVED_NATIVE_RENDERER_COVERAGE_TESTING=1
    )
    add_test(NAME native_renderer_coverage COMMAND native_renderer_coverage_tests)

    add_executable(guest_gpu_service_tests
        src/gpu/guest_gpu_service.cpp
        tests/gpu/guest_gpu_service_test.cpp
    )
    target_include_directories(guest_gpu_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(guest_gpu_service_tests PRIVATE rex::runtime)
    add_test(NAME guest_gpu_service COMMAND guest_gpu_service_tests)

    add_executable(renderer_backend_tests
        src/gpu/renderer_backend.cpp
        tests/gpu/renderer_backend_test.cpp
    )
    target_include_directories(renderer_backend_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME renderer_backend COMMAND renderer_backend_tests)

    add_executable(fence_trace_finalization_gate_tests
        tests/fence_trace_finalization_gate_test.cpp
    )
    target_include_directories(fence_trace_finalization_gate_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME fence_trace_finalization_gate COMMAND fence_trace_finalization_gate_tests)

    add_executable(native_renderer_guest_state_tests
        src/gpu/diagnostics/native_renderer_guest_state.cpp
        tests/gpu/diagnostics/native_renderer_guest_state_test.cpp
    )
    target_include_directories(native_renderer_guest_state_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME native_renderer_guest_state COMMAND native_renderer_guest_state_tests)

    add_executable(native_renderer_passive_trace_tests
        src/gpu/diagnostics/native_renderer_passive_trace.cpp
        tests/gpu/diagnostics/native_renderer_passive_trace_test.cpp
    )
    target_include_directories(native_renderer_passive_trace_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(native_renderer_passive_trace_tests PRIVATE rex::runtime)
    add_test(NAME native_renderer_passive_trace COMMAND native_renderer_passive_trace_tests)
endif()
