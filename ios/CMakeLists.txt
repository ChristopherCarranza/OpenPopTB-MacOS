# iOS / iPadOS build of Populous: The Beginning (software renderer, popTB.exe only).
# The game code (../gen) is generated on the Mac with recomp/gen.py and linked statically: iOS can't
# compile or dlopen code on the device, so the runtime-only "prebuilt" mode (prepare.c) is left out.
cmake_minimum_required(VERSION 3.24)
project(openpoptb_ios LANGUAGES C OBJC)
set(CMAKE_C_STANDARD 11)

set(SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ios-deps/SDL" CACHE PATH "SDL3 source tree")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} sdl EXCLUDE_FROM_ALL)

set(TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB RT_SRC CONFIGURE_DEPENDS ${TOP}/runtime/*.c)
list(REMOVE_ITEM RT_SRC ${TOP}/runtime/prepare.c)
# POPTB_VARIANT: tb = software renderer (popTB.exe, gen/), d3d = Direct3D build (D3DPopTB.exe, gen_d3d/)
set(POPTB_VARIANT "tb" CACHE STRING "tb or d3d")
if(POPTB_VARIANT STREQUAL "d3d")
  set(GEN_DIR ${TOP}/gen_d3d)
  set(GAME_EXE D3DPopTB.exe)
  set(APP_NAME "Populous 3D")
else()
  set(GEN_DIR ${TOP}/gen)
  set(GAME_EXE popTB.exe)
  set(APP_NAME "Populous TB")
endif()
file(GLOB GEN_SRC CONFIGURE_DEPENDS ${GEN_DIR}/*.c)
if(NOT GEN_SRC)
  message(FATAL_ERROR "no generated code in ${GEN_DIR}: run recomp/gen.py first (see ios/build_ios.sh)")
endif()

add_executable(PopulousTB MACOSX_BUNDLE ${RT_SRC} ${GEN_SRC} ios_stubs.c)
target_include_directories(PopulousTB PRIVATE ${TOP}/runtime ${GEN_DIR})
target_link_libraries(PopulousTB PRIVATE SDL3::SDL3-static m)
target_compile_definitions(PopulousTB PRIVATE POPTB_EXE="${GAME_EXE}")
target_compile_options(PopulousTB PRIVATE -fno-strict-aliasing -fwrapv -Wno-parentheses -Wno-unused-value)
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-O1;-w")
set_target_properties(PopulousTB PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "${POPTB_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}")
