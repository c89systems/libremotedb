include("${CMAKE_CURRENT_LIST_DIR}/../cmake/DependencyCache.cmake")
if(NOT CPKT_DEPENDENCY_CACHE STREQUAL EXPECTED)
  message(FATAL_ERROR "Expected cache '${EXPECTED}', got '${CPKT_DEPENDENCY_CACHE}'")
endif()
