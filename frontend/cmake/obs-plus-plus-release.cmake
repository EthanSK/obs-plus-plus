# Local OBS++ releases follow the source commit, not the upstream tag or the date the app is opened.
if(NOT DEFINED OBS_PLUS_PLUS_RELEASE_TIMESTAMP)
  execute_process(
    COMMAND git log -1 --format=%cI
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE OBS_PLUS_PLUS_RELEASE_TIMESTAMP
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(
    COMMAND git rev-parse --path-format=absolute --git-path logs/HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _obs_plus_plus_git_log
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  # A later commit must refresh the timestamp even when only a plug-in changed since the last build.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_obs_plus_plus_git_log}")
  unset(_obs_plus_plus_git_log)
endif()

if(NOT OBS_PLUS_PLUS_RELEASE_TIMESTAMP MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9](Z|[+-][0-9][0-9]:[0-9][0-9])$")
  message(FATAL_ERROR "OBS_PLUS_PLUS_RELEASE_TIMESTAMP must be an ISO 8601 timestamp with a timezone; provide it explicitly for source archives.")
endif()
