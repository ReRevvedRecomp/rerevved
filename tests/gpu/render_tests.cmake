# The renderer, presentation, fixture and adapter test sources of rerevved_tests.
# Included from CMakeLists.txt after
# tests/rerevved_tests.cmake, in the same directory scope, so target_sources()
# on rerevved_tests applies. The cases are discovered there: each one was a
# CTest registration and keeps its name, and the [native_gpu], [run_serial] and
# [timeout-N] tags in the sources replace the lock and timeout properties.
# Fixture emitters and renderer proofs run as `rerevved_tests --helper <name>`.
# The Prepared format matrices and their Original-versus-native proofs belong to
# the optional rerevved_prepared_tests executable, which this file includes.

if(BUILD_TESTING)
    # Sources that compile on every platform. The Windows fixture core object
    # library (CMakeLists.txt) already holds the replay, solid-draw and renderer
    # sources, so other platforms add those directly, plus the ticket Export that
    # the renderer source would define.
    target_sources(rerevved_tests PRIVATE
        tests/support/manual_cases.cpp
        tests/support/native_gameplay_fixture_inputs.cpp
        tests/support/native_gameplay_fixture_snapshot.cpp

        src/gpu/renderer_backend.cpp
        src/gpu/completion/completion_ledger.cpp
        src/gpu/guest_d3d/guest_resolve_encoder.cpp
        src/gpu/guest_d3d/native_draw_dispatch.cpp
        src/gpu/guest_d3d/native_frame_assembler.cpp
        src/gpu/d3d12/native_guest_menu_draw.cpp
        src/gpu/d3d12/native_menu_frame.cpp

        tests/gpu/label_draw_inputs_test.cpp
        tests/gpu/native_app_route_test.cpp
        tests/gpu/native_draw_replay_test.cpp
        tests/gpu/native_frame_assembler_test.cpp
        tests/gpu/native_frame_replay_test.cpp
        tests/gpu/native_gameplay_draw_test.cpp
        tests/gpu/native_gameplay_frame_test.cpp
        tests/gpu/native_gameplay_input_test.cpp
        tests/gpu/native_gameplay_fixture_inputs_test.cpp
        tests/gpu/native_gameplay_render_frame_test.cpp
        tests/gpu/native_gameplay_shaders_test.cpp
        tests/gpu/native_guest_menu_draw_test.cpp
        tests/gpu/native_menu_draw_test.cpp
        tests/gpu/native_menu_panel_test.cpp
        tests/gpu/native_presentation_test.cpp
        tests/gpu/native_texture_upload_test.cpp
        tests/gpu/renderer_backend_test.cpp
        tests/gpu/shader_resource_literals_test.cpp
        tests/gpu/solid_draw_adapter_test.cpp
        tests/gpu/solid_draw_reference_test.cpp
        tests/gpu/solid_draw_samples_test.cpp
    )
    if(REREVVED_DEVELOPER)
        target_sources(rerevved_tests PRIVATE
            src/gpu/guest_d3d/draw_effect_probe.cpp
            src/gpu/present/native_cadence_trace.cpp
            src/gpu/diagnostics/scene_resource_decode.cpp
            src/gpu/diagnostics/shader_harvest.cpp
            tests/gpu/native_cadence_trace_test.cpp
            tests/gpu/shader_harvest_test.cpp
        )
    endif()
    if(NOT WIN32)
        target_sources(rerevved_tests PRIVATE
            tests/support/native_render_frame_ticket_export.cpp
            src/gpu/present/native_output_lease.cpp
            src/gpu/d3d12/native_draw_replay.cpp
            src/gpu/d3d12/native_frame_replay.cpp
            src/gpu/d3d12/native_gameplay_clear.cpp
            src/gpu/d3d12/native_gameplay_draw.cpp
            src/gpu/d3d12/native_gameplay_frame.cpp
            src/gpu/d3d12/native_gameplay_render_frame.cpp
            src/gpu/d3d12/native_gameplay_shaders.cpp
            src/gpu/d3d12/native_menu_draw.cpp
            src/gpu/d3d12/native_menu_panel.cpp
            src/gpu/d3d12/native_render_frame.cpp
            src/gpu/d3d12/native_texture_upload.cpp
            src/gpu/guest_d3d/solid_draw_adapter.cpp
            src/gpu/guest_d3d/solid_draw_reference.cpp
            src/gpu/guest_d3d/solid_draw_samples.cpp
            tools/gameplay_input.cpp
        )
    endif()

    target_include_directories(rerevved_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(rerevved_tests PRIVATE rerevved_guest_d3d_adapters)

    # Fixture builders of the Prepared families. The smoke cases below and the
    # optional research executable (tests/gpu/prepared_tests.cmake) compile them.
    set(REREVVED_PREPARED_FIXTURE_SOURCES
        tests/gpu/native_pixel_lod_fixture.cpp
        tests/gpu/native_prepared_bc1_fixture.cpp
        tests/gpu/native_prepared_cube_matrix_fixture.cpp
        tests/gpu/native_prepared_float_matrix_fixture.cpp
    )

    if(WIN32)
        target_sources(rerevved_tests PRIVATE
            ${REREVVED_PREPARED_FIXTURE_SOURCES}
            src/gpu/present/native_immediate_drawer.cpp
            src/gpu/present/native_presentation.cpp
            src/gpu/present/plume_host.cpp

            tests/gpu/gfx_draw_adapter_test.cpp
            tests/gpu/label_draw_adapter_test.cpp
            tests/gpu/leader_draw_adapter_test.cpp
            tests/gpu/movie_draw_adapter_test.cpp
            tests/gpu/native_depth_only_frame_test.cpp
            tests/gpu/native_frame_assembler_d3d12_test.cpp
            tests/gpu/native_frame_stream_test.cpp
            tests/gpu/native_format_smoke_fixtures.cpp
            tests/gpu/native_format_smoke_test.cpp
            tests/gpu/native_presentation_d3d12_test.cpp
            tests/gpu/native_render_frame_mrt_test.cpp
            tests/gpu/native_upload_pool_test.cpp
            tests/gpu/native_vertex_residency_test.cpp
            tests/gpu/scene_draw_adapter_test.cpp
            tests/gpu/scene_mesh_draw_adapter_test.cpp
        )
        target_include_directories(rerevved_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/generated
        )
        target_link_libraries(rerevved_tests PRIVATE
            rerevved_native_fixture_core
            d3d12 dxgi dxguid d3dcompiler user32 plume
        )
        add_dependencies(rerevved_tests rerevved_present_shaders)
        # Test discovery runs rerevved_tests as soon as it links, so stage its
        # DLLs in a prerequisite rather than a later POST_BUILD step; staging
        # first also leaves other targets' copies of them nothing to rewrite.
        # TARGET_RUNTIME_DLLS omits the runtime-loaded original GPU plugin.
        add_custom_target(rerevved_tests_runtime
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:rerevved_tests>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:rerevved_tests>;${CMAKE_CURRENT_BINARY_DIR}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::gpu-xenos>
                ${CMAKE_CURRENT_BINARY_DIR}
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_dependencies(rerevved_tests rerevved_tests_runtime)
    endif()

    include(tests/gpu/native_app_smoke.cmake)
    include(tests/gpu/prepared_tests.cmake)

    # Targets that copy the same DLLs into this directory wait for staging,
    # so no two copies of one file run at once.
    if(TARGET rerevved_tests_runtime)
        foreach(_copier IN ITEMS rerevved rerevved_native_app_smoke rerevved_prepared_tests
                                 native_draw_replay native_vulkan_draw_compare)
            if(TARGET ${_copier})
                add_dependencies(${_copier} rerevved_tests_runtime)
            endif()
        endforeach()
    endif()
endif()
