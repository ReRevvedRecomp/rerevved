cmake_minimum_required(VERSION 3.25)
project(rerevved VERSION 0.5.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Embed source-relative paths in __FILE__ and debug info, not the build host's.
add_compile_options(-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=)

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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(REREVVED_SDK_PLATFORM "mac-amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(REREVVED_SDK_PLATFORM "mac-arm64")
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

if(WIN32)
    # Plume is the title-owned host layer for native presentation. Its D3D12
    # backend runs on the system runtime and its Vulkan backend builds with it.
    set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "Build Plume example applications" FORCE)
    set(PLUME_D3D12_AGILITY_SDK_ENABLED OFF CACHE BOOL "Enable the D3D12 Agility SDK in Plume" FORCE)
    add_subdirectory(thirdparty/plume)

    # Prefer the newest Windows SDK dxc to PATH tools; a Vulkan SDK dxc may be
    # unable to sign DXIL. A compiler outside the Windows SDK needs dxil.dll.
    if(NOT REREVVED_DXC)
        file(GLOB _rerevved_sdk_dxc "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/dxc.exe")
        list(SORT _rerevved_sdk_dxc COMPARE NATURAL ORDER DESCENDING)
        if(_rerevved_sdk_dxc)
            list(GET _rerevved_sdk_dxc 0 _rerevved_dxc_newest)
            set(REREVVED_DXC "${_rerevved_dxc_newest}" CACHE FILEPATH "DirectX Shader Compiler" FORCE)
        else()
            find_program(REREVVED_DXC NAMES dxc HINTS "$ENV{WindowsSdkVerBinPath}/x64")
        endif()
    endif()
    get_filename_component(_rerevved_dxc_dir "${REREVVED_DXC}" DIRECTORY)
    if(NOT EXISTS "${REREVVED_DXC}")
        message(FATAL_ERROR "Presentation shaders require dxc.exe. Install the Windows SDK or set REREVVED_DXC.")
    endif()
    if(NOT REREVVED_DXC MATCHES "/Windows Kits/" AND NOT EXISTS "${_rerevved_dxc_dir}/dxil.dll")
        message(FATAL_ERROR "A dxc.exe outside the Windows SDK needs dxil.dll beside it to sign DXIL (selected: ${REREVVED_DXC}).")
    endif()
    set(REREVVED_PRESENT_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/present/native_present.hlsl")
    set(REREVVED_PRESENT_SHADER_VERTEX "${CMAKE_CURRENT_BINARY_DIR}/generated/native_present_vertex.h")
    set(REREVVED_PRESENT_SHADER_PIXEL "${CMAKE_CURRENT_BINARY_DIR}/generated/native_present_pixel.h")
    add_custom_command(
        OUTPUT "${REREVVED_PRESENT_SHADER_VERTEX}" "${REREVVED_PRESENT_SHADER_PIXEL}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${REREVVED_DXC}" -T vs_6_0 -E VSMain -O3
            -Fh "${REREVVED_PRESENT_SHADER_VERTEX}" -Vn kNativePresentVertexDxil
            "${REREVVED_PRESENT_SHADER_SOURCE}"
        COMMAND "${REREVVED_DXC}" -T ps_6_0 -E PSMain -O3
            -Fh "${REREVVED_PRESENT_SHADER_PIXEL}" -Vn kNativePresentPixelDxil
            "${REREVVED_PRESENT_SHADER_SOURCE}"
        DEPENDS "${REREVVED_PRESENT_SHADER_SOURCE}"
        COMMENT "Compiling native presentation shaders"
        VERBATIM)
    add_custom_target(rerevved_present_shaders
        DEPENDS "${REREVVED_PRESENT_SHADER_VERTEX}" "${REREVVED_PRESENT_SHADER_PIXEL}"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_vertex.h"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_pixel.h")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_vertex.h"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_pixel.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${REREVVED_DXC}" -T vs_6_0 -E VSMain -O3
            -Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_vertex.h" -Vn kNativeImmediateVertexDxil
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/present/native_immediate.hlsl"
        COMMAND "${REREVVED_DXC}" -T ps_6_0 -E PSMain -O3
            -Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/native_immediate_pixel.h" -Vn kNativeImmediatePixelDxil
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/present/native_immediate.hlsl"
        DEPENDS src/gpu/present/native_immediate.hlsl
        COMMENT "Compiling native overlay shaders"
        VERBATIM)
endif()

option(REREVVED_DEVELOPER "Build the developer diagnostics recorders and replay tools; never packaged" OFF)
if(REREVVED_DEVELOPER)
    add_compile_definitions(REREVVED_DEVELOPER=1)
else()
    add_compile_definitions(REREVVED_DEVELOPER=0)
endif()

set(REREVVED_SOURCES
    src/app.cpp
    src/rerevved_hooks.cpp
    src/xenos_frame_counter.cpp
    src/xenos_frame_counter.h
    src/game_content.cpp
    src/great_general_attachment.cpp
    src/great_general_attachment.h
    src/gameplay_state.cpp
    src/gameplay_state.h
    src/display_aspect.cpp
    src/display_aspect.h
    src/display_aspect_hooks.cpp
    src/display_aspect_hooks.h
    src/gpu/guest_gpu_service.cpp
    src/gpu/guest_gpu_service.h
    src/gpu/completion/completion_ledger.cpp
    src/gpu/completion/completion_ledger.h
    src/gpu/completion/guest_progress_hooks.cpp
    src/gpu/completion/guest_progress_hooks.h
    src/gpu/present/native_output_lease.cpp
    src/gpu/present/native_output_lease.h
    src/usb_camera_stubs.cpp
    src/main.cpp
    src/gpu/d3d12/native_renderer_d3d12.cpp
    src/gpu/d3d12/native_render_frame.cpp
    src/gpu/d3d12/native_render_frame.h
    src/gpu/d3d12/native_renderer_d3d12.h
    src/gpu/d3d12/native_resource_pool.h
    src/gpu/d3d12/native_upload_buffer_d3d12.cpp
    src/gpu/d3d12/native_upload_buffer_d3d12.h
    src/gpu/d3d12/native_upload_pool.cpp
    src/gpu/d3d12/native_upload_pool.h
    src/gpu/d3d12/native_completion_source.cpp
    src/gpu/d3d12/native_completion_source.h
    src/gpu/d3d12/native_draw_replay.cpp
    src/gpu/d3d12/native_draw_replay.h
    src/gpu/d3d12/native_frame_replay.cpp
    src/gpu/d3d12/native_frame_replay.h
    src/gpu/d3d12/native_gameplay_draw.cpp
    src/gpu/d3d12/native_gameplay_draw.h
    src/gpu/d3d12/native_gameplay_shaders.cpp
    src/gpu/d3d12/native_gameplay_shaders.h
    src/gpu/d3d12/native_gameplay_frame.cpp
    src/gpu/d3d12/native_gameplay_frame.h
    src/gpu/d3d12/native_gameplay_render_frame.cpp
    src/gpu/d3d12/native_gameplay_clear.cpp
    src/gpu/d3d12/native_gameplay_render_frame.h
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
    src/gpu/guest_d3d/developer_hooks.h
    src/gpu/guest_d3d/guest_api_entries.inc
    src/gpu/guest_d3d/guest_api_hooks.cpp
    src/gpu/guest_d3d/route_entries.inc
    src/gpu/guest_d3d/route_hooks.cpp
    src/gpu/guest_d3d/guest_render_capture.h
    src/gpu/guest_d3d/guest_resource_read.h
    src/gpu/guest_d3d/draw_hooks.cpp
    src/gpu/guest_d3d/guest_memory.cpp
    src/gpu/guest_d3d/guest_memory.h
    src/gpu/guest_d3d/guest_resolve_encoder.cpp
    src/gpu/guest_d3d/guest_resolve_encoder.h
    src/gpu/guest_d3d/clear_consume.cpp
    src/gpu/guest_d3d/clear_derive.cpp
    src/gpu/guest_d3d/gamma_observer.cpp
    src/gpu/guest_d3d/clear_consume.h
    src/gpu/guest_d3d/clear_derive.h
    src/gpu/guest_d3d/gamma_observer.h
    src/gpu/guest_d3d/completion_probe_hooks.cpp
    src/gpu/guest_d3d/resolve_consume.cpp
    src/gpu/guest_d3d/resolve_derive.cpp
    src/gpu/guest_d3d/resolve_consume.h
    src/gpu/guest_d3d/resolve_derive.h
    src/gpu/guest_d3d/set_predication_consume.cpp
    src/gpu/guest_d3d/set_predication_derive.cpp
    src/gpu/guest_d3d/set_predication_consume.h
    src/gpu/guest_d3d/set_predication_derive.h
    src/gpu/guest_d3d/stack_guard.h
    src/gpu/guest_d3d/indexed_buffer_consume.cpp
    src/gpu/guest_d3d/indexed_buffer_gate.cpp
    src/gpu/guest_d3d/lowerer_consume.cpp
    src/gpu/guest_d3d/lowerer_derive.cpp
    src/gpu/guest_d3d/user_pointer_consume.cpp
    src/gpu/guest_d3d/window_writer_consume.cpp
    src/gpu/guest_d3d/window_writer_derive.cpp
    src/gpu/guest_d3d/window_writer_hooks.cpp
    src/gpu/guest_d3d/native_draw_dispatch.cpp
    src/gpu/guest_d3d/indexed_buffer_consume.h
    src/gpu/guest_d3d/indexed_buffer_gate.h
    src/gpu/guest_d3d/lowerer_consume.h
    src/gpu/guest_d3d/lowerer_derive.h
    src/gpu/guest_d3d/user_pointer_consume.h
    src/gpu/guest_d3d/window_writer_consume.h
    src/gpu/guest_d3d/window_writer_derive.h
    src/gpu/guest_d3d/native_draw_dispatch.h
    src/gpu/guest_d3d/native_draw_hook.h
    src/gpu/guest_d3d/native_frame_assembler.cpp
    src/gpu/guest_d3d/native_frame_assembler.h
    src/gpu/guest_d3d/present_hook.cpp
    src/gpu/guest_d3d/guest_resource_observers.h
    src/gpu/guest_d3d/resolve_surface_hooks.cpp
    src/gpu/guest_d3d/resolve_surface_hooks.h
    src/gpu/guest_d3d/resolve_surface_registry.h
    src/gpu/guest_d3d/resource_mirror_hooks.cpp
    src/gpu/guest_d3d/resource_mirror_hooks.h
    src/gpu/guest_d3d/resource_mirror_registry.cpp
    src/gpu/guest_d3d/resource_mirror_registry.h
    src/gpu/guest_d3d/ring_stub_hooks.cpp
    src/gpu/guest_d3d/ring_stubs.cpp
    src/gpu/guest_d3d/ring_stubs.h
    src/gpu/guest_d3d/ring_words.h
    src/gpu/guest_d3d/solid_draw_adapter.cpp
    src/gpu/guest_d3d/solid_draw_adapter.h
    src/gpu/guest_d3d/solid_draw_reference.cpp
    src/gpu/guest_d3d/solid_draw_reference.h
    src/gpu/diagnostics/native_renderer_guest_state.cpp
    src/gpu/diagnostics/native_renderer_guest_state.h
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
    src/rules_bundle.cpp
    src/rules_bundle.h
    src/mod_script_rules.cpp
    src/mod_script_rules.h
    src/building_effect_rules_hooks.cpp
    src/building_effect_rules_registry.h
    src/building_wonder_effect_rules.cpp
    src/wonder_effect_rules_hooks.cpp
    src/wonder_effect_rules_registry.h
    src/discord_invites_model.cpp
    src/discord_invites_model.h
    src/discord_presence.cpp
    src/discord_presence.h
    src/discord_presence_model.cpp
    src/discord_presence_model.h
    src/rush_cost.cpp
    src/rush_cost.h
    src/gameplay_fixes.cpp
    src/gameplay_fixes.h
    src/gameplay_fixes_hooks.cpp
    src/overlay_settings.cpp
    src/overlay_skin.cpp
    src/retail_ui_art.cpp
    src/retail_ui_art.h
    src/system_link_trace.cpp
    src/system_link_trace.h
    src/system_link_trace_hooks.cpp
    src/research_prompt.h
    src/research_prompt_hooks.cpp
    src/advisor_popups.h
    src/advisor_popup_hooks.cpp
    src/barbarian_messages.h
    src/barbarian_message_hooks.cpp
    src/ai_civilizations.h
    src/ai_civilizations_hooks.cpp
    src/host_settings.cpp
    src/host_settings.h
    src/unit_catalog_api.cpp
    src/unit_catalog_api.h
)

# The guest D3D draw families and their readers, built once for the title and
# the adapter tests. Their assembler, converter and texture dependencies come
# from the linking target.
add_library(rerevved_guest_d3d_adapters STATIC
    src/gpu/guest_d3d/label_draw_inputs.cpp
    src/gpu/guest_d3d/label_draw_inputs.h
    src/gpu/guest_d3d/label_draw_adapter.cpp
    src/gpu/guest_d3d/label_draw_adapter.h
    src/gpu/guest_d3d/gfx_draw_inputs.cpp
    src/gpu/guest_d3d/gfx_draw_inputs.h
    src/gpu/guest_d3d/gfx_draw_adapter.cpp
    src/gpu/guest_d3d/gfx_draw_adapter.h
    src/gpu/guest_d3d/indexed_buffer_draw_inputs.cpp
    src/gpu/guest_d3d/indexed_buffer_draw_inputs.h
    src/gpu/guest_d3d/leader_draw_adapter.cpp
    src/gpu/guest_d3d/leader_draw_adapter.h
    src/gpu/guest_d3d/scene_mesh_draw_adapter.cpp
    src/gpu/guest_d3d/scene_mesh_draw_adapter.h
    src/gpu/guest_d3d/shader_resource_literals.cpp
    src/gpu/guest_d3d/shader_resource_literals.h
    src/gpu/guest_d3d/movie_draw_inputs.cpp
    src/gpu/guest_d3d/movie_draw_inputs.h
    src/gpu/guest_d3d/movie_draw_adapter.cpp
    src/gpu/guest_d3d/movie_draw_adapter.h
    src/gpu/guest_d3d/scene_draw_adapter.cpp
    src/gpu/guest_d3d/scene_draw_adapter.h
    src/gpu/guest_d3d/scene_draw_coverage.cpp
    src/gpu/guest_d3d/scene_draw_coverage.h
    src/gpu/guest_d3d/native_draw_families.cpp
    src/gpu/guest_d3d/native_draw_families.h
    src/gpu/guest_d3d/native_draw_shader_words.cpp
    src/gpu/guest_d3d/native_draw_shader_words.h
)
# The vertex shader code probe and watch observe consumed dirty vertex fetch
# draws; only a developer build links them.
if(REREVVED_DEVELOPER)
    target_sources(rerevved_guest_d3d_adapters PRIVATE
        src/gpu/guest_d3d/vs_code_probe.cpp
        src/gpu/guest_d3d/vs_code_probe.h
        src/gpu/guest_d3d/vs_code_watch.cpp
        src/gpu/guest_d3d/vs_code_watch.h
    )
endif()
target_include_directories(rerevved_guest_d3d_adapters PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rerevved_guest_d3d_adapters PRIVATE rex::runtime)
rexglue_apply_target_settings(rerevved_guest_d3d_adapters)

if(WIN32)
    set(REREVVED_RC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/rerevved.ico")
    configure_file(res/rerevved.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc" @ONLY)
    list(APPEND REREVVED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc")
    list(APPEND REREVVED_SOURCES
        src/gpu/present/native_app_session.cpp
        tools/presentation_fixture.cpp
        src/gpu/present/native_presentation.cpp
        src/gpu/present/native_immediate_drawer.cpp
        src/gpu/present/native_presentation.h
        src/gpu/present/plume_host.cpp
        src/gpu/present/plume_host.h
    )
endif()

list(APPEND REREVVED_SOURCES
    src/raging_barbarians_settings.cpp
    src/raging_barbarians_settings.h
    src/raging_barbarians_hooks.cpp
    src/raging_barbarians_hooks.h
    src/occ_settings.cpp
    src/occ_settings.h
    src/occ_hooks.cpp
    src/occ_hooks.h
    src/city_founding_unit_rules.cpp
    src/city_founding_unit_rules.h
    src/city_founding_unit_hooks.cpp
    src/city_founding_unit_hooks.h
    src/gotw_settings.cpp
    src/gotw_settings.h
    src/gotw_hooks.cpp
    src/gotw_hooks.h
    src/save_sidecar.h
    src/save_sidecar.cpp
    src/save_sidecar_hooks.cpp
    src/resource_settings.h
    src/resource_settings.cpp
    src/start_resources.h
    src/start_resources.cpp
    src/start_resources_runtime.h
    src/resource_identity_overrides.h
    src/resource_identity_overrides.cpp
    src/resource_identity_trace.h
    src/resource_identity_trace.cpp
    src/resource_identity_hooks.cpp
    src/start_resources_runtime.cpp
)

# Developer recorders: scene resource, payload and target capture, route and
# effect traces, shader harvest, Xenos frame reference, the guest API census and
# the completion, original residue, tiling and vertex shader readback probes,
# and the default-off tiling restore and video import replacements. The
# shipping binary links none of them.
if(REREVVED_DEVELOPER)
    list(APPEND REREVVED_SOURCES
        src/gpu/guest_d3d/completion_probe.cpp
        src/gpu/guest_d3d/completion_probe.h
        src/gpu/guest_d3d/original_residue_probe.cpp
        src/gpu/guest_d3d/original_residue_probe.h
        src/gpu/guest_d3d/tiling_probe.cpp
        src/gpu/guest_d3d/tiling_probe.h
        src/gpu/guest_d3d/tiling_restore_consume.cpp
        src/gpu/guest_d3d/tiling_restore_consume.h
        src/gpu/guest_d3d/video_display_mode_stub.cpp
        src/gpu/guest_d3d/video_display_mode_stub.h
        src/gpu/guest_d3d/video_import_stub_table.cpp
        src/gpu/guest_d3d/video_import_stub_table.h
        src/gpu/guest_d3d/video_scaler_stub.cpp
        src/gpu/guest_d3d/video_scaler_stub.h
        src/gpu/guest_d3d/vs_readback_probe.cpp
        src/gpu/guest_d3d/vs_readback_probe.h
        src/gpu/guest_d3d/vs_readback_probe_hooks.cpp
        src/gpu/diagnostics/draw_capture_frame_inputs.cpp
        src/gpu/diagnostics/draw_capture_frame_inputs.h
        src/gpu/guest_d3d/draw_effect_probe.cpp
        src/gpu/guest_d3d/draw_effect_probe.h
        src/gpu/present/native_cadence_trace.cpp
        src/gpu/present/native_cadence_trace.h
        src/gpu/diagnostics/label_draw_capture.cpp
        src/gpu/diagnostics/label_draw_capture.h
        src/gpu/diagnostics/route_effect_trace.cpp
        src/gpu/diagnostics/route_effect_trace.h
        src/gpu/diagnostics/route_trace.cpp
        src/gpu/diagnostics/route_trace.h
        src/gpu/diagnostics/route_trace_calls.inc
        src/gpu/diagnostics/scene_payload_store.cpp
        src/gpu/diagnostics/scene_payload_store.h
        src/gpu/diagnostics/scene_resource_capture.cpp
        src/gpu/diagnostics/scene_resource_capture.h
        src/gpu/diagnostics/scene_resource_decode.cpp
        src/gpu/diagnostics/scene_target_readback.cpp
        src/gpu/diagnostics/scene_target_readback.h
        src/gpu/diagnostics/shader_harvest.cpp
        src/gpu/diagnostics/shader_harvest.h
        src/gpu/diagnostics/shader_harvest_hook.h
        src/gpu/diagnostics/xenos_frame_reference.cpp
        src/gpu/diagnostics/xenos_frame_reference.h
        src/gpu/guest_d3d/guest_api_census.cpp
        src/gpu/guest_d3d/guest_api_census.h
    )
endif()

if(WIN32)
    add_executable(rerevved WIN32 ${REREVVED_SOURCES})
else()
    add_executable(rerevved ${REREVVED_SOURCES})
endif()

target_link_libraries(rerevved PRIVATE rerevved_guest_d3d_adapters)

if(WIN32)
    target_link_libraries(rerevved PRIVATE d3d12 dxgi dxguid d3dcompiler)
    target_link_libraries(rerevved PRIVATE plume)
    add_dependencies(rerevved rerevved_present_shaders)
endif()

if(WIN32)
    # The replay sources the tool, the pixel fixtures and the adapter tests all
    # compile with one command, built once as objects. Targets that need other
    # include directories or options list these sources themselves.
    set(REREVVED_NATIVE_FIXTURE_SOURCES
        tools/gameplay_input.cpp
        tools/gameplay_input.h
        src/gpu/d3d12/native_gameplay_frame.cpp
        src/gpu/d3d12/native_gameplay_draw.cpp
        src/gpu/d3d12/native_gameplay_shaders.cpp
        src/gpu/d3d12/native_gameplay_render_frame.cpp
        src/gpu/d3d12/native_gameplay_clear.cpp
        src/gpu/d3d12/native_menu_draw.cpp
        src/gpu/guest_d3d/solid_draw_adapter.cpp
        src/gpu/guest_d3d/solid_draw_reference.cpp
        src/gpu/d3d12/native_menu_panel.cpp
        src/gpu/guest_d3d/solid_draw_samples.cpp
        tools/solid_adapter.cpp
        tools/solid_adapter.h
        src/gpu/d3d12/native_renderer_d3d12.cpp
        src/gpu/d3d12/native_render_frame.cpp
        src/gpu/d3d12/native_render_frame.h
        src/gpu/d3d12/native_renderer_d3d12.h
        src/gpu/d3d12/native_resource_pool.h
        src/gpu/d3d12/native_upload_buffer_d3d12.cpp
        src/gpu/d3d12/native_upload_buffer_d3d12.h
        src/gpu/d3d12/native_upload_pool.cpp
        src/gpu/d3d12/native_upload_pool.h
        src/gpu/d3d12/native_draw_replay.cpp
        src/gpu/d3d12/native_draw_replay.h
        src/gpu/d3d12/native_frame_replay.cpp
        src/gpu/d3d12/native_frame_replay.h
        src/gpu/d3d12/native_texture_upload.cpp
        src/gpu/d3d12/native_texture_upload.h
        src/gpu/diagnostics/native_renderer_guest_state.cpp
        src/gpu/diagnostics/native_renderer_guest_state.h
        src/gpu/present/native_output_lease.cpp
        src/gpu/present/native_output_lease.h
    )
    add_library(rerevved_native_fixture_core OBJECT ${REREVVED_NATIVE_FIXTURE_SOURCES})
    target_include_directories(rerevved_native_fixture_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(rerevved_native_fixture_core PRIVATE
        rex::runtime
        d3d12
        dxgi
        dxguid
        d3dcompiler
    )
    rexglue_apply_target_settings(rerevved_native_fixture_core)

    # The replay tools are developer-only; the fixture core above stays because
    # the adapter tests link it.
    if(REREVVED_DEVELOPER)
        add_executable(native_draw_replay
            tools/main.cpp
            tools/sequence.cpp
            tools/sequence.h
        )
        target_include_directories(native_draw_replay PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(native_draw_replay PRIVATE
            rerevved_native_fixture_core
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
        set(REREVVED_VULKAN_INCLUDE_DIR "$ENV{REREVVED_VULKAN_INCLUDE_DIR}" CACHE PATH "Vulkan header include directory for the offline draw comparison")
        if(REREVVED_VULKAN_INCLUDE_DIR)
            if(NOT EXISTS "${REREVVED_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
                message(FATAL_ERROR "REREVVED_VULKAN_INCLUDE_DIR must contain vulkan/vulkan.h")
            endif()
            add_executable(native_vulkan_draw_compare
                ${REREVVED_NATIVE_FIXTURE_SOURCES}
                tools/vulkan_compare.cpp
                tools/vulkan_draw.cpp
                tools/vulkan_draw.h
            )
            target_include_directories(native_vulkan_draw_compare PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${REREVVED_VULKAN_INCLUDE_DIR}
            )
            target_link_libraries(native_vulkan_draw_compare PRIVATE rex::runtime d3d12 dxgi dxguid d3dcompiler)
            rexglue_apply_target_settings(native_vulkan_draw_compare)
            add_custom_command(TARGET native_vulkan_draw_compare POST_BUILD
                COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:native_vulkan_draw_compare>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:native_vulkan_draw_compare>;$<TARGET_FILE_DIR:native_vulkan_draw_compare>>"
                COMMAND_EXPAND_LISTS
                VERBATIM
            )
        endif()
    endif()
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
    BUILDING_EFFECT_RULES_API_EXPORTS
    WONDER_EFFECT_RULES_API_EXPORTS
    UNIT_CATALOG_API_EXPORTS
    REREVVED_VERSION="${PROJECT_VERSION}"
)
if(APPLE)
    target_link_options(rerevved PRIVATE -Wl,-export_dynamic -Wl,-headerpad_max_install_names)
elseif(UNIX)
    target_link_options(rerevved PRIVATE -Wl,--export-dynamic)
endif()
# Asset-free builds compile the standalone tests without codegen input; the
# title executable needs generated guest code, so it leaves the build.
option(REREVVED_TESTS_ONLY "Build only the standalone tests, without codegen" OFF)
if(REREVVED_TESTS_ONLY)
    set_target_properties(rerevved PROPERTIES EXCLUDE_FROM_ALL ON)
else()
    rexglue_setup_recompiled_target(rerevved
        MANIFEST rerevved_manifest.toml
        OUTPUT_DIR generated/default
        GPU_PLUGINS xenos)
endif()

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
set(REREVVED_NATIVE_PRESENT_CATALOG "" CACHE PATH "Admitted native presentation shader catalog")
include(CTest)
if(BUILD_TESTING)
    # Catch2 v3 builds the unified native test executable. Only its library
    # target is wanted: no install rules, docs, extras or its own tests.
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Install Catch2 documentation" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Install Catch2 extras" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "Build Catch2 tests and warnings" FORCE)
    add_subdirectory(thirdparty/catch2 EXCLUDE_FROM_ALL)
    # The title's own copy, not a module path an SDK or other project added.
    include("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/catch2/extras/Catch.cmake")

    # include() rather than add_subdirectory(): the test targets and their
    # registrations keep this directory's scope.
    include(tests/rerevved_tests.cmake)
    include(tests/gpu/render_tests.cmake)
    include(tests/gpu/hook_tests.cmake)
endif()

# Shipping bridges for the VdInitializeScalerCommandBuffer and VdSetDisplayMode
# import hooks; the stubs and their observations are developer-only.
target_sources(rerevved PRIVATE
    src/gpu/guest_d3d/video_scaler_hooks.cpp
    src/gpu/guest_d3d/video_display_mode_hooks.cpp)

# Preflight-then-commit decision of the VdSwap call-site replacement.
target_sources(rerevved PRIVATE
    src/gpu/guest_d3d/present_swap.cpp
    src/gpu/guest_d3d/present_swap.h)

# Shipping bridge for the VdIsHSIOTrainingSucceeded and VdRetrainEDRAMWorker
# import hooks; the stub table and its observations are developer-only.
target_sources(rerevved PRIVATE
    src/gpu/guest_d3d/video_import_stub_hooks.cpp)
