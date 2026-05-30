cmake_minimum_required(VERSION 3.20)
project(mixer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/IDEs pick up the C++20 flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- dependencies ---
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# --- target ---
add_executable(mixer
  src/main.cpp
  src/ca_error.cpp
  src/ca_props.cpp
  src/device.cpp
  src/aggregate.cpp
  src/recorder.cpp
  src/router.cpp
  src/config.cpp
  src/matrix.cpp
)

target_link_libraries(mixer PRIVATE
  nlohmann_json::nlohmann_json
  "-framework CoreAudio"
  "-framework AudioToolbox"
  "-framework CoreFoundation"
)

# --- snapshot tests (golden input config -> mixer output) ---
enable_testing()

set(CASES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases)
set(SNAPSHOT_RUNNER ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_snapshot.cmake)

# Regenerate every snapshot: `cmake --build build --target update_snapshots`.
add_custom_target(update_snapshots)
add_dependencies(update_snapshots mixer)

# Register one snapshot case. ARGN holds extra -D vars for the runner (e.g. the
# plan mode + config/devs files) — passed as separate args, never shell-joined.
function(_snapshot_case name)
  set(common -DMIXER=$<TARGET_FILE:mixer> -DCASES_DIR=${CASES_DIR} -DNAME=${name})
  add_test(NAME snapshot_${name}
    COMMAND ${CMAKE_COMMAND} ${common} ${ARGN} -P ${SNAPSHOT_RUNNER})
  add_custom_command(TARGET update_snapshots POST_BUILD
    COMMAND ${CMAKE_COMMAND} ${common} ${ARGN} -DUPDATE=ON -P ${SNAPSHOT_RUNNER})
endfunction()

# mixer --check <name>.json
function(add_check_case name)
  _snapshot_case(${name})
endfunction()

# mixer --plan <cfg> --devs <devs>
function(add_plan_case name cfg devs)
  _snapshot_case(${name} -DMODE=plan -DCFG=${cfg} -DDEVS=${devs})
endfunction()

# config parse / validate
add_check_case(valid-three-bus)
add_check_case(minimal-recorder)
add_check_case(missing-device)
add_check_case(self-loop)
add_check_case(bad-json)

# matrix planner (synthetic device channel counts -> no hardware needed)
add_plan_case(plan-three-bus valid-three-bus.json three-bus.devs.json)
add_plan_case(plan-asymmetric asymmetric.json asymmetric.devs.json)
add_plan_case(plan-cue-offset cue-offset.json cue-offset.devs.json)