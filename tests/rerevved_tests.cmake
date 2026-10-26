# The unified native test executable, its Catch2 discovery and the fast tier.
#
# Included from CMakeLists.txt under BUILD_TESTING with include() so the
# targets and test registrations keep that directory's scope. Catch2 and
# Catch.cmake are already available. The render and hook include files under
# tests/gpu add their sources to rerevved_tests with target_sources().

find_package(Threads REQUIRED)

# Production sources whose test seam (RULES_BUNDLE_TESTING) must not reach the
# title, so they compile once here, apart from the shared production definitions.
add_library(rerevved_test_variants OBJECT
    src/rules_bundle.cpp
    src/mod_script_rules.cpp
)
target_include_directories(rerevved_test_variants PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(rerevved_test_variants PRIVATE
    RULES_BUNDLE_TESTING
)
target_link_libraries(rerevved_test_variants PRIVATE rex::runtime Threads::Threads)
rexglue_apply_target_settings(rerevved_test_variants)

# The C header checks stay C translation units; C++ cases call their renamed
# check functions.
add_library(rerevved_test_c_headers OBJECT
    tests/building_wonder_effect_rules_c_header_test.c
    tests/nation_select_text_c_header_test.c
    tests/terrain_yield_rules_c_header_test.c
    tests/unique_era_abilities_c_header_test.c
    tests/unique_unit_rules_c_header_test.c
    tests/unit_catalog_api_c_header_test.c
    tests/unit_combat_rules_c_header_test.c
    tests/unit_effect_rules_c_header_test.c
    tests/unit_movement_rules_c_header_test.c
    tests/unit_production_cost_rules_c_header_test.c
)
target_include_directories(rerevved_test_c_headers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(rerevved_tests
    tests/support/test_main.cpp
    tests/support/helper_commands.cpp
    tests/support/rules_fixtures.cpp
    tests/support/allocation_harness.cpp
    tests/support/guest_import_doubles.cpp

    src/building_wonder_effect_rules.cpp
    src/ai_economy_settings.cpp
    src/ai_civilization_setup.cpp
    src/ai_economy_hooks.cpp
    src/discord_invites_model.cpp
    src/discord_presence_model.cpp
    src/display_aspect.cpp
    src/city_founding_unit_rules.cpp
    src/gotw_hooks.cpp
    src/gotw_settings.cpp
    src/great_general_attachment.cpp
    src/gameplay_fixes.cpp
    src/gameplay_fixes_hooks.cpp
    src/host_settings.cpp
    src/gameplay_session_settings.cpp
    src/main_menu_logo_asset.cpp
    src/nation_select_text.cpp
    src/nation_select_text_hooks.cpp
    src/occ_hooks.cpp
    src/occ_settings.cpp
    src/raging_barbarians_hooks.cpp
    src/raging_barbarians_settings.cpp
    src/resource_identity_overrides.cpp
    src/resource_settings.cpp
    src/retail_ui_art.cpp
    src/rush_cost.cpp
    src/unit_sale_value.cpp
    src/unit_sale_hooks.cpp
    src/save_sidecar.cpp
    src/start_resources.cpp
    src/system_link_trace.cpp
    src/terrain_yield_rules.cpp
    src/terrain_yield_rules_hooks.cpp
    src/unique_era_abilities.cpp
    src/unique_era_abilities_hooks.cpp
    src/unique_unit_rules.cpp
    src/unit_catalog_api.cpp
    src/unit_combat_rules.cpp
    src/unit_effect_rules.cpp
    src/unit_movement_rules.cpp
    src/unit_production_cost_rules.cpp
    src/unit_production_cost_rules_hooks.cpp
    src/xenos_frame_counter.cpp

    tests/advisor_popups_test.cpp
    tests/ai_civilizations_test.cpp
    tests/ai_civilization_setup_test.cpp
    tests/ai_economy_settings_test.cpp
    tests/ai_economy_hooks_test.cpp
    tests/barbarian_messages_test.cpp
    tests/building_wonder_effect_rules_test.cpp
    tests/bundled_mods_test.cpp
    tests/discord_invites_model_test.cpp
    tests/discord_presence_model_test.cpp
    tests/display_aspect_test.cpp
    tests/city_founding_unit_rules_test.cpp
    tests/gameplay_fixes_test.cpp
    tests/gotw_settings_test.cpp
    tests/great_general_attachment_test.cpp
    tests/host_settings_test.cpp
    tests/gameplay_session_settings_test.cpp
    tests/helper_commands_test.cpp
    tests/network_pause_test.cpp
    tests/main_menu_logo_asset_test.cpp
    tests/mod_script_rules_test.cpp
    tests/nation_select_text_test.cpp
    tests/occ_settings_test.cpp
    tests/raging_barbarians_settings_test.cpp
    tests/research_prompt_test.cpp
    tests/resource_identity_overrides_test.cpp
    tests/resource_identity_trace_test.cpp
    tests/resource_settings_test.cpp
    tests/retail_ui_art_test.cpp
    tests/rules_bundle_test.cpp
    tests/rush_cost_test.cpp
    tests/unit_sale_value_test.cpp
    tests/save_sidecar_test.cpp
    tests/start_resources_test.cpp
    tests/system_link_trace_test.cpp
    tests/terrain_yield_rules_test.cpp
    tests/unique_era_abilities_test.cpp
    tests/unique_unit_rules_test.cpp
    tests/unit_catalog_api_test.cpp
    tests/unit_combat_rules_test.cpp
    tests/unit_effect_rules_test.cpp
    tests/unit_movement_rules_test.cpp
    tests/unit_production_cost_rules_test.cpp
    tests/xenos_frame_counter_test.cpp
)
target_include_directories(rerevved_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)
target_link_libraries(rerevved_tests PRIVATE
    rerevved_test_variants
    rerevved_test_c_headers
    Catch2::Catch2
    rex::runtime
    Threads::Threads
)
rexglue_apply_target_settings(rerevved_tests)
target_compile_definitions(rerevved_tests PRIVATE UNIT_SALE_TESTING)
# The test sources that see a variant source's test seam compile with it too.
set_source_files_properties(
    tests/support/rules_fixtures.cpp
    tests/rules_bundle_test.cpp
    tests/mod_script_rules_test.cpp
    PROPERTIES COMPILE_DEFINITIONS RULES_BUNDLE_TESTING)

# The bundled_mods case loads the mods that scripts/assemble_mods.py writes to
# its default output, with the same version the title reports to the SDK.
target_compile_definitions(rerevved_tests PRIVATE
    REREVVED_BUNDLED_MODS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/out/bundled-mods"
    REREVVED_TEST_GAME_VERSION="${PROJECT_VERSION}")

# One case per former CTest registration, named exactly as it was. Cases tagged
# [ctest] are the automatic ones; manual proofs and helper commands stay out of
# the selection. Discovery runs after linking, so the scheduling properties
# come from a TEST_INCLUDE_FILES script rather than from this configure step.
# Hidden [renderer-research] cases register only on request, through the same
# single discovery call, so they keep their scheduling tags and the label.
option(REREVVED_REGISTER_RESEARCH_TESTS
    "Register the hidden [renderer-research] cases with CTest" OFF)
if(REREVVED_REGISTER_RESEARCH_TESTS)
    set(REREVVED_TEST_SPEC "[ctest],[renderer-research]")
else()
    set(REREVVED_TEST_SPEC "[ctest]")
endif()
catch_discover_tests(rerevved_tests
    TEST_SPEC "${REREVVED_TEST_SPEC}"
    TEST_LIST REREVVED_DISCOVERED_TESTS
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE POST_BUILD
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# Discovery files are named by a hash of the spec; hand the scheduling script
# the exact tests file of this configuration.
get_property(_rerevved_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)
foreach(_include IN LISTS _rerevved_includes)
    if(_include MATCHES "/rerevved_tests-[0-9a-f]+_include\\.cmake$")
        string(REGEX REPLACE "_include\\.cmake$" "_tests.cmake"
            _rerevved_discovery_file "${_include}")
    endif()
endforeach()
if(NOT _rerevved_discovery_file)
    message(FATAL_ERROR "catch_discover_tests registered no rerevved_tests include file")
endif()
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rerevved_tests_properties.cmake"
    CONTENT "set(REREVVED_TEST_DISCOVERY_FILE [==[${_rerevved_discovery_file}]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/tests/support/catch_apply_test_properties.cmake]==])
"
    NEWLINE_STYLE LF)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rerevved_tests_properties.cmake")

# Local-fast tier: the cases tagged [local-fast], an explicit CPU-only,
# GPU-free list in the sources (never a name pattern). This target builds the
# unified binary, which carries every case; select the tier with
# `ctest -L local-fast`. It never depends on the title executable.
add_custom_target(rerevved_fast_tests)
add_dependencies(rerevved_fast_tests rerevved_tests)

# These unittest files are also discovered by scripts/verify.ps1, so a full
# local gate can exclude this label when verify runs on the same worktree.
add_test(NAME save_sidecar_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_save_sidecar_contract.py)
add_test(NAME resource_settings_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_resource_settings_contract.py)
add_test(NAME raging_barbarians_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_raging_barbarians_contract.py)
add_test(NAME occ_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_occ_contract.py)
add_test(NAME gotw_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_gotw_contract.py)
add_test(NAME city_founding_unit_contract COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_city_founding_unit_contract.py)
foreach(_python_contract IN ITEMS
    save_sidecar_contract resource_settings_contract raging_barbarians_contract
    occ_contract gotw_contract city_founding_unit_contract)
    set_property(TEST ${_python_contract} APPEND PROPERTY LABELS python-contract)
endforeach()
