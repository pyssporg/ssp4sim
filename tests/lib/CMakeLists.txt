find_package(Catch2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(Catch)

include(cmake/AddUnitTargets.cmake)

# Tests that legitimately depend on the full pipeline stay in the retained
# integration binary. Everything else is built as a standalone per-test
# executable from its own source cone (see AddUnitTargets.cmake).
set(SSP4SIM_INTEGRATION_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/high_level/test_smoke_simulator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/high_level/test_c_api_control.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core/test_data_recorder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core/test_fmi4c_adapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core/test_sqlite_recorder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/graph/test_sim_graph_builder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/model/test_model_connection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/test_sim_graph_builder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/graph_executor/execution/loop_aware/test_la2_config_compat.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/graph_executor/execution/loop_aware/test_la2_scheduling.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/graph_executor/execution/loop_aware/test_executor_builder_variants.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/graph_executor/execution/substep/test_substep_executor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/test_graph_builder_dedup.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/test_simulation_lifecycle.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/test_snapshot.cpp
)

file(GLOB_RECURSE ALL_SRC CONFIGURE_DEPENDS "*.hpp" "*.cpp")
# NOTE: CONFIGURE_DEPENDS re-configures only when files are added/removed.
# Editing an `#include` inside an existing header does NOT retrigger it; the
# cone closure is recomputed only on the next explicit configure (or build).
# After adding/removing an include in a cone source, re-run `cmake --preset=vcpkg`.
list(REMOVE_ITEM ALL_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/high_level/test_simulator_core_c_api.cpp
)

set(SSP4SIM_UNIT_TESTS "")
foreach(TEST_SRC IN LISTS ALL_SRC)
  if(NOT TEST_SRC IN_LIST SSP4SIM_INTEGRATION_TESTS)
    add_unit_test(${TEST_SRC})
    list(APPEND SSP4SIM_UNIT_TESTS "${TEST_SRC}")
  endif()
endforeach()

# Configure-time consistency check for the cone machinery (unresolved local
# includes, per-test cone). See AddUnitTargets.cmake.
ssp4sim_cone_audit(${SSP4SIM_UNIT_TESTS})

# --- Retained integration binary ---------------------------------------------------
add_executable(ssp4sim_tests test_main.cpp ${SSP4SIM_INTEGRATION_TESTS})

target_include_directories(ssp4sim_tests PRIVATE ${SSP4SIM_TEST_INCLUDE_DIRS})

target_link_libraries(ssp4sim_tests PUBLIC ssp4sim_lib)

# Catch2::Catch2, nlohmann_json and the Catch module were found above; the
# targets exist here by name.
target_link_libraries(ssp4sim_tests PRIVATE Catch2::Catch2)

target_link_libraries(ssp4sim_tests PRIVATE ssp4cpp::ssp4cpp)

target_link_libraries(ssp4sim_tests PUBLIC ssp4sim::3rd::fmi4c)

target_compile_definitions(ssp4sim_tests PRIVATE SSP4SIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

catch_discover_tests(ssp4sim_tests)

# Snapshot restore exercises the full SimulatorCore pipeline (FMI state, resolver,
# storages), which the per-test cone cannot link on its own. It therefore uses
# an integration-style target under the name the snapshot tooling expects.
add_executable(test_snapshot test_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simulation/test_snapshot.cpp)

target_include_directories(test_snapshot PRIVATE ${SSP4SIM_TEST_INCLUDE_DIRS})

target_link_libraries(test_snapshot PUBLIC ssp4sim_lib)

target_link_libraries(test_snapshot PRIVATE Catch2::Catch2)

target_link_libraries(test_snapshot PRIVATE ssp4cpp::ssp4cpp)

target_link_libraries(test_snapshot PUBLIC ssp4sim::3rd::fmi4c)

target_compile_definitions(test_snapshot PRIVATE SSP4SIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

# The synchronous C ABI test drives the full pipeline through the public
# handle, which the per-test cone cannot link on its own (.h/.cpp pair).
add_executable(test_simulator_core_c_api test_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/high_level/test_simulator_core_c_api.cpp)

target_include_directories(test_simulator_core_c_api PRIVATE ${SSP4SIM_TEST_INCLUDE_DIRS})

target_link_libraries(test_simulator_core_c_api PUBLIC ssp4sim_lib)

target_link_libraries(test_simulator_core_c_api PRIVATE Catch2::Catch2)

target_link_libraries(test_simulator_core_c_api PRIVATE ssp4cpp::ssp4cpp)

target_link_libraries(test_simulator_core_c_api PUBLIC ssp4sim::3rd::fmi4c)

target_compile_definitions(test_simulator_core_c_api PRIVATE SSP4SIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

catch_discover_tests(test_simulator_core_c_api)
