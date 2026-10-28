# Copies the scenario results and the diagnostics log from a Kobo connected
# over USB into a run's device folder, which compare.cmake reads.
#   cmake -DDEVICE=/Volumes/KOBOeReader -DOUTPUT=build/eval/<label>/runs/<time>/device-qt5
#       -P platforms/kobo/collect-eval.cmake
if(NOT DEFINED DEVICE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "DEVICE and OUTPUT are required")
endif()
set(diagnostics "${DEVICE}/.adds/kobalt/diagnostics")
if(NOT EXISTS "${diagnostics}/scenarios.jsonl")
    message(FATAL_ERROR "The Kobo has no scenario results yet. Open the test book in Kobalt and wait for "
        "the scenario-run-end event.")
endif()
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(name scenarios.jsonl events.jsonl events.jsonl.previous)
    if(EXISTS "${diagnostics}/${name}")
        file(COPY "${diagnostics}/${name}" DESTINATION "${OUTPUT}")
    endif()
endforeach()
message(STATUS "Collected the results into ${OUTPUT}")
