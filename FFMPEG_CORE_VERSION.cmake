set(FFMPEG_CORE_VERSION_MAJOR 1)
set(FFMPEG_CORE_VERSION_MINOR 1)
set(FFMPEG_CORE_VERSION_MICRO 1)
set(FFMPEG_CORE_VERSION_REV 0)
set(FFMPEG_CORE_VERSION ${FFMPEG_CORE_VERSION_MAJOR}.${FFMPEG_CORE_VERSION_MINOR}.${FFMPEG_CORE_VERSION_MICRO}.${FFMPEG_CORE_VERSION_REV})
message(STATUS "Generate \"${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_core_version.h\"")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ffmpeg_core_version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_core_version.h")
message(STATUS "Generate \"${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_core.rc\"")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ffmpeg_core.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_core.rc")
