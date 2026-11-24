# What CMakeLists.txt leaves out: flags, version.h, shader bytecode, the test app, for cl and clang-cl.
# Everything generated goes under ${CMAKE_BINARY_DIR}/ultra, nothing is written into the source tree.

set(ULTRA_DIR ${CMAKE_BINARY_DIR}/ultra)

# An API that is off has its folder left out of the build, ULTRARESHADE_D3D9 / ULTRARESHADE_D3D10 (always defined, 1 or 0) guard the code that reaches into it
# and its hook, exports and test scene go with it. D3D10 keeps source/dxgi/dxgi_d3d10.cpp: those are dxgi.dll's exports.
option(ULTRARESHADE_D3D9 "Hook Direct3D 9 (source/d3d9)" ON)
option(ULTRARESHADE_D3D10 "Hook Direct3D 10 (source/d3d10)" ON)

# Release: no C++ exceptions, no RTTI, no buffer checks in every project
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS_DEBUG " /EHsc /GR")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " /GR-")
add_compile_options(/MP /Zc:inline $<$<CONFIG:Release>:/GS->)
add_compile_definitions(UNICODE _UNICODE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # The clang-cl build requires AVX, source/ultra_dll_entry.cpp refuses to load without it
  add_compile_options(-mavx)
endif()

# version.h from the number in VERSION (the effects' __RESHADE__ and the DLL's version resource need it as numbers); the build time is
# taken in by source/ultra_version.cpp, never by this file
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION ULTRA_VERSION)
string(STRIP "${ULTRA_VERSION}" ULTRA_VERSION)
if(NOT ULTRA_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "VERSION holds '${ULTRA_VERSION}', not major.minor.patch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
set(VERSION_MAJOR ${CMAKE_MATCH_1})
set(VERSION_MINOR ${CMAKE_MATCH_2})
set(VERSION_REVISION ${CMAKE_MATCH_3})
set(VERSION_BUILD 0)
set(VERSION_FULL ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}.${VERSION_BUILD})
set(VERSION_PRODUCT ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION})
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/sign.pfx)
  string(APPEND VERSION_PRODUCT " UNOFFICIAL")
endif()
file(CONFIGURE OUTPUT ${ULTRA_DIR}/res/version.h NEWLINE_STYLE CRLF CONTENT [[
#pragma once

#define VERSION_FULL @VERSION_FULL@
#define VERSION_MAJOR @VERSION_MAJOR@
#define VERSION_MINOR @VERSION_MINOR@
#define VERSION_REVISION @VERSION_REVISION@
#define VERSION_BUILD @VERSION_BUILD@

#define VERSION_STRING_FILE "@VERSION_FULL@"
#define VERSION_STRING_PRODUCT "@VERSION_PRODUCT@"
]] @ONLY)

# rc searches the including file's folder first: copies next to the generated version.h keep a stale res/version.h out
configure_file(res/resource.rc ${ULTRA_DIR}/res/resource.rc COPYONLY)
configure_file(res/version.rc2 ${ULTRA_DIR}/res/version.rc2 COPYONLY)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/panel-theme.json ${CMAKE_CURRENT_SOURCE_DIR}/res/InterVariable.ttf)

# res/exports.def without the sections of the APIs that are off (a section runs from its "; <name>.dll" comment to the next comment)
function(ultra_generate_exports)
  set(skipped_sections)
  if(NOT ULTRARESHADE_D3D9)
    list(APPEND skipped_sections "d3d9.dll")
  endif()
  if(NOT ULTRARESHADE_D3D10)
    list(APPEND skipped_sections "d3d10.dll" "d3d10_1.dll")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/exports.def)
  file(READ res/exports.def text)
  string(REPLACE "\r" "" text "${text}")
  string(REPLACE ";" "<semicolon>" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(kept)
  set(skipping FALSE)
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*<semicolon>")
      set(skipping FALSE)
      foreach(section ${skipped_sections})
        if(line MATCHES "^[ \t]*<semicolon> ${section}( |$)")
          set(skipping TRUE)
        endif()
      endforeach()
    endif()
    if(NOT skipping)
      list(APPEND kept "${line}")
    endif()
  endforeach()
  list(JOIN kept "\n" text)
  string(REPLACE "<semicolon>" ";" text "${text}")
  file(GENERATE OUTPUT ${ULTRA_DIR}/exports.def CONTENT "${text}\n")
endfunction()

# source/ultra_version.cpp takes in VERSION and the build time. It is compiled again exactly when the DLL's other sources or VERSION changed,
# so a DLL nothing changed in is not linked again: it includes a stamp that a custom command makes again whenever one of them is newer
# (OBJECT_DEPENDS says the same on the makefile generators, the Visual Studio ones leave it out). libraries: static libraries the DLL links
function(ultra_version_stamp target version_source libraries)
  set(stamp_dir ${ULTRA_DIR}/stamp/${target})
  set(stamp ${stamp_dir}/ultra_build_stamp.h)
  file(MAKE_DIRECTORY ${stamp_dir})

  set(dependencies ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
  foreach(library ${target} ${libraries})
    get_target_property(sources ${library} SOURCES)
    foreach(source IN LISTS sources)
      cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE)
      get_source_file_property(is_generated ${source} GENERATED)
      if(EXISTS ${source} AND NOT is_generated)
        list(APPEND dependencies ${source})
      endif()
    endforeach()
  endforeach()

  add_custom_command(OUTPUT ${stamp} COMMAND ${CMAKE_COMMAND} -E touch ${stamp} DEPENDS ${dependencies} VERBATIM)
  target_sources(${target} PRIVATE ${stamp})
  target_include_directories(${target} PRIVATE ${stamp_dir})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # #embed is C23, the compile time is the point of the file
    set_source_files_properties(${version_source} PROPERTIES COMPILE_OPTIONS "-Wno-c23-extensions;-Wno-date-time")
  endif()
endfunction()

function(ultra_finish_targets)
  set_target_properties(ReShade ReShadeFX PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

  target_compile_options(ReShadeFX PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
  foreach(target ReShadeFX ReShade)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Release>:_HAS_EXCEPTIONS=0>)
  endforeach()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(stb PRIVATE -w)
    # The ClangCL toolset drops /GL: ThinLTO in its place, lld-link links the bitcode
    foreach(target ReShade ReShadeFX)
      target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    endforeach()
    # Delay-loading XInput makes lld-link note the DllMain in its import library
    target_link_options(ReShade PRIVATE /ignore:importeddllmain)
    # Upstream's switches over API enums handle only the values they need
    target_compile_options(ReShade PRIVATE -Wno-switch)
    target_compile_options(ReShadeFX PRIVATE -Wno-switch)
    # Upstream's templates use names their includers declare later; clang-cl parses templates early from C++20 on
    target_compile_options(ReShade PRIVATE -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20)
    target_sources(ReShade PRIVATE source/ultra_dll_entry.cpp)
    set_source_files_properties(source/ultra_dll_entry.cpp PROPERTIES COMPILE_OPTIONS -mno-avx)
    target_link_options(ReShade PRIVATE /ENTRY:ultraDllEntry)
  endif()
  # Json numbers round-trip exactly and refuse infinity and NaN, which upstream's /fp:fast assumes away
  set_source_files_properties(source/ultra_json.cpp source/ultra_test_json.cpp PROPERTIES COMPILE_OPTIONS /fp:precise)
  # Third-party headers: their warnings are not ours to fix
  foreach(target DirectXHeaders SPIRV stb utfcpp VMA)
    set_property(TARGET ${target} PROPERTY SYSTEM TRUE)
  endforeach()

  ultra_generate_exports()
  target_compile_definitions(ReShade PRIVATE ULTRARESHADE_D3D9=$<BOOL:${ULTRARESHADE_D3D9}> ULTRARESHADE_D3D10=$<BOOL:${ULTRARESHADE_D3D10}>)

  get_target_property(sources ReShade SOURCES)
  list(REMOVE_ITEM sources res/exports.def res/resource.rc res/version.h res/version.rc2)
  set_property(TARGET ReShade PROPERTY SOURCES ${sources})
  target_sources(ReShade PRIVATE ${ULTRA_DIR}/exports.def ${ULTRA_DIR}/res/resource.rc ${ULTRA_DIR}/res/version.h)
  target_include_directories(ReShade BEFORE PRIVATE ${ULTRA_DIR}/res ${ULTRA_DIR}/$<CONFIG>)
  set_source_files_properties(res/ultra_ui.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/panel-theme.json;${CMAKE_CURRENT_SOURCE_DIR}/res/InterVariable.ttf")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_source_files_properties(${ULTRA_DIR}/res/resource.rc PROPERTIES COMPILE_DEFINITIONS _WIN64)
  endif()

  # Shader bytecode for resource.rc's "shaders\*.cso", per configuration
  foreach(shader copy_ps:Pixel:4.0 float_copy_ps:Pixel:4.0 fullscreen_vs:Vertex:4.0 imgui_ps_3_0:Pixel:3.0 imgui_ps_4_0:Pixel:4.0 imgui_vs_3_0:Vertex:3.0 imgui_vs_4_0:Vertex:4.0 mipmap_cs_5_0:Compute:5.0)
    string(REPLACE ":" ";" shader ${shader})
    list(GET shader 0 name)
    list(GET shader 1 type)
    list(GET shader 2 model)
    target_sources(ReShade PRIVATE res/shaders/${name}.hlsl)
    set_source_files_properties(res/shaders/${name}.hlsl PROPERTIES
      VS_SHADER_TYPE ${type}
      VS_SHADER_MODEL ${model}
      VS_SHADER_OBJECT_FILE_NAME "${ULTRA_DIR}/$(Configuration)/shaders/${name}.cso"
    )
  endforeach()

  # Thread-safe statics stay on except in two OpenGL files
  get_target_property(options ReShade COMPILE_OPTIONS)
  list(REMOVE_ITEM options /Zc:threadSafeInit-)
  set_property(TARGET ReShade PROPERTY COMPILE_OPTIONS ${options})
  set_property(SOURCE source/opengl/opengl.cpp source/opengl/opengl_hooks_ffp.cpp APPEND PROPERTY COMPILE_OPTIONS $<$<CONFIG:Release>:/Zc:threadSafeInit->)

  # Upstream gates it on cl, clang-cl reports itself as Clang
  target_compile_options(ReShade PRIVATE /W3)
  target_link_options(ReShade PRIVATE /SUBSYSTEM:WINDOWS /DELAYLOAD:D3DCompiler_47.dll /DELAYLOAD:XInput9_1_0.dll $<$<CONFIG:Release>:/MANIFEST:NO>)
  target_link_libraries(ReShade PRIVATE delayimp.lib)

  ultra_version_stamp(ReShade source/ultra_version.cpp ReShadeFX)
  ultra_add_effect_compiler()
  ultra_add_test_app()
  ultra_add_draw_bench()
endfunction()

# ultra_effect_compile.exe (64-bit, 32-bit binaries are the same): ultraGame's build runs it on its effects, so the binaries they need are
# built once, with the front end this DLL runs (options in source/ultra_effect_compile.cpp). Built when asked for by name, like the test app
function(ultra_add_effect_compiler)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
  endif()
  add_executable(ultra_effect_compile EXCLUDE_FROM_ALL source/ultra_effect_compile.cpp)
  set_target_properties(ultra_effect_compile PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
  target_compile_definitions(ultra_effect_compile PRIVATE $<$<CONFIG:Release>:_HAS_EXCEPTIONS=0>)
  target_compile_options(ultra_effect_compile PRIVATE /utf-8 /Zc:char8_t-)
  target_link_libraries(ultra_effect_compile PRIVATE ReShadeFX Windows delayimp.lib)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(ultra_effect_compile PRIVATE /ignore:importeddllmain)
  endif()
endfunction()

# ReShadeDrawBench64.exe / ReShadeDrawBench32.exe: a draw-heavy D3D11/D3D12 loop without ReShade built in, measuring
# whichever ReShade DLL sits next to it as dxgi.dll (options in source/ultra_draw_bench.cpp)
function(ultra_add_draw_bench)
  # Built only when asked for by name (Tools/test-draw-bench.sh does), never by a plain build
  add_executable(ReShadeDrawBench WIN32 EXCLUDE_FROM_ALL source/ultra_draw_bench.cpp)
  set_target_properties(ReShadeDrawBench PROPERTIES OUTPUT_NAME ReShadeDrawBench${RESHADE_SUFFIX} CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON VS_DPI_AWARE PerMonitor)
  target_compile_definitions(ReShadeDrawBench PRIVATE _HAS_EXCEPTIONS=0)
endfunction()

# ReShadeTest64.exe / ReShadeTest32.exe: upstream's test application (source/dll_main_test_app.cpp), the whole
# runtime with overlay and add-ons in a resizable window. Base path is its home from the index ULTRAGAME_INDEX names.
# Command line: -d3d9, -d3d10, -d3d11 (default), -d3d12, -opengl, -vulkan, -width N, -height N, -vsync, -validation,
# -work-dir <folder> (a shader cache seeded for that run),
# -capture <frame> <file.png> (that frame after the effects, without the overlay); exit codes in source/ultra_test_check.hpp
function(ultra_add_test_app)
  # Built only when asked for by name (Tools/test-run.sh does), never by a plain build
  add_executable(ReShadeTest WIN32 EXCLUDE_FROM_ALL)
  set_target_properties(ReShadeTest PROPERTIES OUTPUT_NAME ReShadeTest${RESHADE_SUFFIX} VS_DPI_AWARE PerMonitor)
  # Same .cso files as the DLL, compiled by its build
  add_dependencies(ReShadeTest ReShade)

  get_target_property(sources ReShade SOURCES)
  list(FILTER sources EXCLUDE REGEX "\\.hlsl$|ultra_dll_entry\\.cpp$|ultra_build_stamp\\.h$")
  target_sources(ReShadeTest PRIVATE ${sources} source/dll_main_test_app.cpp source/ultra_test_scene.cpp source/ultra_test_scene.hpp
    source/ultra_test_model.cpp source/ultra_test_model.hpp source/ultra_test_check.cpp source/ultra_test_check.hpp source/ultra_test_run.cpp source/ultra_test_run.hpp
    source/ultra_test_display.cpp source/ultra_test_display.hpp source/ultra_test_home.cpp source/ultra_test_home.hpp
    source/ultra_test_image.cpp source/ultra_test_image.hpp
    source/ultra_test_environment.cpp source/ultra_test_environment.hpp source/ultra_test_json.cpp source/ultra_test_json.hpp)

  foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(values ReShade ${property})
    if(property STREQUAL "LINK_OPTIONS")
      list(FILTER values EXCLUDE REGEX "/ENTRY:|/MANIFEST:NO")
    endif()
    set_property(TARGET ReShadeTest PROPERTY ${property} ${values})
  endforeach()
  target_compile_definitions(ReShadeTest PRIVATE RESHADE_TEST_APPLICATION)
  target_include_directories(ReShadeTest SYSTEM PRIVATE ${cgltf_SOURCE_DIR})
  # The scenes' models and textures next to the exe (test_scene\head)
  add_custom_command(TARGET ReShadeTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/res/test_scene $<TARGET_FILE_DIR:ReShadeTest>/test_scene)
  # CMake hands .def files only to libraries; the hooks match the system DLLs' exports against these
  target_link_options(ReShadeTest PRIVATE /DEF:${ULTRA_DIR}/exports.def)

  # -dlss (64-bit): NGX's headers (include/) and its static-CRT loader (lib/nvsdk_ngx_s.lib) from a folder outside the tree,
  # ultraGame's build has both in Builds/vs/ngx
  set(ULTRARESHADE_NGX_DIR "" CACHE PATH "NGX's include/ and lib/nvsdk_ngx_s.lib for the test app's -dlss; empty: no -dlss")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND ULTRARESHADE_NGX_DIR)
    if(NOT EXISTS ${ULTRARESHADE_NGX_DIR}/include/nvsdk_ngx.h OR NOT EXISTS ${ULTRARESHADE_NGX_DIR}/lib/nvsdk_ngx_s.lib)
      message(FATAL_ERROR "ULTRARESHADE_NGX_DIR (${ULTRARESHADE_NGX_DIR}) has no include/nvsdk_ngx.h or lib/nvsdk_ngx_s.lib")
    endif()
    target_sources(ReShadeTest PRIVATE source/ultra_test_dlss.cpp source/ultra_test_dlss.hpp)
    target_include_directories(ReShadeTest SYSTEM PRIVATE ${ULTRARESHADE_NGX_DIR}/include)
    target_link_libraries(ReShadeTest PRIVATE ${ULTRARESHADE_NGX_DIR}/lib/nvsdk_ngx_s.lib)
    target_compile_definitions(ReShadeTest PRIVATE ULTRA_TEST_DLSS=1)

    # -dlss-streamline: the Streamline SDK's include/ (no library: the game's sl.interposer.dll is loaded by name at run time)
    set(ULTRARESHADE_STREAMLINE_DIR "" CACHE PATH "The Streamline SDK's include/ folder for the test app's -dlss-streamline; empty: no -dlss-streamline")
    if(ULTRARESHADE_STREAMLINE_DIR)
      if(NOT EXISTS ${ULTRARESHADE_STREAMLINE_DIR}/sl.h)
        message(FATAL_ERROR "ULTRARESHADE_STREAMLINE_DIR (${ULTRARESHADE_STREAMLINE_DIR}) has no sl.h")
      endif()
      target_include_directories(ReShadeTest SYSTEM PRIVATE ${ULTRARESHADE_STREAMLINE_DIR})
      target_compile_definitions(ReShadeTest PRIVATE ULTRA_TEST_STREAMLINE=1)
      # sl_pcl.h and sl_reflex.h alias std::to_underlying under C++23, which does not compile
      set_source_files_properties(source/ultra_test_dlss.cpp PROPERTIES COMPILE_OPTIONS -clang:-std=c++20)
    endif()
  endif()
endfunction()

cmake_language(DEFER CALL ultra_finish_targets)
