cmake_minimum_required(VERSION 3.16)
project(Ultralight_IP_CAM_Viewer_Recorder C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Ultralight_IP_CAM_Viewer_Recorder
  src/main.c
  src/plugin_host.c
  src/core/util.c
  src/core/log.c
  src/core/paths.c
  src/core/seh.c
  src/core/watchdog.c
  src/app/camera.c
  src/app/config.c
  src/app/persist.c
  src/app/i18n.c
  src/app/tray.c
  src/app/single_instance.c
  src/audio/audio.c
  src/audio/audio_pipe.c
  src/cam/record.c
  src/cam/async_recorder.c
  src/cam/time_machine.c
  src/cam/archive.c
  src/cam/cam_cache.c
  src/cam/decode.c
  src/cam/onvif.c
  src/cam/ptz.c
  src/cam/warm.c
  src/cam/tile_stream.c
  src/ui/text.c
  src/ui/ui_render.c
  src/ui/ui_intro.c
  src/ui/ui_cam_mgr.c
  src/ui/ui_settings.c
  src/ui/ui_plugins.c
  src/ui/ui_timeline.c
)

target_include_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")

if(FFMPEG_ROOT)
  message(STATUS "Using custom FFmpeg from FFMPEG_ROOT=${FFMPEG_ROOT}")
endif()

# Prefer pkg-config (Linux/macOS, MSYS2). On Windows, you can also use vcpkg
# or pass include/lib paths manually via the cache variables below.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  if(NOT FFMPEG_ROOT)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
  endif()
  pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()

set(FFMPEG_INCLUDE_DIR "" CACHE PATH "Path to FFmpeg include directory (contains libavformat/avformat.h)")
set(FFMPEG_LIBRARY_DIR "" CACHE PATH "Path to FFmpeg library directory (contains avformat/avcodec/etc libs)")
set(SDL2_INCLUDE_DIR "" CACHE PATH "Path to SDL2 include directory (contains SDL.h)")
set(SDL2_LIBRARY_DIR "" CACHE PATH "Path to SDL2 library directory (contains SDL2.lib/SDL2.dll import lib)")
set(FFMPEG_ROOT "" CACHE PATH "Optional: FFmpeg prefix (include/ and lib/) for a custom minimal build")

if(FFMPEG_ROOT)
  set(_ff_inc "${FFMPEG_ROOT}/include")
  set(_ff_lib "${FFMPEG_ROOT}/lib")
  target_include_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${_ff_inc}")
  target_link_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${_ff_lib}")
  # Static FFmpeg libs: order matters.
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE avformat avcodec swscale swresample avutil)
elseif(TARGET PkgConfig::FFMPEG)
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE PkgConfig::FFMPEG)
else()
  if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_LIBRARY_DIR)
    message(FATAL_ERROR "FFmpeg not found. Install via pkg-config or set FFMPEG_ROOT (preferred) or set FFMPEG_INCLUDE_DIR and FFMPEG_LIBRARY_DIR.")
  endif()
  target_include_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${FFMPEG_INCLUDE_DIR}")
  target_link_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${FFMPEG_LIBRARY_DIR}")
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE avformat avcodec avutil swscale swresample)
endif()

if(TARGET PkgConfig::SDL2)
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE PkgConfig::SDL2)
else()
  if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY_DIR)
    message(FATAL_ERROR "SDL2 not found. Install via pkg-config (recommended) or set SDL2_INCLUDE_DIR and SDL2_LIBRARY_DIR.")
  endif()
  target_include_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${SDL2_INCLUDE_DIR}")
  target_link_directories(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${SDL2_LIBRARY_DIR}")
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE SDL2)
endif()

if(WIN32)
  target_link_libraries(Ultralight_IP_CAM_Viewer_Recorder PRIVATE ws2_32 secur32 bcrypt gdi32 shell32 user32)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_link_options(Ultralight_IP_CAM_Viewer_Recorder PRIVATE -static-libgcc -static-libstdc++)
  endif()

  # Embed an .ico for the executable.
  set(_icon_b64 "${CMAKE_CURRENT_LIST_DIR}/assets/camera.ico.b64")
  set(_icon_out "${CMAKE_BINARY_DIR}/camera.ico")
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "[IO.File]::WriteAllBytes('${_icon_out}', [Convert]::FromBase64String((Get-Content -Raw '${_icon_b64}').Trim()))"
    RESULT_VARIABLE _icon_rc
  )
  if(NOT _icon_rc EQUAL 0)
    message(WARNING "Failed to generate ${_icon_out} from ${_icon_b64} (rc=${_icon_rc})")
  else()
    file(TO_CMAKE_PATH "${_icon_out}" _icon_out_cmake)
    set(ICON_PATH "${_icon_out_cmake}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/resources/app.rc.in" "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
    target_sources(Ultralight_IP_CAM_Viewer_Recorder PRIVATE "${CMAKE_BINARY_DIR}/app.rc")
  endif()
endif()

option(COPY_RUNTIME_DEPS "Copy runtime DLLs next to the executable (Windows)" ON)
if(WIN32 AND COPY_RUNTIME_DEPS)
  set(MSYS2_UCRT64_BIN "C:/msys64/ucrt64/bin" CACHE PATH "MSYS2 UCRT64 bin dir for runtime DLL lookup")
  add_custom_command(
    TARGET Ultralight_IP_CAM_Viewer_Recorder
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -Dexe=$<TARGET_FILE:Ultralight_IP_CAM_Viewer_Recorder>
            -Dout=$<TARGET_FILE_DIR:Ultralight_IP_CAM_Viewer_Recorder>
            -Dsearch_dir=${MSYS2_UCRT64_BIN}
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/copy_runtime_deps.cmake"
    VERBATIM
  )
endif()

add_custom_target(portable
  COMMAND "${CMAKE_COMMAND}"
          -Dbuild_dir=$<TARGET_FILE_DIR:Ultralight_IP_CAM_Viewer_Recorder>
          -Dout_dir=${CMAKE_CURRENT_LIST_DIR}/dist
          -P "${CMAKE_CURRENT_LIST_DIR}/cmake/make_portable.cmake"
  DEPENDS Ultralight_IP_CAM_Viewer_Recorder
  VERBATIM
)
