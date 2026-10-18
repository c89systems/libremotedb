include("${CMAKE_CURRENT_LIST_DIR}/AcquireArchive.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/LonejsonPins.cmake")
set(digest "${lonejson_${REMOTEDB_TARGET_ID}}")
if(NOT digest)
  message(FATAL_ERROR "No pinned lonejson SDK for ${REMOTEDB_TARGET_ID}")
endif()
set(name "liblonejson-${lonejson_version}-${REMOTEDB_TARGET_ID}")
if(APPLE)
  # The released Darwin binary requires macOS 15; rebuild its unchanged core
  # sources with our selected macOS 11 deployment target.
  set(name "lonejson-${lonejson_version}")
  set(digest 2d5a2c0ac234e81955636965bf703fcf5d0c290ee10b70ccbc9a8c6db4429c2a)
endif()
set(url "https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${name}.tar.gz")
remotedb_acquire_archive(lonejson "${url}" "${digest}" archive)
set(work "${CMAKE_SOURCE_DIR}/.cache/deps-build/${REMOTEDB_TARGET_ID}/lonejson")
set(REMOTEDB_LONEJSON_SDK "${CMAKE_SOURCE_DIR}/.cache/deps/${REMOTEDB_TARGET_ID}/lonejson/install")
remotedb_owned_path("${work}")
remotedb_owned_path("${REMOTEDB_LONEJSON_SDK}")
file(MAKE_DIRECTORY "${REMOTEDB_LONEJSON_SDK}/include")
if(APPLE)
  include(ExternalProject)
  ExternalProject_Add(remotedb_deps_lonejson
    PREFIX "${work}/external" URL "${archive}" URL_HASH "SHA256=${digest}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${work}/source" BINARY_DIR "${work}/build"
    SOURCE_SUBDIR unused
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_SOURCE_DIR}/cmake/deps/lonejson" -B "${work}/build" -G Ninja
      "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake"
      "-DCPKT_LIFECYCLE_SCRIPTS_DIR=${CPKT_LIFECYCLE_SCRIPTS_DIR}"
      "-DREMOTEDB_TARGET_ID=${REMOTEDB_TARGET_ID}" "-DREMOTEDB_LONEJSON_SOURCE=${work}/source"
      "-DCMAKE_INSTALL_PREFIX=${REMOTEDB_LONEJSON_SDK}" -DCMAKE_BUILD_TYPE=Release
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${work}/build" --parallel 1
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install "${work}/build"
    BUILD_BYPRODUCTS "${REMOTEDB_LONEJSON_SDK}/lib/liblonejson.a")
  ExternalProject_Add_StepDependencies(remotedb_deps_lonejson configure
    "${CMAKE_SOURCE_DIR}/cmake/deps/lonejson/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/cmake/LonejsonMetadata.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/lonejson-core-config.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake")
else()
  add_custom_command(OUTPUT "${REMOTEDB_LONEJSON_SDK}/lib/liblonejson.a" "${REMOTEDB_LONEJSON_SDK}/include/lonejson.h"
    COMMAND "${CMAKE_COMMAND}" "-DARCHIVE=${archive}" "-DARCHIVE_ROOT=${name}" "-DWORK=${work}"
      "-DSDK=${REMOTEDB_LONEJSON_SDK}" -P "${CMAKE_SOURCE_DIR}/cmake/PrepareLonejson.cmake"
    DEPENDS "${archive}" cmake/PrepareLonejson.cmake cmake/LonejsonMetadata.cmake cmake/lonejson-core-config.cmake
    VERBATIM)
  add_custom_target(remotedb_deps_lonejson DEPENDS "${REMOTEDB_LONEJSON_SDK}/lib/liblonejson.a" "${REMOTEDB_LONEJSON_SDK}/include/lonejson.h")
endif()
add_library(lonejson::lonejson_static STATIC IMPORTED GLOBAL)
set_target_properties(lonejson::lonejson_static PROPERTIES
  IMPORTED_LOCATION "${REMOTEDB_LONEJSON_SDK}/lib/liblonejson.a"
  INTERFACE_INCLUDE_DIRECTORIES "${REMOTEDB_LONEJSON_SDK}/include")
add_dependencies(lonejson::lonejson_static remotedb_deps_lonejson)
if(REMOTEDB_INSTALL)
  install(DIRECTORY "${REMOTEDB_LONEJSON_SDK}/include/" DESTINATION include)
  install(FILES "${REMOTEDB_LONEJSON_SDK}/lib/liblonejson.a" DESTINATION lib)
  install(DIRECTORY "${REMOTEDB_LONEJSON_SDK}/lib/cmake/lonejson/" DESTINATION lib/cmake/lonejson)
  install(FILES "${REMOTEDB_LONEJSON_SDK}/lib/pkgconfig/lonejson.pc" DESTINATION lib/pkgconfig)
  install(FILES "${REMOTEDB_LONEJSON_SDK}/share/doc/liblonejson/LICENSE" DESTINATION share/remotedb/licenses/lonejson)
  file(WRITE "${CMAKE_BINARY_DIR}/dependencies.json"
    "{\"backend\":\"mock\",\"dependencies\":[{\"name\":\"lonejson\",\"version\":\"${lonejson_version}\",\"target\":\"${REMOTEDB_TARGET_ID}\",\"source\":\"github-release\",\"url\":\"${url}\",\"sha256\":\"${digest}\",\"archive\":\"${name}.tar.gz\",\"license\":\"MIT\",\"bundled\":true,\"role\":\"json-control-and-persistence\"}]}\n")
  install(FILES "${CMAKE_BINARY_DIR}/dependencies.json" DESTINATION share/remotedb)
endif()
