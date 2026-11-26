add_executable(ssp4simd
  main.cpp
  daemon.cpp
  protocol.cpp
  ndjson.cpp
  server.cpp
)

set(SSP4SIMD_VERSION "unknown")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SSP4SIMD_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE SSP4SIMD_VERSION_RESULT
  )

  if(NOT SSP4SIMD_VERSION_RESULT EQUAL 0 OR SSP4SIMD_VERSION STREQUAL "")
    set(SSP4SIMD_VERSION "unknown")
  endif()
endif()

target_compile_options(ssp4simd PRIVATE -Wfatal-errors)
target_compile_definitions(ssp4simd PRIVATE SSP4SIMD_VERSION="${SSP4SIMD_VERSION}")
target_include_directories(ssp4simd PRIVATE ${CMAKE_SOURCE_DIR}/src/lib/include)

target_link_libraries(ssp4simd PRIVATE ssp4sim_lib)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(ssp4simd PRIVATE nlohmann_json::nlohmann_json)

find_package(TBB CONFIG REQUIRED)
target_link_libraries(ssp4simd PRIVATE TBB::tbb TBB::tbbmalloc)

find_package(Threads REQUIRED)
target_link_libraries(ssp4simd PRIVATE Threads::Threads)

target_link_libraries(ssp4simd PRIVATE ssp4cpp::ssp4cpp)

target_link_libraries(ssp4simd PUBLIC ssp4sim::3rd::fmi4c)

install(
  TARGETS ssp4simd
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(SSP4SIM_BUILD_TEST)
  find_package(Catch2 CONFIG REQUIRED)

  add_executable(test_ssp4simd_ndjson
    test_ndjson.cpp
    ndjson.cpp
    protocol.cpp
  )

  target_include_directories(test_ssp4simd_ndjson PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_SOURCE_DIR}/src/lib/public_include
  )
  target_compile_options(test_ssp4simd_ndjson PRIVATE -Wfatal-errors)
  target_link_libraries(test_ssp4simd_ndjson PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
  )

  enable_testing()
  add_test(NAME ssp4simd_ndjson COMMAND test_ssp4simd_ndjson)
endif()
