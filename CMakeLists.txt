cmake_minimum_required(VERSION 3.25)
project(rerevved VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.10.0 REQUIRED CONFIG)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sdk.lock.json" REREVVED_SDK_LOCK)
foreach(_property repository commit version dirty platform)
    string(JSON REREVVED_SDK_LOCK_${_property} ERROR_VARIABLE _json_error
        GET "${REREVVED_SDK_LOCK}" ${_property})
    if(_json_error)
        message(FATAL_ERROR "rexglue-sdk.lock.json is missing '${_property}'")
    endif()
endforeach()
if(NOT REREVVED_SDK_LOCK_commit STREQUAL REXGLUE_GIT_REVISION OR
   NOT REREVVED_SDK_LOCK_version STREQUAL REXGLUE_VERSION_STRING OR
   NOT REREVVED_SDK_LOCK_dirty STREQUAL REXGLUE_GIT_DIRTY OR
   NOT REREVVED_SDK_LOCK_platform STREQUAL REXGLUE_BUILD_PLATFORM)
    message(FATAL_ERROR
        "Installed ReXGlue package does not match rexglue-sdk.lock.json")
endif()

find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE REREVVED_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE REREVVED_TRACKED_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(REREVVED_TRACKED_STATUS STREQUAL "")
    set(REREVVED_GIT_DIRTY clean)
else()
    set(REREVVED_GIT_DIRTY dirty)
endif()
configure_file(src/build_provenance.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_provenance.h" @ONLY)

set(REREVVED_SOURCES
    src/app.cpp
    src/compat_hooks.cpp
    src/game_content.cpp
    src/great_general_attachment.cpp
    src/great_general_attachment.h
    src/game_state.cpp
    src/game_state.h
    src/imports.cpp
    src/main.cpp
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
    src/world_api.cpp
    src/world_api.h
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
    REREVVED_WORLD_API_EXPORTS
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

    add_executable(world_api_tests
        src/world_api.cpp
        tests/world_api_test.cpp
    )
    target_include_directories(world_api_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME world_api_contract COMMAND world_api_tests)

    add_executable(world_api_c_header_tests
        src/world_api.cpp
        tests/world_api_c_header_test.c
    )
    target_include_directories(world_api_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME world_api_c_header COMMAND world_api_c_header_tests)

    add_executable(unique_unit_rules_tests
        src/unique_unit_rules.cpp
        src/world_api.cpp
        tests/unique_unit_rules_test.cpp
    )
    target_include_directories(unique_unit_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unique_unit_rules_contract COMMAND unique_unit_rules_tests)

    add_executable(unique_unit_rules_c_header_tests
        src/unique_unit_rules.cpp
        src/world_api.cpp
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
endif()
