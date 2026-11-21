cmake_minimum_required(VERSION 3.23)
project(ReShade LANGUAGES C;CXX)
include(cmake/ultraReShade.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CONFIGURATION_TYPES Debug;Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Dependencies: every one is fetched, none is edited. Pinned to a tag, or to a commit where the project has none; a fetched
# one stays in the build folder, so a later configure needs no network. SOURCE_SUBDIR points nowhere: the projects' own
# CMake never runs, the targets below build only the parts needed.

include(FetchContent)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0-docking
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
# stb has no releases or tags
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
# stb_image_resize2.h lives in a repo of its own since it left stb's
FetchContent_Declare(stb_image_resize
  GIT_REPOSITORY https://github.com/nothings/stb_image_resize.git
  GIT_TAG a4f303e34a92d1410d510cbe85a1781ce4b8c9fd
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
# glad2's branch head, past the last release (v2.0.8)
FetchContent_Declare(glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG 0a7cf1af9e25e02821587bd4095807fc90b6426a
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR nonexistent
)
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
# The registry files glad generates from. vk.xml stays at v1.4.329: from v1.4.330 on its function pointer types have a
# form glad (v2.0.8 and its branch head) cannot parse
FetchContent_Declare(khronos_gl
  URL https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/6af574a14089ccfee87efe230ebcdd8742859813/xml/gl.xml
  URL_HASH SHA256=b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(khronos_wgl
  URL https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/6af574a14089ccfee87efe230ebcdd8742859813/xml/wgl.xml
  URL_HASH SHA256=9172b2f34cff8eb19ee5c1651b040019c0290a7075679df3a6da2a5f6ffd1f73
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(khronos_vk
  URL https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/v1.4.329/xml/vk.xml
  URL_HASH SHA256=b527fa6afeb508ae71cc5859c43593458ffabbd92a03e545450b6e6c5a73af28
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(imgui minhook stb stb_image_resize glad utfcpp spirv vma d3d12 cgltf khronos_gl khronos_wgl khronos_vk)

# DirectX headers: the D3D12 ones from DirectX-Headers, d3d9on12.h and d3d11on12.h from the Windows SDK

add_library(DirectXHeaders INTERFACE)
target_include_directories(
  DirectXHeaders
  INTERFACE
    ${d3d12_SOURCE_DIR}/include/directx
)

# glad

add_library(glad STATIC)
target_include_directories(
  glad
  PUBLIC
    ${ULTRA_DIR}/glad/target/include
)
if(WIN32)
  target_compile_definitions(
    glad
    PUBLIC
      VK_USE_PLATFORM_WIN32_KHR
  )
endif()

set(GLAD_APIS
  gl:compatibility=4.6
  vulkan=1.4
)
set(GLAD_EXTENSIONS
  GL_ARB_texture_compression_bptc;GL_EXT_texture;GL_EXT_texture_compression_s3tc;GL_EXT_texture_storage;GL_EXT_memory_object;GL_EXT_semaphore
  VK_EXT_conservative_rasterization;VK_EXT_custom_border_color;VK_EXT_debug_utils;VK_EXT_descriptor_indexing;VK_EXT_extended_dynamic_state;VK_EXT_extended_dynamic_state2;VK_EXT_extended_dynamic_state3;VK_EXT_full_screen_exclusive;VK_EXT_graphics_pipeline_library;VK_EXT_host_image_copy;VK_EXT_host_query_reset;VK_EXT_index_type_uint8;VK_EXT_load_store_op_none;VK_EXT_mesh_shader;VK_EXT_multi_draw;VK_EXT_private_data;VK_EXT_swapchain_colorspace;VK_EXT_tooling_info;VK_EXT_transform_feedback;VK_KHR_acceleration_structure;VK_KHR_bind_memory2;VK_KHR_buffer_device_address;VK_KHR_copy_commands2;VK_KHR_create_renderpass2;VK_KHR_dedicated_allocation;VK_KHR_depth_stencil_resolve;VK_KHR_descriptor_update_template;VK_KHR_device_group;VK_KHR_display_swapchain;VK_KHR_draw_indirect_count;VK_KHR_dynamic_rendering;VK_KHR_external_memory_capabilities;VK_KHR_format_feature_flags2;VK_KHR_get_memory_requirements2;VK_KHR_get_physical_device_properties2;VK_KHR_image_format_list;VK_KHR_incremental_present;VK_KHR_maintenance4;VK_KHR_maintenance5;VK_KHR_maintenance9;VK_KHR_pipeline_library;VK_KHR_push_descriptor;VK_KHR_ray_tracing_maintenance1;VK_KHR_ray_tracing_pipeline;VK_KHR_surface;VK_KHR_swapchain;VK_KHR_swapchain_mutable_format;VK_KHR_synchronization2;VK_KHR_timeline_semaphore
)
set(GLAD_GENERATED_FILES
  ${ULTRA_DIR}/glad/target/src/gl.c
  ${ULTRA_DIR}/glad/target/src/vulkan.c
  ${ULTRA_DIR}/glad/target/include/glad/gl.h
  ${ULTRA_DIR}/glad/target/include/glad/vulkan.h
)
if(WIN32)
  set(GLAD_APIS
    ${GLAD_APIS}
    wgl=1.0
  )
  set(GLAD_EXTENSIONS
    ${GLAD_EXTENSIONS}
    GL_EXT_memory_object_win32;GL_EXT_semaphore_win32
    VK_KHR_external_memory_win32;VK_KHR_external_semaphore_win32;VK_KHR_win32_surface
    WGL_ARB_create_context;WGL_ARB_create_context_profile;WGL_ARB_framebuffer_sRGB;WGL_ARB_multisample;WGL_ARB_pbuffer;WGL_ARB_pixel_format;WGL_ARB_render_texture;WGL_EXT_swap_control
  )
  set(GLAD_GENERATED_FILES
    ${GLAD_GENERATED_FILES}
    ${ULTRA_DIR}/glad/target/src/wgl.c
    ${ULTRA_DIR}/glad/target/include/glad/wgl.h
  )
endif()

string(REPLACE ";" "," GLAD_APIS "${GLAD_APIS}")
string(REPLACE ";" "," GLAD_EXTENSIONS "${GLAD_EXTENSIONS}")
set(GLAD_SPECIFICATIONS
  ${khronos_gl_SOURCE_DIR}/gl.xml
  ${khronos_vk_SOURCE_DIR}/vk.xml
  ${khronos_wgl_SOURCE_DIR}/wgl.xml
)
ultra_generate_glad()

# ImGui

add_library(ImGui STATIC)
target_sources(
  ImGui
  PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    source/imgui_config.cpp
    source/imgui_config.hpp
)
target_include_directories(
  ImGui
  PUBLIC
    ${imgui_SOURCE_DIR}
)
target_compile_definitions(
  ImGui
  PUBLIC
    ImTextureID=ImU64
    IMGUI_USER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/source/imgui_config.hpp"
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
  PRIVATE
    "$<$<CONFIG:Release>:IMGUI_DISABLE_DEMO_WINDOWS>"
    "$<$<CONFIG:Release>:IMGUI_DISABLE_DEBUG_TOOLS>"
    "$<$<CONFIG:Release>:IMGUI_DEBUG_PRINTF=((void)0)>"
)

# MinHook

if(WIN32)
  add_library(MinHook STATIC)
  target_sources(
    MinHook
    PRIVATE
      ${minhook_SOURCE_DIR}/src/buffer.c
      ${minhook_SOURCE_DIR}/src/hde/hde32.c
      ${minhook_SOURCE_DIR}/src/hde/hde64.c
      ${minhook_SOURCE_DIR}/src/hook.c
      ${minhook_SOURCE_DIR}/src/trampoline.c
  )
  target_include_directories(
    MinHook
    PUBLIC
      ${minhook_SOURCE_DIR}/include
  )
endif()

# SPIRV

add_library(SPIRV INTERFACE)
target_include_directories(
  SPIRV
  INTERFACE
    ${spirv_SOURCE_DIR}/include/spirv/unified1
)

# stb

add_library(stb STATIC)
target_sources(
  stb
  PRIVATE
    source/stb_impl.c
)
target_include_directories(
  stb
  PUBLIC
    ${stb_SOURCE_DIR}
    ${stb_image_resize_SOURCE_DIR}
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

foreach(target glad ImGui MinHook stb)
  target_compile_options(${target} PRIVATE /fp:fast)
endforeach()

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
  res/fonts/forkawesome.h
  res/fonts/forkawesome.inl
  res/exports.def
  res/resource.h
  res/resource.rc
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
  source/imgui_function_table.cpp
  source/imgui_function_table_19300.cpp
  source/imgui_function_table_19300.hpp
  source/imgui_widgets.cpp
  source/imgui_widgets.hpp
  source/ini_file.cpp
  source/ini_file.hpp
  source/input.cpp
  source/input.hpp
  source/platform_utils.hpp
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
  source/ultra_overlay.cpp
  source/ultra_overlay.hpp
  source/ultra_pass_through.cpp
  source/ultra_pass_through.hpp
  source/ultra_splash.cpp
  source/ultra_splash.hpp
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
      source/platform_utils_windows.cpp
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

target_link_libraries(ReShade PRIVATE ReShadeFX DirectXHeaders glad ImGui MinHook stb utfcpp VMA Windows dwmapi.lib shcore.lib)

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
