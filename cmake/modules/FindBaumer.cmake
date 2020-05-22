# - Try to find Baumer GAPI SDK
# Once done this will define
#
#  BAUMER_FOUND - system has Baumer SDK
#  BAUMER_INCLUDE_DIR - the Baumer SDK include directory
#  BAUMER_LIBRARIES - the libraries needed to use Baumer GAPI SDK

if (NOT BAUMER_DIR)
  if (WIN32)
    set (_BAUMER_DIR "C:\Program Files\Baumer\Baumer GAPI SDK")
  else ()
    set (_BAUMER_DIR "/opt/baumer-gapi-sdk/")
  endif ()
  set (BAUMER_DIR ${_BAUMER_DIR} CACHE PATH "Directory containing Baumer GAPI SDK includes and libraries")
endif ()

find_path (BAUMER_INCLUDE_DIR bgapi2_genicam/bgapi2_genicam.h
    PATHS
    "${BAUMER_DIR}/include"
    DOC "Directory containing include files")

find_library (_BaumerCLib NAMES bgapi2_genicam
    PATHS
    "${BAUMER_DIR}/lib")

set (BAUMER_LIBRARIES ${_BaumerCLib})

mark_as_advanced (_BaumerCLib)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (BAUMER  DEFAULT_MSG  BAUMER_INCLUDE_DIR BAUMER_LIBRARIES)
