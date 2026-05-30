# Snapshot test runner (invoked via `cmake -P`).
#
# Runs `mixer --check <case>.json` from the cases dir, captures stdout + stderr +
# exit code into a single snapshot string, and either compares it to the stored
# <case>.snapshot (test mode) or rewrites it (UPDATE=ON).
#
# Required -D vars: MIXER (abs path to binary), CASES_DIR, NAME.
# Optional: UPDATE; MODE (check|plan, default check); CFG (config file, default
# <name>.json); DEVS (synthetic device-channels file, plan mode only).

set(snapshot_file "${CASES_DIR}/${NAME}.snapshot")

if(NOT DEFINED MODE)
  set(MODE check)
endif()
if(NOT DEFINED CFG)
  set(CFG "${NAME}.json")
endif()

if(MODE STREQUAL "plan")
  set(mixer_args --plan "${CFG}")
  if(DEFINED DEVS)
    list(APPEND mixer_args --devs "${DEVS}")
  endif()
else()
  set(mixer_args --check "${CFG}")
endif()

execute_process(
  COMMAND "${MIXER}" ${mixer_args}
  WORKING_DIRECTORY "${CASES_DIR}"
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE code
)

set(snapshot "exit: ${code}\n===== stdout =====\n${out}===== stderr =====\n${err}")

if(UPDATE)
  file(WRITE "${snapshot_file}" "${snapshot}")
  message(STATUS "updated snapshot: ${NAME}")
else()
  if(NOT EXISTS "${snapshot_file}")
    message(FATAL_ERROR
      "no snapshot for '${NAME}' — generate with: cmake --build build --target update_snapshots")
  endif()
  file(READ "${snapshot_file}" want)
  if(NOT "${snapshot}" STREQUAL "${want}")
    message(FATAL_ERROR
      "snapshot mismatch: ${NAME}\n--- snapshot ---\n${want}\n--- actual ---\n${snapshot}")
  endif()
endif()