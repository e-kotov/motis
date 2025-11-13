include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(PC_NNG QUIET nng)
endif ()

find_path(nng_INCLUDE_DIR
          NAMES nng/nng.h
          HINTS ${PC_NNG_INCLUDE_DIRS})

find_library(nng_LIBRARY
             NAMES nng
             HINTS ${PC_NNG_LIBRARY_DIRS})

set(nng_INCLUDE_DIRS ${nng_INCLUDE_DIR})
set(nng_LIBRARIES ${nng_LIBRARY})

find_package_handle_standard_args(
    nng DEFAULT_MSG nng_LIBRARY nng_INCLUDE_DIR)

if (nng_FOUND AND NOT TARGET nng::nng)
  add_library(nng::nng UNKNOWN IMPORTED)
  set_target_properties(nng::nng PROPERTIES
      IMPORTED_LOCATION "${nng_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${nng_INCLUDE_DIR}")
  if (PC_NNG_LIBRARIES)
    set_target_properties(nng::nng PROPERTIES
        INTERFACE_LINK_LIBRARIES "${PC_NNG_LIBRARIES}")
  endif ()
  if (PC_NNG_LINK_OPTIONS)
    set_target_properties(nng::nng PROPERTIES
        INTERFACE_LINK_OPTIONS "${PC_NNG_LINK_OPTIONS}")
  endif ()
endif ()
