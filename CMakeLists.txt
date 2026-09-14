cmake_minimum_required(VERSION 3.25)
project(rerevved VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.12.0 REQUIRED CONFIG)
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
    src/app.cpp
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
    src/gpu/d3d12/native_draw_replay.cpp
    src/gpu/d3d12/native_draw_replay.h
    src/gpu/d3d12/native_frame_replay.cpp
    src/gpu/d3d12/native_frame_replay.h
    src/gpu/d3d12/native_menu_panel.cpp
    src/gpu/d3d12/native_menu_panel.h
    src/gpu/d3d12/native_menu_draw.cpp
    src/gpu/d3d12/native_menu_draw.h
    src/gpu/d3d12/native_guest_menu_draw.cpp
    src/gpu/d3d12/native_guest_menu_draw.h
    src/gpu/d3d12/native_menu_frame.cpp
    src/gpu/d3d12/native_menu_frame.h
    src/gpu/d3d12/native_texture_upload.cpp
    src/gpu/d3d12/native_texture_upload.h
    src/gpu/diagnostics/native_menu_shadow.cpp
    src/gpu/diagnostics/native_menu_shadow.h
    src/gpu/diagnostics/native_menu_frame_shadow.cpp
    src/gpu/diagnostics/native_menu_frame_shadow.h
    src/gpu/diagnostics/native_guest_draw_capture.cpp
    src/gpu/diagnostics/native_guest_draw_capture.h
    src/gpu/diagnostics/native_renderer_guest_state.cpp
    src/gpu/diagnostics/native_renderer_guest_state.h
    src/gpu/diagnostics/native_renderer_passive_trace.cpp
    src/gpu/diagnostics/native_renderer_passive_trace.h
    src/native_renderer_coverage.cpp
    src/native_renderer_coverage.h
    src/native_renderer_coverage_hooks.inc
    src/nation_select_text.cpp
    src/nation_select_text_hooks.cpp
    src/nation_select_text_registry.h
    src/main_menu_logo_asset.cpp
    src/main_menu_logo_hooks.cpp
    src/gpu/renderer_backend.cpp
    src/gpu/renderer_backend.h
    src/unique_era_abilities.cpp
    src/unique_era_abilities_hooks.cpp
    src/unique_era_abilities_registry.h
    src/unique_unit_rules.cpp
    src/unique_unit_rules_hooks.cpp
    src/unique_unit_rules_registry.h
    src/unit_movement_rules.cpp
    src/unit_movement_rules_hooks.cpp
    src/unit_movement_rules_registry.h
    src/unit_production_cost_rules.cpp
    src/unit_production_cost_rules_hooks.cpp
    src/unit_production_cost_rules_registry.h
    src/unit_combat_rules.cpp
    src/unit_combat_rules_hooks.cpp
    src/unit_combat_rules_registry.h
    src/unit_effect_rules.cpp
    src/unit_effect_rules_hooks.cpp
    src/unit_effect_rules_registry.h
    src/terrain_yield_rules.cpp
    src/terrain_yield_rules_hooks.cpp
    src/terrain_yield_rules_registry.h
    src/discord_presence.cpp
    src/discord_presence.h
    src/discord_presence_model.cpp
    src/discord_presence_model.h
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
    target_link_libraries(rerevved PRIVATE d3d12 dxgi dxguid d3dcompiler)
endif()

if(WIN32)
    add_executable(native_draw_replay
        tools/native-draw-replay/main.cpp
        src/gpu/d3d12/native_renderer_d3d12.cpp
        src/gpu/d3d12/native_renderer_d3d12.h
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_draw_replay.h
        src/gpu/d3d12/native_frame_replay.cpp
        src/gpu/d3d12/native_frame_replay.h
        src/gpu/diagnostics/native_renderer_guest_state.cpp
        src/gpu/diagnostics/native_renderer_guest_state.h
    )
    target_include_directories(native_draw_replay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(native_draw_replay PRIVATE
        rex::runtime
        d3d12
        dxgi
        dxguid
        d3dcompiler
    )
    rexglue_apply_target_settings(native_draw_replay)
    add_custom_command(TARGET native_draw_replay POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:native_draw_replay>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:native_draw_replay>;$<TARGET_FILE_DIR:native_draw_replay>>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

target_include_directories(rerevved PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
)
target_compile_definitions(rerevved PRIVATE
    GAMEPLAY_API_EXPORTS
    NATION_SELECT_TEXT_API_EXPORTS
    ERA_ABILITIES_API_EXPORTS
    UNIQUE_UNIT_RULES_API_EXPORTS
    UNIT_MOVEMENT_RULES_API_EXPORTS
    UNIT_PRODUCTION_COST_RULES_API_EXPORTS
    UNIT_COMBAT_RULES_API_EXPORTS
    UNIT_EFFECT_RULES_API_EXPORTS
    TERRAIN_YIELD_RULES_API_EXPORTS
    UNIT_CATALOG_API_EXPORTS
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

set(REREVVED_BUNDLED_ASSET_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/asset-overrides")
add_custom_command(TARGET rerevved POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:rerevved>/asset-overrides"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${REREVVED_BUNDLED_ASSET_ROOT}/default_asset_order.txt"
        "$<TARGET_FILE_DIR:rerevved>/asset-overrides/default_asset_order.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${REREVVED_BUNDLED_ASSET_ROOT}/rerevved-logo"
        "$<TARGET_FILE_DIR:rerevved>/asset-overrides/rerevved-logo"
    COMMENT "Staging bundled asset overrides"
    VERBATIM)
add_custom_target(native_renderer_coverage_codegen_check
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-native-renderer-coverage.py --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking native renderer coverage generation"
    VERBATIM)
add_dependencies(rerevved_codegen native_renderer_coverage_codegen_check)

include(CTest)
if(BUILD_TESTING)
    add_executable(discord_presence_model_tests
        src/discord_presence_model.cpp
        tests/discord_presence_model_test.cpp
    )
    target_include_directories(discord_presence_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME discord_presence_model COMMAND discord_presence_model_tests)

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

    add_executable(nation_select_text_tests
        src/nation_select_text.cpp
        src/unit_catalog_api.cpp
        tests/nation_select_text_test.cpp
    )
    target_include_directories(nation_select_text_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME nation_select_text_contract COMMAND nation_select_text_tests)

    add_executable(nation_select_text_c_header_tests
        src/nation_select_text.cpp
        src/unit_catalog_api.cpp
        tests/nation_select_text_c_header_test.c
    )
    target_include_directories(nation_select_text_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME nation_select_text_c_header COMMAND nation_select_text_c_header_tests)

    add_executable(main_menu_logo_asset_tests
        src/main_menu_logo_asset.cpp
        tests/main_menu_logo_asset_test.cpp
    )
    target_include_directories(main_menu_logo_asset_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(main_menu_logo_asset_tests PRIVATE rex::runtime)
    add_test(NAME main_menu_logo_asset COMMAND main_menu_logo_asset_tests)

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

    add_executable(unit_movement_rules_tests
        src/unit_movement_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_movement_rules_test.cpp
    )
    target_include_directories(unit_movement_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_movement_rules_contract COMMAND unit_movement_rules_tests)

    add_executable(unit_movement_rules_c_header_tests
        src/unit_movement_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_movement_rules_c_header_test.c
    )
    target_include_directories(unit_movement_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_movement_rules_c_header COMMAND unit_movement_rules_c_header_tests)

    add_executable(unit_production_cost_rules_tests
        src/unit_production_cost_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_production_cost_rules_test.cpp
    )
    target_include_directories(unit_production_cost_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_production_cost_rules_contract
        COMMAND unit_production_cost_rules_tests)

    add_executable(unit_production_cost_rules_c_header_tests
        src/unit_production_cost_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_production_cost_rules_c_header_test.c
    )
    target_include_directories(unit_production_cost_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_production_cost_rules_c_header
        COMMAND unit_production_cost_rules_c_header_tests)

    add_executable(unit_combat_rules_tests
        src/unit_combat_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_combat_rules_test.cpp
    )
    target_include_directories(unit_combat_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_combat_rules_contract COMMAND unit_combat_rules_tests)

    add_executable(unit_combat_rules_c_header_tests
        src/unit_combat_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_combat_rules_c_header_test.c
    )
    target_include_directories(unit_combat_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_combat_rules_c_header COMMAND unit_combat_rules_c_header_tests)

    add_executable(unit_effect_rules_tests
        src/unit_effect_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_effect_rules_test.cpp
    )
    target_include_directories(unit_effect_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_effect_rules_contract COMMAND unit_effect_rules_tests)

    add_executable(unit_effect_rules_c_header_tests
        src/unit_effect_rules.cpp
        src/unit_catalog_api.cpp
        tests/unit_effect_rules_c_header_test.c
    )
    target_include_directories(unit_effect_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME unit_effect_rules_c_header COMMAND unit_effect_rules_c_header_tests)

    add_executable(terrain_yield_rules_tests
        src/terrain_yield_rules.cpp
        src/terrain_yield_rules_hooks.cpp
        tests/terrain_yield_rules_test.cpp
    )
    target_include_directories(terrain_yield_rules_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(terrain_yield_rules_tests PRIVATE rex::runtime)
    add_test(NAME terrain_yield_rules_contract COMMAND terrain_yield_rules_tests)

    add_executable(terrain_yield_rules_c_header_tests
        src/terrain_yield_rules.cpp
        tests/terrain_yield_rules_c_header_test.c
    )
    target_include_directories(terrain_yield_rules_c_header_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME terrain_yield_rules_c_header COMMAND terrain_yield_rules_c_header_tests)

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

    add_executable(native_draw_replay_tests
        src/gpu/d3d12/native_draw_replay.cpp
        tests/gpu/native_draw_replay_test.cpp
    )
    target_include_directories(native_draw_replay_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(native_draw_replay_tests PRIVATE rex::runtime)
    add_test(NAME native_draw_replay_recipe COMMAND native_draw_replay_tests)

    add_executable(native_frame_replay_tests
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_frame_replay.cpp
        tests/gpu/native_frame_replay_test.cpp
    )
    target_include_directories(native_frame_replay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_frame_replay_tests PRIVATE rex::runtime)
    add_test(NAME native_frame_replay_recipe COMMAND native_frame_replay_tests)

    add_executable(native_menu_panel_tests
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_menu_panel.cpp
        tests/gpu/native_menu_panel_test.cpp
    )
    target_include_directories(native_menu_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_menu_panel_tests PRIVATE rex::runtime)
    add_test(NAME native_menu_panel COMMAND native_menu_panel_tests)

    add_executable(native_menu_draw_tests
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_frame_replay.cpp
        src/gpu/d3d12/native_menu_frame.cpp
        src/gpu/d3d12/native_menu_draw.cpp
        tests/gpu/native_menu_draw_test.cpp
    )
    target_include_directories(native_menu_draw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_menu_draw_tests PRIVATE rex::runtime)
    add_test(NAME native_menu_draw COMMAND native_menu_draw_tests)

    add_executable(native_texture_upload_tests
        src/gpu/d3d12/native_texture_upload.cpp
        tests/gpu/native_texture_upload_test.cpp
    )
    target_include_directories(native_texture_upload_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_texture_upload_tests PRIVATE rex::runtime)
    add_test(NAME native_texture_upload COMMAND native_texture_upload_tests)

    add_executable(native_guest_draw_capture_tests
        tests/gpu/native_guest_draw_capture_test.cpp
        src/gpu/diagnostics/native_guest_draw_capture.cpp
        src/gpu/d3d12/native_texture_upload.cpp
    )
    target_include_directories(native_guest_draw_capture_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_guest_draw_capture_tests PRIVATE rex::runtime)
    add_test(NAME native_guest_draw_capture COMMAND native_guest_draw_capture_tests)

    add_executable(native_guest_menu_draw_tests
        tests/gpu/native_guest_menu_draw_test.cpp
        src/gpu/d3d12/native_guest_menu_draw.cpp
        src/gpu/d3d12/native_menu_draw.cpp
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_texture_upload.cpp
    )
    target_include_directories(native_guest_menu_draw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(native_guest_menu_draw_tests PRIVATE rex::runtime)
    add_test(NAME native_guest_menu_draw COMMAND native_guest_menu_draw_tests)

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
