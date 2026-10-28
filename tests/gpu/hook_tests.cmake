# The hook, state, completion and recorder tests of rerevved_tests, the recorder
# analysis registrations and the production variants that keep their test seams
# out of the title. Included from CMakeLists.txt after tests/rerevved_tests.cmake
# and tests/gpu/render_tests.cmake, in the same directory scope, so
# target_sources() on rerevved_tests applies. Each case was a CTest registration
# and keeps its name; the [timeout-N], [run_serial] and [native_gpu] tags in the
# sources replace the properties. Fatal children and recorder modes run as
# `rerevved_tests --helper <name>`.

if(BUILD_TESTING)
    if(REREVVED_DEVELOPER)
        # Production sources whose test seams, or whose symbols a test must
        # observe, compile once here under renamed production names, apart from
        # the title's copy. tests/support/production_observers.cpp defines the
        # production names and forwards to the Real<Name> versions unless a case
        # installs an observer. REREVVED_SCENE_CAPTURE_TEST adds the recorder
        # test seam. The inline ObserveSceneEntry in scene_resource_capture.h
        # calls BeginSceneEntry, so it is renamed with it: every other
        # translation unit keeps the one real body.
        add_library(rerevved_test_capture_variants OBJECT
            tests/support/scene_capture_variant.cpp
            src/gpu/diagnostics/label_draw_capture.cpp
        )
        target_include_directories(rerevved_test_capture_variants PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
        )
        target_compile_definitions(rerevved_test_capture_variants PRIVATE
            REREVVED_SCENE_CAPTURE_TEST=1
            BeginSceneEntry=RealBeginSceneEntry
            ObserveSceneEntry=RealObserveSceneEntry
            EndSceneEntry=RealEndSceneEntry
            BeginLabelAdapterDraw=RealBeginLabelAdapterDraw
            FinishLabelAdapterDraw=RealFinishLabelAdapterDraw
            ObserveLabelAdapterPatch=RealObserveLabelAdapterPatch
            ObserveLabelAdapterSelection=RealObserveLabelAdapterSelection
        )
        target_link_libraries(rerevved_test_capture_variants PRIVATE rex::runtime Threads::Threads)
        rexglue_apply_target_settings(rerevved_test_capture_variants)
    endif()

    # The present swap test supplies the ring stub report and guest memory the
    # hook adapter reaches for. Those names belong to the real ring stub hooks in
    # this executable, so the adapter and its test compile with them renamed.
    add_library(rerevved_test_present_variant OBJECT
        src/gpu/guest_d3d/present_hook.cpp
    )
    target_include_directories(rerevved_test_present_variant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(rerevved_test_present_variant PRIVATE
        NoteRingStub=NoteRingStubPresentFixture
        GuestRingMemory=GuestRingMemoryPresentFixture
    )
    target_link_libraries(rerevved_test_present_variant PRIVATE rex::runtime Threads::Threads)
    rexglue_apply_target_settings(rerevved_test_present_variant)
    set_source_files_properties(tests/gpu/present_swap_test.cpp
        PROPERTIES COMPILE_DEFINITIONS "NoteRingStub=NoteRingStubPresentFixture;GuestRingMemory=GuestRingMemoryPresentFixture")
    # The recorder tests drive the capture session through its test seam.
    if(REREVVED_DEVELOPER)
        set_source_files_properties(
            tests/gpu/scene_resource_test.cpp
            tests/gpu/xenos_frame_reference_test.cpp
            PROPERTIES COMPILE_DEFINITIONS REREVVED_SCENE_CAPTURE_TEST=1)
    endif()

    target_sources(rerevved_tests PRIVATE
        tests/support/cvar_isolation.cpp

        # This pure DTO persistence helper has no live recorder integration.
        src/gpu/diagnostics/draw_capture_frame_inputs.cpp
        src/gpu/completion/guest_progress_hooks.cpp
        src/gpu/guest_d3d/clear_consume.cpp
        src/gpu/guest_d3d/clear_derive.cpp
        src/gpu/guest_d3d/completion_probe_hooks.cpp
        src/gpu/guest_d3d/draw_hooks.cpp
        src/gpu/guest_d3d/gamma_observer.cpp
        src/gpu/guest_d3d/guest_api_hooks.cpp
        src/gpu/guest_d3d/guest_memory.cpp
        src/gpu/guest_d3d/present_swap.cpp
        src/gpu/guest_d3d/resolve_consume.cpp
        src/gpu/guest_d3d/resolve_surface_hooks.cpp
        src/gpu/guest_d3d/resource_mirror_hooks.cpp
        src/gpu/guest_d3d/ring_stub_hooks.cpp
        src/gpu/guest_d3d/route_hooks.cpp
        src/gpu/guest_d3d/set_predication_consume.cpp
        src/gpu/guest_d3d/set_predication_derive.cpp
        src/gpu/guest_d3d/video_display_mode_hooks.cpp
        src/gpu/guest_d3d/video_import_stub_hooks.cpp
        src/gpu/guest_d3d/video_scaler_hooks.cpp
        src/gpu/guest_d3d/window_writer_consume.cpp
        src/gpu/guest_d3d/window_writer_derive.cpp
        src/gpu/guest_gpu_service.cpp
        src/gpu/guest_d3d/indexed_buffer_consume.cpp
        src/gpu/guest_d3d/indexed_buffer_gate.cpp
        src/gpu/guest_d3d/lowerer_consume.cpp
        src/gpu/guest_d3d/lowerer_derive.cpp
        src/gpu/guest_d3d/resolve_derive.cpp
        src/gpu/guest_d3d/resource_mirror_registry.cpp
        src/gpu/guest_d3d/ring_stubs.cpp
        src/gpu/guest_d3d/user_pointer_consume.cpp
        src/gpu/d3d12/native_completion_source.cpp

        tests/gpu/clear_consume_test.cpp
        tests/gpu/clear_derive_test.cpp
        tests/gpu/clear_effect_test.cpp
        tests/gpu/completion_ledger_test.cpp
        tests/gpu/diagnostics/native_renderer_guest_state_test.cpp
        tests/gpu/draw_hooks_test.cpp
        tests/gpu/draw_capture_frame_inputs_test.cpp
        tests/gpu/gamma_observer_test.cpp
        tests/gpu/guest_gpu_service_test.cpp
        tests/gpu/guest_memory_test.cpp
        tests/gpu/guest_resolve_encoder_test.cpp
        tests/gpu/lowerer_derive_test.cpp
        tests/gpu/native_completion_source_test.cpp
        tests/gpu/native_draw_dispatch_test.cpp
        tests/gpu/present_swap_test.cpp
        tests/gpu/resolve_consume_test.cpp
        tests/gpu/resolve_derive_test.cpp
        tests/gpu/resolve_effect_test.cpp
        tests/gpu/resolve_surface_registry_test.cpp
        tests/gpu/resource_mirror_registry_test.cpp
        tests/gpu/ring_stub_route_test.cpp
        tests/gpu/ring_stubs_test.cpp
        tests/gpu/set_predication_test.cpp
        tests/gpu/stack_guard_test.cpp
        tests/gpu/window_writer_consume_test.cpp
        tests/gpu/window_writer_derive_test.cpp
    )
    if(NOT WIN32)
        # The Windows fixture core object library holds this source.
        target_sources(rerevved_tests PRIVATE src/gpu/diagnostics/native_renderer_guest_state.cpp)
    endif()
    target_include_directories(rerevved_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rerevved_tests PRIVATE rerevved_test_present_variant)

    # Live recorder and probe integrations, their test seams and cases exist
    # only in a developer build.
    if(REREVVED_DEVELOPER)
        target_sources(rerevved_tests PRIVATE
            tests/support/original_residue_probe_variant.cpp
            tests/support/production_observers.cpp
            tests/support/route_effect_variant.cpp
            tests/support/route_trace_variant.cpp
            tests/support/xenos_frame_reference_variant.cpp
            src/gpu/diagnostics/scene_payload_store.cpp
            src/gpu/diagnostics/scene_target_readback.cpp
            src/gpu/guest_d3d/completion_probe.cpp
            src/gpu/guest_d3d/guest_api_census.cpp
            src/gpu/guest_d3d/tiling_probe.cpp
            src/gpu/guest_d3d/tiling_restore_consume.cpp
            src/gpu/guest_d3d/video_display_mode_stub.cpp
            src/gpu/guest_d3d/video_import_stub_table.cpp
            src/gpu/guest_d3d/video_scaler_stub.cpp
            src/gpu/guest_d3d/vs_readback_probe.cpp
            src/gpu/guest_d3d/vs_readback_probe_hooks.cpp

            tests/gpu/completion_probe_test.cpp
            tests/gpu/draw_effect_probe_test.cpp
            tests/gpu/guest_api_census_test.cpp
            tests/gpu/original_residue_probe_test.cpp
            tests/gpu/ring_consume_monitor_test.cpp
            tests/gpu/route_effect_trace_test.cpp
            tests/gpu/route_trace_test.cpp
            tests/gpu/scene_payload_store_test.cpp
            tests/gpu/scene_resource_hooks_test.cpp
            tests/gpu/scene_resource_test.cpp
            tests/gpu/tiling_probe_test.cpp
            tests/gpu/tiling_restore_test.cpp
            tests/gpu/video_display_mode_stub_test.cpp
            tests/gpu/video_import_stub_table_test.cpp
            tests/gpu/video_scaler_stub_test.cpp
            tests/gpu/vs_readback_probe_test.cpp
            tests/gpu/xenos_frame_reference_test.cpp
        )
        target_link_libraries(rerevved_tests PRIVATE rerevved_test_capture_variants)
    endif()

    if(REREVVED_DEVELOPER)
        add_test(NAME scene_resource_analysis COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/test_scene_resource_analysis.py)
        add_test(NAME route_trace_analysis COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/test_route_trace_analysis.py)
        add_test(NAME scene_resource_payload_analysis COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/test_scene_resource_analysis.py
            --recorder $<TARGET_FILE:rerevved_tests> SceneAnalysisTests.test_cpp_payload_capture)
        add_test(NAME scene_resource_payload_windows_analysis COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/test_scene_resource_analysis.py
            --recorder $<TARGET_FILE:rerevved_tests> SceneAnalysisTests.test_cpp_payload_windows)
    endif()
endif()
