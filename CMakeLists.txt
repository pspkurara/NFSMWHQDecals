cmake_minimum_required(VERSION 3.20)
project(NFSMWHQDecals CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Win32 only (speed.exe is 32-bit). Build: cmake -A Win32
option(HQDECALS_DIAG "Also build the frame-capture diagnostic ASI" OFF)

add_subdirectory(vendor/minhook EXCLUDE_FROM_ALL)
# The default INI written on first start is NFSMWHQDecals.ini of this tree, embedded as a raw string.
set(HQDECALS_INI "${CMAKE_CURRENT_SOURCE_DIR}/NFSMWHQDecals.ini")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HQDECALS_INI}")
file(READ "${HQDECALS_INI}" HQDECALS_INI_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/default_ini.inc" "R\"DGINI(${HQDECALS_INI_TEXT})DGINI\"
")

add_library(NFSMWHQDecals SHARED src/hqdecals.cpp)
target_include_directories(NFSMWHQDecals PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
set_target_properties(NFSMWHQDecals PROPERTIES SUFFIX ".asi")
target_link_libraries(NFSMWHQDecals minhook d3d9 d3dcompiler)
target_compile_definitions(NFSMWHQDecals PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(HQDECALS_DIAG)
  add_library(NFSMWHQDecalsDiag SHARED src/diag.cpp)
  set_target_properties(NFSMWHQDecalsDiag PROPERTIES SUFFIX ".asi")
  target_link_libraries(NFSMWHQDecalsDiag minhook d3d9 d3dcompiler)
  target_compile_definitions(NFSMWHQDecalsDiag PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
