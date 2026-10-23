# Applies the scheduling tags of rerevved_tests cases as CTest properties.
#
# CTest includes this script after Catch2's generated discovery file, so every
# discovered case already exists. The tags are the only source of scheduling:
#
#   native_gpu     RESOURCE_LOCK native_gpu (cases that create a D3D12 device)
#   run_serial     RUN_SERIAL TRUE
#   timeout-<N>    TIMEOUT <N> (seconds; no tag leaves the CTest default)
#
# A misspelled scheduling tag (native-gpu, run-serial, timeout-, timeout-abc,
# a second timeout tag) stops CTest with an error instead of silently running
# the case unscheduled. CTest scripts cannot read test properties back, so the
# tags are read from the generated discovery file.
#
# Expects REREVVED_TEST_DISCOVERY_FILE, the exact discovery file of this configuration.

set(_rerevved_discovery "${REREVVED_TEST_DISCOVERY_FILE}")
if(EXISTS "${_rerevved_discovery}")
    file(READ "${_rerevved_discovery}" _rerevved_discovery_text)
    # Tag lists carry semicolons and bracket quoting; hide both from list splitting.
    string(REPLACE ";" "<SC>" _rerevved_discovery_text "${_rerevved_discovery_text}")
    string(REPLACE "[" "<LB>" _rerevved_discovery_text "${_rerevved_discovery_text}")
    string(REPLACE "]" "<RB>" _rerevved_discovery_text "${_rerevved_discovery_text}")
    string(REPLACE "\n" ";" _rerevved_discovery_lines "${_rerevved_discovery_text}")
    foreach(_line IN LISTS _rerevved_discovery_lines)
        if(NOT _line MATCHES "^set_tests_properties\\( ([^ ]+) PROPERTIES LABELS <LB>==<LB>(.*)<RB>==<RB>\\)$")
            continue()
        endif()
        set(_name "${CMAKE_MATCH_1}")
        string(REPLACE "<SC>" ";" _tags "${CMAKE_MATCH_2}")
        set(_properties "")
        set(_timeouts 0)
        foreach(_tag IN LISTS _tags)
            string(TOLOWER "${_tag}" _lower)
            if(_tag STREQUAL "native_gpu")
                list(APPEND _properties RESOURCE_LOCK native_gpu)
            elseif(_tag STREQUAL "run_serial")
                list(APPEND _properties RUN_SERIAL TRUE)
            elseif(_tag MATCHES "^timeout-([0-9]+)$")
                math(EXPR _timeouts "${_timeouts} + 1")
                list(APPEND _properties TIMEOUT ${CMAKE_MATCH_1})
            elseif(_lower MATCHES "^(native[-_]?gpu|gpu|run[-_]?serial|serial|timeout.*)$")
                message(FATAL_ERROR "Unknown scheduling tag [${_tag}] on rerevved_tests case ${_name}; use native_gpu, run_serial or timeout-<seconds>")
            endif()
        endforeach()
        if(_timeouts GREATER 1)
            message(FATAL_ERROR "rerevved_tests case ${_name} carries more than one timeout tag")
        endif()
        if(_properties)
            set_tests_properties(${_name} PROPERTIES ${_properties})
        endif()
    endforeach()
endif()
