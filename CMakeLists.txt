cmake_minimum_required(VERSION 3.23)
project(ReShade LANGUAGES C;CXX)
include(cmake/ultraReShade.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CONFIGURATION_TYPES Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Dependencies. The ones that compile (ImGui, MinHook, glad) are built once for both bitnesses by ultraGame's
# Tools/build_deps.sh into its Deps folder and only linked here; no build of this repo compiles them. The header-only ones are
# fetched, none is edited. Pinned to a tag, or to a commit where the project has none; a fetched one stays in the build folder,
# so a later configure needs no network. SOURCE_SUBDIR points nowhere: the projects' own CMake never runs.

include(FetchContent)

FetchContent_Declare(utfcpp
  GIT_REPOSITORY https://github.com/nemtrif/utfcpp.git
  GIT_TAG v5.0.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
FetchContent_Declare(spirv
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
  GIT_TAG vulkan-sdk-1.4.363.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.5.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
FetchContent_Declare(d3d12
  GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
  GIT_TAG v1.620.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
# The test app's glTF reader
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
FetchContent_MakeAvailable(utfcpp spirv vma d3d12 cgltf)

# DirectX headers: the D3D12 ones from DirectX-Headers, d3d9on12.h and d3d11on12.h from the Windows SDK

add_library(DirectXHeaders INTERFACE)
target_include_directories(
  DirectXHeaders
  INTERFACE
    ${d3d12_SOURCE_DIR}/include/directx
)

# The prebuilt libraries of ultraGame's Deps folder (Tools/build_deps.sh there)

set(ULTRARESHADE_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../ultraGame/Deps" CACHE PATH "ultraGame's Deps folder with the prebuilt libraries")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ULTRA_DEPS_ARCH x64)
else()
  set(ULTRA_DEPS_ARCH x86)
endif()

function(ultra_deps_library target name)
  set(library ${ULTRARESHADE_DEPS}/${name}/lib/${ULTRA_DEPS_ARCH}/${name}.lib)
  if(NOT EXISTS ${library})
    message(FATAL_ERROR "${library} is missing: run Tools/build_deps.sh in ultraGame")
  endif()
  add_library(${target} STATIC IMPORTED)
  set_target_properties(${target} PROPERTIES IMPORTED_LOCATION ${library} INTERFACE_INCLUDE_DIRECTORIES ${ULTRARESHADE_DEPS}/${name}/include)
endfunction()

# glad

ultra_deps_library(glad glad)
if(WIN32)
  target_compile_definitions(
    glad
    INTERFACE
      VK_USE_PLATFORM_WIN32_KHR
  )
endif()

# ImGui: its configuration header (the TLS context pointer, defined by source/imgui_config.cpp) is installed beside its headers

ultra_deps_library(ImGui imgui)
target_compile_definitions(
  ImGui
  INTERFACE
    ImTextureID=ImU64
    IMGUI_USER_CONFIG="${ULTRARESHADE_DEPS}/imgui/include/imgui_config.hpp"
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
)

# MinHook

if(WIN32)
  ultra_deps_library(MinHook minhook)
endif()

# SPIRV

add_library(SPIRV INTERFACE)
target_include_directories(
  SPIRV
  INTERFACE
    ${spirv_SOURCE_DIR}/include/spirv/unified1
)

# stb: the implementation unit of ours (source/stb_impl.c, with the DDS reader and the HDR PNG writer that build on stb's own
# internals) over stb's headers

add_library(stb STATIC)
target_sources(
  stb
  PRIVATE
    source/stb_impl.c
)
target_include_directories(
  stb
  PUBLIC
    ${ULTRARESHADE_DEPS}/stb/include
    source/stb_image
)
target_compile_definitions(
  stb
  PUBLIC
    STBI_NO_STDIO
    STBI_WRITE_NO_STDIO
    _CRT_SECURE_NO_WARNINGS
)

# utfcpp

add_library(utfcpp INTERFACE)
target_include_directories(
  utfcpp
  INTERFACE
    ${utfcpp_SOURCE_DIR}/source
)
# The build turns char8_t off (/Zc:char8_t-), so utfcpp must not take its C++20 types
target_compile_definitions(
  utfcpp
  INTERFACE
    UTF_CPP_CPLUSPLUS=201703L
)

# VMA

add_library(VMA INTERFACE)
target_include_directories(
  VMA
  INTERFACE
    ${vma_SOURCE_DIR}/include
)
target_compile_definitions(
  VMA
  INTERFACE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
    VMA_STATS_STRING_ENABLED=0
)

# Windows

if(WIN32)
  add_library(Windows INTERFACE)
  target_compile_definitions(
    Windows
    INTERFACE
      WIN32_LEAN_AND_MEAN
      NOGDICAPMASKS
      NOMENUS
      NOICONS
      NOKEYSTATES
      NOSYSCOMMANDS
      NORASTEROPS
      NOATOM
      NOCOLOR
      NODRAWTEXT
      NOMEMMGR
      NOMETAFILE
      NOMINMAX
      NOOPENFILE
      NOSCROLL
      NOSERVICE
      NOSOUND
      NOTEXTMETRIC
      NOCOMM
      NOKANJI
      NOHELP
      NOPROFILER
      NODEFERWINDOWPOS
      NOMCX
  )
  target_link_libraries(
    Windows
    INTERFACE
      d3dcompiler.lib
      dxguid.lib
      ShLwApi.lib
      Version.lib
      WS2_32.lib
      Xinput9_1_0.lib
  )
endif()

target_compile_options(stb PRIVATE /fp:fast)

# ReShade

add_library(ReShade SHARED)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(RESHADE_SUFFIX 64)
else()
  set(RESHADE_SUFFIX 32)
endif()
set_target_properties(ReShade PROPERTIES PREFIX "")
set_target_properties(ReShade PROPERTIES SUFFIX ${RESHADE_SUFFIX}${CMAKE_SHARED_LIBRARY_SUFFIX})

set(RESHADE_SOURCE
  examples/09-depth/generic_depth_addon.cpp
  res/exports.def
  res/resource.h
  res/resource.rc
  res/ultra_ui.rc
  res/version.h
  res/version.rc2
  source/addon.cpp
  source/addon.hpp
  source/addon_manager.cpp
  source/addon_manager.hpp
  source/com_ptr.hpp
  source/com_utils.hpp
  source/dll_log.cpp
  source/dll_log.hpp
  source/dll_main.cpp
  source/dll_resources.cpp
  source/dll_resources.hpp
  source/hook.cpp
  source/hook.hpp
  source/hook_manager.cpp
  source/hook_manager.hpp
  source/imgui_config.cpp
  source/imgui_function_table.cpp
  source/imgui_function_table_19300.cpp
  source/imgui_function_table_19300.hpp
  source/ini_file.cpp
  source/ini_file.hpp
  source/input.cpp
  source/input.hpp
  source/runtime.cpp
  source/runtime.hpp
  source/runtime_api.cpp
  source/runtime_gui.cpp
  source/runtime_internal.hpp
  source/runtime_manager.cpp
  source/runtime_manager.hpp
  source/state_block.cpp
  source/state_block.hpp
  source/ultra_addon_reload.cpp
  source/ultra_addon_reload.hpp
  source/ultra_constant_buffers.cpp
  source/ultra_effect_reload.cpp
  source/ultra_effect_reload.hpp
  source/ultra_effect_settings.cpp
  source/ultra_effect_settings.hpp
  source/ultra_file_watch.cpp
  source/ultra_file_watch.hpp
  source/ultra_home.cpp
  source/ultra_home.hpp
  source/ultra_host_api.cpp
  source/ultra_host_api.hpp
  source/ultra_json.cpp
  source/ultra_json.hpp
  source/ultra_json_watch.cpp
  source/ultra_log.cpp
  source/ultra_log.hpp
  source/ultra_main_log.cpp
  source/ultra_main_log.hpp
  source/ultra_process_exit.cpp
  source/ultra_process_exit.hpp
  source/ultra_resource_ids.cpp
  source/ultra_pass_through.cpp
  source/ultra_pass_through.hpp
  source/ultra_splash.cpp
  source/ultra_splash.hpp
  source/ultra_version.cpp
  source/ultra_version.hpp
  source/ultra_ui_anim.cpp
  source/ultra_ui_anim.hpp
  source/ultra_ui_button.cpp
  source/ultra_ui_choice.cpp
  source/ultra_ui_collapse.cpp
  source/ultra_ui_color_picker.cpp
  source/ultra_ui_colourspace.cpp
  source/ultra_ui_colourspace.hpp
  source/ultra_ui_convert.hpp
  source/ultra_ui_draw.cpp
  source/ultra_ui_draw.hpp
  source/ultra_ui_dropdown.cpp
  source/ultra_ui_font.cpp
  source/ultra_ui_font.hpp
  source/ultra_ui_format.cpp
  source/ultra_ui_format.hpp
  source/ultra_ui_header.cpp
  source/ultra_ui_hotkey.cpp
  source/ultra_ui_hotkey.hpp
  source/ultra_ui_image.cpp
  source/ultra_ui_image.hpp
  source/ultra_ui_label.cpp
  source/ultra_ui_layout.cpp
  source/ultra_ui_layout.hpp
  source/ultra_ui_reset.cpp
  source/ultra_ui_shaders.cpp
  source/ultra_ui_shaders.hpp
  source/ultra_ui_slider.cpp
  source/ultra_ui_subheading.cpp
  source/ultra_ui_table.cpp
  source/ultra_ui_text_field.cpp
  source/ultra_ui_texture.cpp
  source/ultra_ui_texture.hpp
  source/ultra_ui_theme.cpp
  source/ultra_ui_theme.hpp
  source/ultra_ui_toggle.cpp
  source/ultra_ui_tooltip.cpp
  source/ultra_ui_value_edit.cpp
  source/ultra_ui_window.cpp
  source/ultra_ui_window.hpp
  source/ultra_ui_widgets.hpp
)
set(RESHADE_SOURCE_DIRECTX
  source/d2d1/d2d1.cpp
  source/d3d10/d3d10.cpp
  source/d3d10/d3d10_device.cpp
  source/d3d10/d3d10_device.hpp
  source/d3d10/d3d10_impl_command_list.cpp
  source/d3d10/d3d10_impl_command_queue.cpp
  source/d3d10/d3d10_impl_device.cpp
  source/d3d10/d3d10_impl_device.hpp
  source/d3d10/d3d10_impl_state_block.cpp
  source/d3d10/d3d10_impl_state_block.hpp
  source/d3d10/d3d10_impl_swapchain.cpp
  source/d3d10/d3d10_impl_swapchain.hpp
  source/d3d10/d3d10_impl_type_convert.cpp
  source/d3d10/d3d10_impl_type_convert.hpp
  source/d3d10/d3d10_resource.cpp
  source/d3d10/d3d10_resource.hpp
  source/d3d10/d3d10_resource_call_vtable.inl
  source/d3d11/d3d11.cpp
  source/d3d11/d3d11on12.cpp
  source/d3d11/d3d11on12_device.cpp
  source/d3d11/d3d11on12_device.hpp
  source/d3d11/d3d11_command_list.cpp
  source/d3d11/d3d11_command_list.hpp
  source/d3d11/d3d11_device.cpp
  source/d3d11/d3d11_device.hpp
  source/d3d11/d3d11_device_context.cpp
  source/d3d11/d3d11_device_context.hpp
  source/d3d11/d3d11_extensions.cpp
  source/d3d11/d3d11_extensions.hpp
  source/d3d11/d3d11_impl_command_list.cpp
  source/d3d11/d3d11_impl_command_queue.cpp
  source/d3d11/d3d11_impl_device.cpp
  source/d3d11/d3d11_impl_device.hpp
  source/d3d11/d3d11_impl_device_context.hpp
  source/d3d11/d3d11_impl_state_block.cpp
  source/d3d11/d3d11_impl_state_block.hpp
  source/d3d11/d3d11_impl_swapchain.cpp
  source/d3d11/d3d11_impl_swapchain.hpp
  source/d3d11/d3d11_impl_type_convert.cpp
  source/d3d11/d3d11_impl_type_convert.hpp
  source/d3d11/d3d11_resource.cpp
  source/d3d11/d3d11_resource.hpp
  source/d3d11/d3d11_resource_call_vtable.inl
  source/d3d12/d3d12.cpp
  source/d3d12/d3d12_command_list.cpp
  source/d3d12/d3d12_command_list.hpp
  source/d3d12/d3d12_command_queue.cpp
  source/d3d12/d3d12_command_queue.hpp
  source/d3d12/d3d12_descriptor_heap.cpp
  source/d3d12/d3d12_device.cpp
  source/d3d12/d3d12_extensions.cpp
  source/d3d12/d3d12_extensions.hpp
  source/d3d12/d3d12_impl_command_list.cpp
  source/d3d12/d3d12_impl_command_list.hpp
  source/d3d12/d3d12_impl_command_list_immediate.cpp
  source/d3d12/d3d12_impl_command_list_immediate.hpp
  source/d3d12/d3d12_impl_command_queue.cpp
  source/d3d12/d3d12_impl_command_queue.hpp
  source/d3d12/d3d12_impl_device.cpp
  source/d3d12/d3d12_impl_device.hpp
  source/d3d12/d3d12_impl_swapchain.cpp
  source/d3d12/d3d12_impl_swapchain.hpp
  source/d3d12/d3d12_impl_type_convert.cpp
  source/d3d12/d3d12_impl_type_convert.hpp
  source/d3d12/d3d12_pipeline_library.cpp
  source/d3d12/d3d12_pipeline_library.hpp
  source/d3d12/d3d12_pix.cpp
  source/d3d12/d3d12_resource.cpp
  source/d3d12/d3d12_resource.hpp
  source/d3d12/d3d12_resource_call_vtable.inl
  source/d3d9/d3d9.cpp
  source/d3d9/d3d9on12.cpp
  source/d3d9/d3d9on12_device.cpp
  source/d3d9/d3d9on12_device.hpp
  source/d3d9/d3d9_device.cpp
  source/d3d9/d3d9_impl_command_list.cpp
  source/d3d9/d3d9_impl_command_queue.cpp
  source/d3d9/d3d9_impl_device.cpp
  source/d3d9/d3d9_impl_device.hpp
  source/d3d9/d3d9_impl_state_block.cpp
  source/d3d9/d3d9_impl_state_block.hpp
  source/d3d9/d3d9_impl_swapchain.cpp
  source/d3d9/d3d9_impl_swapchain.hpp
  source/d3d9/d3d9_impl_type_convert.cpp
  source/d3d9/d3d9_impl_type_convert.hpp
  source/d3d9/d3d9_internal.cpp
  source/d3d9/d3d9_pix.cpp
  source/d3d9/d3d9_resource.cpp
  source/d3d9/d3d9_resource.hpp
  source/d3d9/d3d9_resource_call_vtable.inl
  source/d3d9/d3d9_swapchain.cpp
  source/d3d9/d3d9_swapchain.hpp
  source/ddraw/ddraw.cpp
  source/dxgi/dxgi.cpp
  source/dxgi/dxgi_adapter.cpp
  source/dxgi/dxgi_adapter.hpp
  source/dxgi/dxgi_d3d10.cpp
  source/dxgi/dxgi_device.cpp
  source/dxgi/dxgi_device.hpp
  source/dxgi/dxgi_factory.cpp
  source/dxgi/dxgi_factory.hpp
  source/dxgi/dxgi_swapchain.cpp
  source/dxgi/dxgi_swapchain.hpp
)
if(NOT ULTRARESHADE_D3D9)
  list(FILTER RESHADE_SOURCE_DIRECTX EXCLUDE REGEX "^source/d3d9/")
endif()
if(NOT ULTRARESHADE_D3D10)
  list(FILTER RESHADE_SOURCE_DIRECTX EXCLUDE REGEX "^source/d3d10/")
endif()
set(RESHADE_SOURCE_OPENGL
  source/opengl/opengl.cpp
  source/opengl/opengl_hooks.hpp
  source/opengl/opengl_hooks_ffp.cpp
  source/opengl/opengl_hooks_wgl.cpp
  source/opengl/opengl_impl_command_list.cpp
  source/opengl/opengl_impl_command_queue.cpp
  source/opengl/opengl_impl_device.cpp
  source/opengl/opengl_impl_device.hpp
  source/opengl/opengl_impl_device_context.hpp
  source/opengl/opengl_impl_state_block.cpp
  source/opengl/opengl_impl_state_block.hpp
  source/opengl/opengl_impl_swapchain.cpp
  source/opengl/opengl_impl_swapchain.hpp
  source/opengl/opengl_impl_type_convert.cpp
  source/opengl/opengl_impl_type_convert.hpp
)
set(RESHADE_SOURCE_VULKAN
  source/vulkan/vulkan.cpp
  source/vulkan/vulkan_hooks.hpp
  source/vulkan/vulkan_hooks_command_list.cpp
  source/vulkan/vulkan_hooks_device.cpp
  source/vulkan/vulkan_hooks_instance.cpp
  source/vulkan/vulkan_hooks_swapchain.cpp
  source/vulkan/vulkan_impl_command_list.cpp
  source/vulkan/vulkan_impl_command_list.hpp
  source/vulkan/vulkan_impl_command_list_immediate.cpp
  source/vulkan/vulkan_impl_command_list_immediate.hpp
  source/vulkan/vulkan_impl_command_queue.cpp
  source/vulkan/vulkan_impl_command_queue.hpp
  source/vulkan/vulkan_impl_device.cpp
  source/vulkan/vulkan_impl_device.hpp
  source/vulkan/vulkan_impl_swapchain.cpp
  source/vulkan/vulkan_impl_swapchain.hpp
  source/vulkan/vulkan_impl_type_convert.cpp
  source/vulkan/vulkan_impl_type_convert.hpp
)
set(RESHADE_SOURCE_WINDOWS
  source/windows/dinput.cpp
  source/windows/dinput8.cpp
  source/windows/user32.cpp
  source/windows/ws2_32.cpp
)

target_sources(
  ReShade
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
      include
    FILES
      include/reshade.hpp
      include/reshade_api.hpp
      include/reshade_api_device.hpp
      include/reshade_api_format.hpp
      include/reshade_api_pipeline.hpp
      include/reshade_api_resource.hpp
      include/reshade_events.hpp
      include/reshade_overlay.hpp
  PRIVATE
    ${RESHADE_SOURCE}
    ${RESHADE_SOURCE_OPENGL}
    ${RESHADE_SOURCE_VULKAN}
    ${RESHADE_SOURCE_DIRECTX}
    ${RESHADE_SOURCE_WINDOWS}
)
if(WIN32)
  target_sources(
    ReShade
    PRIVATE
      source/input_windows.cpp
      source/input_windows_xinput.cpp
  )
endif()

set_source_files_properties(source/input_windows_xinput.cpp PROPERTIES COMPILE_DEFINITIONS "$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,_AMD64_,_X86_>;_WIN32_WINNT=_WIN32_WINNT_WIN7")
set_source_files_properties(source/vulkan/vulkan_impl_device.cpp PROPERTIES COMPILE_DEFINITIONS VMA_IMPLEMENTATION)

set_source_files_properties(examples/09-depth/generic_depth_addon.cpp PROPERTIES COMPILE_DEFINITIONS BUILTIN_ADDON)
if(MSVC)
  set_source_files_properties(examples/09-depth/generic_depth_addon.cpp PROPERTIES COMPILE_FLAGS /FIreshade.hpp)
else()
  set_source_files_properties(examples/09-depth/generic_depth_addon.cpp PROPERTIES COMPILE_FLAGS "-include reshade.hpp")
endif()

target_include_directories(
  ReShade
  PRIVATE
    res
    source
)

target_compile_definitions(
  ReShade
  PRIVATE
    RESHADE_GUI
    RESHADE_API_LIBRARY_EXPORT
    $<$<CONFIG:Debug>:RESHADE_VERBOSE_LOG>
    _GDI32_
    WINSOCK_API_LINKAGE=
    _UNICODE
    _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

if(MSVC)
  target_compile_options(
    ReShade
    PRIVATE
      /Zc:threadSafeInit-
      /utf-8
      /Zc:char8_t-
      $<$<CONFIG:Release>:/Oi /GL /GF /GS- /Gy>
      /fp:fast
      $<$<CONFIG:Release>:/GR->
  )
  target_link_options(
    ReShade
    PRIVATE
      $<$<CONFIG:Release>:/OPT:REF /OPT:NOICF /LTCG>
  )
else()
  target_compile_options(
    ReShade
    PRIVATE
      -DSTDMETHODCALLTYPE=
      -Wno-changes-meaning
      -fPIC
      $<$<CONFIG:Release>:-fno-rtti>
  )
endif()

target_link_libraries(ReShade PRIVATE ReShadeFX DirectXHeaders glad ImGui MinHook stb utfcpp VMA Windows dwmapi.lib dwrite.lib shcore.lib)

# ReShade FX

add_library(ReShadeFX STATIC)

target_sources(
  ReShadeFX
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
      source
    FILES
      source/effect_codegen.hpp
      source/effect_expression.hpp
      source/effect_lexer.hpp
      source/effect_module.hpp
      source/effect_parser.hpp
      source/effect_preprocessor.hpp
      source/effect_symbol_table.hpp
      source/effect_token.hpp
  PRIVATE
    source/effect_codegen_glsl.cpp
    source/effect_codegen_hlsl.cpp
    source/effect_codegen_spirv.cpp
    source/effect_expression.cpp
    source/effect_lexer.cpp
    source/effect_parser_exp.cpp
    source/effect_parser_stmt.cpp
    source/effect_preprocessor.cpp
    source/effect_symbol_table.cpp
    source/effect_symbol_table_intrinsics.inl
)
if(WIN32)
  target_sources(
    ReShadeFX 
    PRIVATE
      source/effect_codegen_dxbc.cpp
      source/effect_codegen_dxil.cpp
  )
endif()

target_compile_definitions(
  ReShadeFX
  PRIVATE
    _UNICODE
)

if(MSVC)
  target_compile_options(
    ReShadeFX
    PRIVATE
      /utf-8
      /Zc:char8_t-
      $<$<CONFIG:Release>:/Oi /GL /GF /GS- /Gy>
      $<$<CONFIG:Release>:/GR->
  )
else()
  target_compile_options(
    ReShadeFX
    PRIVATE
      -fPIC
      $<$<CONFIG:Release>:-fno-rtti>
  )
endif()

target_link_libraries(ReShadeFX PRIVATE SPIRV)
