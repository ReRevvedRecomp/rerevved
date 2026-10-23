# The optional Prepared research executable: every Prepared format matrix with
# its fixture-guard case, its fixture emitter and its Original-versus-native
# GPU proof, as `rerevved_prepared_tests --helper <family>.emit|run`. Included
# from tests/gpu/render_tests.cmake, in the same directory scope. rerevved_tests
# keeps one representative fixture per format family (native_format_smoke_test).
#
#   cmake --preset win-amd64-release -DREREVVED_PREPARED_TESTS=ON
#   cmake --build --preset win-amd64-release --target rerevved_prepared_tests --parallel 4
#   ctest --test-dir out/build/win-amd64-release -C Release -L prepared-research
#
# The GPU proofs are not CTest cases: scripts/gpu/test-native-prepared.py <family>
# and the separate *-matrix.py validators drive them with prepared shader catalogs.

option(REREVVED_PREPARED_TESTS
    "Build rerevved_prepared_tests, the Prepared fixture matrices and GPU proofs" OFF)

if(BUILD_TESTING AND WIN32 AND REREVVED_PREPARED_TESTS)
    set(REREVVED_PREPARED_STEMS
        native_pixel_lod
        native_prepared_bc1
        native_prepared_bc1_64
        native_prepared_bc1_256
        native_prepared_bc1_512
        native_prepared_bc1_rect
        native_prepared_bc2
        native_prepared_bc2_128
        native_prepared_bc2_rect
        native_prepared_bc3_matrix
        native_prepared_dxt_matrix
        native_prepared_rgba8_matrix
        native_prepared_float_matrix
        native_prepared_cube_matrix
    )
    set(REREVVED_PREPARED_SOURCES "")
    foreach(_stem IN LISTS REREVVED_PREPARED_STEMS)
        list(APPEND REREVVED_PREPARED_SOURCES
            tests/gpu/${_stem}_original.cpp
            tests/gpu/${_stem}_test.cpp)
    endforeach()

    add_executable(rerevved_prepared_tests
        tests/support/test_main.cpp
        tests/support/helper_commands.cpp
        tests/support/manual_cases.cpp
        tests/support/prepared_file_io.cpp

        tests/support/native_gameplay_fixture_inputs.cpp

        ${REREVVED_PREPARED_FIXTURE_SOURCES}
        ${REREVVED_PREPARED_SOURCES}
    )
    target_include_directories(rerevved_prepared_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(rerevved_prepared_tests PRIVATE
        rerevved_native_fixture_core
        Catch2::Catch2
        rex::runtime
        Threads::Threads
        d3d12 dxgi dxguid d3dcompiler user32
    )
    rexglue_apply_target_settings(rerevved_prepared_tests)
    # TARGET_RUNTIME_DLLS omits the runtime-loaded original GPU plugin.
    add_custom_command(TARGET rerevved_prepared_tests POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:rerevved_prepared_tests>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:rerevved_prepared_tests>;$<TARGET_FILE_DIR:rerevved_prepared_tests>>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::gpu-xenos>
            $<TARGET_FILE_DIR:rerevved_prepared_tests>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    # The fixture-guard cases are CPU-only; they keep their case names under
    # a prefix so they never collide with the representative cases.
    catch_discover_tests(rerevved_prepared_tests
        TEST_SPEC "[ctest]"
        TEST_PREFIX "prepared."
        PROPERTIES LABELS prepared-research
        DISCOVERY_MODE POST_BUILD
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
