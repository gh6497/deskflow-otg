# The VERSION file is the only maintained software version. Git identifies builds.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")
set(OTG_RELEASE_TAG "" CACHE STRING "Release tag, must equal v< VERSION >; empty means development build")
set(OTG_GIT_COMMIT "unknown")
set(OTG_GIT_DIRTY FALSE)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE git_result OUTPUT_VARIABLE git_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(git_result EQUAL 0)
    set(OTG_GIT_COMMIT "${git_commit}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE git_status OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT git_status STREQUAL "")
      set(OTG_GIT_DIRTY TRUE)
    endif()
  endif()
endif()
if(NOT OTG_RELEASE_TAG STREQUAL "")
  if(NOT OTG_RELEASE_TAG STREQUAL "v${PROJECT_VERSION}")
    message(FATAL_ERROR "Release tag ${OTG_RELEASE_TAG} does not match VERSION (${PROJECT_VERSION})")
  endif()
  if(OTG_GIT_DIRTY)
    message(FATAL_ERROR "Release builds require a clean tracked working tree")
  endif()
  set(OTG_BUILD_VERSION "${PROJECT_VERSION}")
else()
  set(OTG_BUILD_VERSION "${PROJECT_VERSION}+git.${OTG_GIT_COMMIT}")
  if(OTG_GIT_DIRTY)
    string(APPEND OTG_BUILD_VERSION ".dirty")
  endif()
endif()
configure_file("${PROJECT_SOURCE_DIR}/cmake/build_version.h.in"
  "${PROJECT_BINARY_DIR}/generated/build_version.h" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/cmake/BUILD_VERSION.txt.in"
  "${PROJECT_BINARY_DIR}/BUILD_VERSION.txt" @ONLY)
message(STATUS "Deskflow OTG version: ${OTG_BUILD_VERSION} (commit ${OTG_GIT_COMMIT})")
