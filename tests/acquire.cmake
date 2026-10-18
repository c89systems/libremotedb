include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AcquireArchive.cmake")
remotedb_acquire_archive(fixture "${TEST_URL}" "${TEST_DIGEST}" archive)
file(WRITE "${TEST_OUTPUT}" "${archive}")
