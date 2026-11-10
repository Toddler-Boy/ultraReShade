# What ReShade.vcxproj and its deps projects do that upstream's CMakeLists.txt leaves out, for cl and clang-cl.
# Everything generated goes under ${CMAKE_BINARY_DIR}/ultra, nothing is written into the source tree.

set(ULTRA_DIR ${CMAKE_BINARY_DIR}/ultra)

# The development build, as ultraGame's ULTRAGAME_DEVELOPMENT: add-ons and effects reload when their files change
option(ULTRARESHADE_DEVELOPMENT "Development build: add-ons and effects reload while the game runs" OFF)

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

# version.h from the latest tag, as tools/update_version.ps1 writes it (build number 0)
find_package(Git REQUIRED)
execute_process(
  COMMAND ${GIT_EXECUTABLE} describe --tags
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE ULTRA_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT ULTRA_DESCRIBE MATCHES "v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  message(FATAL_ERROR "No version tag found by 'git describe --tags' (got '${ULTRA_DESCRIBE}'): fetch the tags, ReShade.fxh needs the version")
endif()
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

function(ultra_generate_glad)
  get_directory_property(apis DIRECTORY deps DEFINITION GLAD_APIS)
  get_directory_property(extensions DIRECTORY deps DEFINITION GLAD_EXTENSIONS)
  get_directory_property(specifications DIRECTORY deps DEFINITION GLAD_SPECIFICATIONS)
  get_directory_property(generated DIRECTORY deps DEFINITION GLAD_GENERATED_FILES)
  list(TRANSFORM generated REPLACE "^${CMAKE_CURRENT_SOURCE_DIR}/deps/glad/target" "${ULTRA_DIR}/glad/target")

  set(stamp "${apis}|${extensions}")
  foreach(specification ${specifications})
    file(TIMESTAMP ${specification} time)
    file(SIZE ${specification} size)
    string(APPEND stamp "|${specification}:${time}:${size}")
  endforeach()
  set(missing FALSE)
  foreach(file ${generated})
    if(NOT EXISTS ${file})
      set(missing TRUE)
    endif()
  endforeach()
  if(missing OR NOT stamp STREQUAL "${ULTRA_GLAD_STAMP}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(COPY ${specifications} DESTINATION ${ULTRA_DIR}/glad)
    set(ENV{PYTHONPATH} ${CMAKE_CURRENT_SOURCE_DIR}/deps/glad)
    set(ENV{PYTHONDONTWRITEBYTECODE} 1)
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -m glad --quiet --out-path ./target --api ${apis} --extensions ${extensions} c --mx
      WORKING_DIRECTORY ${ULTRA_DIR}/glad
      RESULT_VARIABLE result
      ERROR_VARIABLE errors
    )
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "glad generation failed (pip install -r deps/glad/requirements.txt?): ${errors}")
    endif()
    set(ULTRA_GLAD_STAMP "${stamp}" CACHE INTERNAL "")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${specifications})

  set_property(TARGET glad PROPERTY SOURCES ${generated})
  set_property(TARGET glad PROPERTY INCLUDE_DIRECTORIES ${ULTRA_DIR}/glad/target/include)
  set_property(TARGET glad PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${ULTRA_DIR}/glad/target/include)
endfunction()

function(ultra_finish_targets)
  ultra_generate_glad()

  set_target_properties(ReShade ReShadeFX PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

  foreach(target ImGui fpng jxl ReShadeFX)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
  endforeach()
  foreach(target fpng jxl ReShadeFX ReShade)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Release>:_HAS_EXCEPTIONS=0>)
  endforeach()
  target_compile_options(jxl PRIVATE /wd4715)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Upstream's non-cl flags (clang-cl takes its MSVC branch); clang-cl knows -msse4.1, not -msse4
    target_compile_options(fpng PRIVATE -mpclmul -msse4.1)
    # MinHook calls __movsb undeclared, which only cl treats as an intrinsic
    target_compile_options(MinHook PRIVATE /FIintrin.h)
    foreach(target fpng glad ImGui jxl MinHook stb)
      target_compile_options(${target} PRIVATE -w)
    endforeach()
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
  # Third-party headers: their warnings are not ours to fix
  foreach(target D3D12On7 fpng glad ImGui jxl MinHook OpenVR OpenXR SPIRV stb utfcpp VMA)
    set_property(TARGET ${target} PROPERTY SYSTEM TRUE)
  endforeach()

  get_target_property(sources ReShade SOURCES)
  list(REMOVE_ITEM sources res/resource.rc res/version.h res/version.rc2)
  if(NOT ULTRARESHADE_DEVELOPMENT)
    list(REMOVE_ITEM sources source/ultra_addon_reload.cpp source/ultra_effect_reload.cpp)
  endif()
  set_property(TARGET ReShade PROPERTY SOURCES ${sources})
  target_sources(ReShade PRIVATE ${ULTRA_DIR}/res/resource.rc ${ULTRA_DIR}/res/version.h)
  target_include_directories(ReShade BEFORE PRIVATE ${ULTRA_DIR}/res ${ULTRA_DIR}/$<CONFIG>)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_source_files_properties(${ULTRA_DIR}/res/resource.rc PROPERTIES COMPILE_DEFINITIONS _WIN64)
  endif()

  # Shader bytecode for resource.rc's "shaders\*.cso", per configuration
  foreach(shader copy_ps:Pixel:4.0 fullscreen_vs:Vertex:4.0 imgui_ps_3_0:Pixel:3.0 imgui_ps_4_0:Pixel:4.0 imgui_vs_3_0:Vertex:3.0 imgui_vs_4_0:Vertex:4.0 mipmap_cs_5_0:Compute:5.0)
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

  # Thread-safe statics stay on except in the two OpenGL files the vcxproj exempts
  get_target_property(options ReShade COMPILE_OPTIONS)
  list(REMOVE_ITEM options /Zc:threadSafeInit-)
  set_property(TARGET ReShade PROPERTY COMPILE_OPTIONS ${options})
  set_property(SOURCE source/opengl/opengl.cpp source/opengl/opengl_hooks_ffp.cpp APPEND PROPERTY COMPILE_OPTIONS $<$<CONFIG:Release>:/Zc:threadSafeInit->)

  # Upstream gates it on cl, clang-cl reports itself as Clang
  target_compile_definitions(ReShade PRIVATE RESHADE_LOCALIZATION ULTRARESHADE_DEVELOPMENT=$<BOOL:${ULTRARESHADE_DEVELOPMENT}>)
  target_compile_options(ReShade PRIVATE /W3)
  target_link_options(ReShade PRIVATE /SUBSYSTEM:WINDOWS /DELAYLOAD:D3DCompiler_47.dll /DELAYLOAD:WinMM.dll /DELAYLOAD:XInput9_1_0.dll $<$<CONFIG:Release>:/MANIFEST:NO>)
  target_link_libraries(ReShade PRIVATE delayimp.lib)

  ultra_add_test_app()
endfunction()

# ReShadeTest64.exe / ReShadeTest32.exe: upstream's test application (source/dll_main_test_app.cpp), the whole
# runtime with overlay and add-ons in a resizable window. Base path is its own folder (ReShade.ini, shaders, add-ons).
# Command line: -d3d9, -d3d10, -d3d11 (default), -d3d12, -opengl, -vulkan, -width N, -height N, -vsync, -validation,
# -work-dir <folder> (base path for that run: ini and preset seeded from the exe folder's, log, screenshots, shader cache),
# -config-layered (the seeded ini is an ultragame.ini with [INSTALL] UltraGameConfig=1, read with ultragame-user.ini on top),
# -capture <frame> <file.png> (that frame after the effects, without the overlay); exit codes in source/ultra_test_check.hpp
function(ultra_add_test_app)
  add_executable(ReShadeTest WIN32)
  set_target_properties(ReShadeTest PROPERTIES OUTPUT_NAME ReShadeTest${RESHADE_SUFFIX} VS_DPI_AWARE PerMonitor)
  # Same .cso files as the DLL, compiled by its build
  add_dependencies(ReShadeTest ReShade)

  get_target_property(sources ReShade SOURCES)
  list(FILTER sources EXCLUDE REGEX "\\.hlsl$|ultra_dll_entry\\.cpp$")
  target_sources(ReShadeTest PRIVATE ${sources} source/dll_main_test_app.cpp source/ultra_test_scene.cpp source/ultra_test_scene.hpp
    source/ultra_test_model.cpp source/ultra_test_model.hpp source/ultra_test_check.cpp source/ultra_test_check.hpp source/ultra_test_run.cpp source/ultra_test_run.hpp
    source/ultra_test_display.cpp source/ultra_test_display.hpp
    source/ultra_test_environment.cpp source/ultra_test_environment.hpp)

  foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(values ReShade ${property})
    if(property STREQUAL "LINK_OPTIONS")
      list(FILTER values EXCLUDE REGEX "/ENTRY:|/MANIFEST:NO")
    endif()
    set_property(TARGET ReShadeTest PROPERTY ${property} ${values})
  endforeach()
  target_compile_definitions(ReShadeTest PRIVATE RESHADE_TEST_APPLICATION)
  target_include_directories(ReShadeTest SYSTEM PRIVATE deps/cgltf)
  # The scenes' models and textures next to the exe (test_scene\head)
  add_custom_command(TARGET ReShadeTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/res/test_scene $<TARGET_FILE_DIR:ReShadeTest>/test_scene)
  # CMake hands .def files only to libraries; the hooks match the system DLLs' exports against these
  target_link_options(ReShadeTest PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/res/exports.def)
endfunction()

cmake_language(DEFER CALL ultra_finish_targets)
