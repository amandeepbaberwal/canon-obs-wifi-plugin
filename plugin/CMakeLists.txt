cmake_minimum_required(VERSION 3.25)
project(obs-m50-source C)

set(CMAKE_C_STANDARD 11)

set(OBS_STUDIO_DIR "C:/Users/aman/obs-studio" CACHE PATH "obs-studio source dir (matching your OBS version tag)")
set(OBS_BUILD_DIR "C:/Users/aman/obs-build" CACHE PATH "obs-studio build dir containing libobs/Release/obs.lib")

add_library(obs-m50-source MODULE
  src/plugin-main.c
  src/m50.c
  src/jpeg_wic.c
)

target_include_directories(obs-m50-source PRIVATE
  "${OBS_STUDIO_DIR}/libobs"
  "${OBS_BUILD_DIR}/libobs"
  "${OBS_BUILD_DIR}/config"
  "${OBS_STUDIO_DIR}/deps/w32-pthreads"
)

target_link_libraries(obs-m50-source PRIVATE
  "${OBS_BUILD_DIR}/libobs/Release/obs.lib"
  "${OBS_BUILD_DIR}/deps/w32-pthreads/Release/w32-pthreads.lib"
  windowscodecs
  ole32
  ws2_32
  iphlpapi
)

target_compile_definitions(obs-m50-source PRIVATE
  WIN32
  _WINDOWS
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

set_target_properties(obs-m50-source PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-m50-source"
)

add_executable(m50-probe
  src/probe-main.c
  src/m50.c
  src/jpeg_wic.c
)

target_include_directories(m50-probe PRIVATE
  "${OBS_BUILD_DIR}/config"
)

target_link_libraries(m50-probe PRIVATE
  windowscodecs
  ole32
  ws2_32
  iphlpapi
)

target_compile_definitions(m50-probe PRIVATE
  WIN32
  _WINDOWS
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
