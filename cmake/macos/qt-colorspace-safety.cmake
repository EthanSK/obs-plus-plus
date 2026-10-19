# Qt 6.11.1's cursor-image conversion can crash before OBS even starts clearing a scene collection (QTBUG-147602).
if(Qt6_VERSION VERSION_EQUAL "6.11.1")
  file(REAL_PATH "${Qt6Gui_DIR}/../../.." qt_prefix)
  set(qt_test_output "${CMAKE_BINARY_DIR}/qt-colorspace-test.noindex")
  file(MAKE_DIRECTORY "${qt_test_output}")
  execute_process(
    COMMAND bash "${CMAKE_SOURCE_DIR}/test/osx/run-qt-colorspace-test.sh" "${qt_prefix}" "${qt_test_output}"
    RESULT_VARIABLE qt_colorspace_result
    OUTPUT_VARIABLE qt_colorspace_output
    ERROR_VARIABLE qt_colorspace_error
  )
  if(NOT qt_colorspace_result STREQUAL "0")
    message(FATAL_ERROR
      "Qt's image color-space lifetime check failed:\n${qt_colorspace_output}${qt_colorspace_error}\n"
      "Run cmake/macos/build-qt-colorspace-fix.sh with this Qt prefix and a .noindex scratch directory, "
      "then configure against its patched-qt.noindex prefix. Do not ship the unpatched QtGui."
    )
  endif()
  message(STATUS "Qt image color-space ownership verified")
endif()
