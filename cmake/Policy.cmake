function(remotedb_c89 target)
  target_compile_options(${target} PRIVATE -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
  if(APPLE)
    target_link_options(${target} PRIVATE "LINKER:-fatal_warnings")
  else()
    target_link_options(${target} PRIVATE "LINKER:--fatal-warnings")
    # Bootlin startup/runtime objects can carry upstream build-host DWARF.
    # Remove that non-runtime metadata during the final release link.
    target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:LINKER:--strip-debug>")
  endif()
  target_compile_options(${target} PRIVATE
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
    "-ffile-prefix-map=${REMOTEDB_TOOLCHAIN_ROOT}=toolchain")
endfunction()

# Private executable-only Bootlin runtime policy. Exported SDK targets never
# inherit these paths. Apply equally to Debug/Release tests and smoke consumers.
function(remotedb_local_runtime target)
  if(REMOTEDB_TARGET_ID MATCHES "^x86_64-linux")
    if(REMOTEDB_TARGET_ID MATCHES "musl$")
      set(loader "${CMAKE_SYSROOT}/lib/ld-musl-x86_64.so.1")
    else()
      set(loader "${CMAKE_SYSROOT}/lib/ld-linux-x86-64.so.2")
    endif()
    if(NOT EXISTS "${loader}" OR NOT EXISTS "${CMAKE_SYSROOT}/lib")
      message(FATAL_ERROR "Selected Bootlin runtime missing: ${loader}")
    endif()
    target_link_options(${target} PRIVATE "LINKER:--dynamic-linker=${loader}" "LINKER:--disable-new-dtags")
    set_target_properties(${target} PROPERTIES BUILD_RPATH "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib")
  endif()
endfunction()
