# This SDK includes the core static closure only; optional adapters are absent.
get_filename_component(_lonejson_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(_lonejson_target lonejson::lonejson lonejson::lonejson_static)
  if(NOT TARGET ${_lonejson_target})
    add_library(${_lonejson_target} STATIC IMPORTED)
    set_target_properties(${_lonejson_target} PROPERTIES
      IMPORTED_LOCATION "${_lonejson_prefix}/lib/liblonejson.a"
      INTERFACE_INCLUDE_DIRECTORIES "${_lonejson_prefix}/include")
  endif()
endforeach()
foreach(_lonejson_component IN LISTS lonejson_FIND_COMPONENTS)
  set(lonejson_${_lonejson_component}_FOUND FALSE)
  if(lonejson_FIND_REQUIRED_${_lonejson_component})
    set(lonejson_FOUND FALSE)
  endif()
endforeach()
unset(_lonejson_prefix)
unset(_lonejson_target)
unset(_lonejson_component)
