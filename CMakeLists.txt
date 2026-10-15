cmake_minimum_required(VERSION 3.20)
project(reforged CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

# The portable build: the relocated guest layout, no native-fallback execution, explicit MMIO routing. This is the configuration used on 64-bit
# Windows, Linux and (through the app shell) iOS. Translated game code is generated on the user's machine and is never part of this repository:
# pass its directory with -DREFORGED_LIFTED_DIR=<dir> (contains lift_*.cpp and lift_table.cpp); without it the runtime builds and links on its own.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REFORGED_LIFTED_DIR "" CACHE PATH "Directory with the locally generated translated code (optional)")
set(VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers 'include' directory (contains vulkan/vulkan.h)")
set(SSE2NEON_DIR "" CACHE PATH "Directory containing sse2neon.h (required on arm64)")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # host tools (python) are found on the build machine even when cross compiling
if(NOT IOS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
find_package(Threads REQUIRED)

set(PORTABLE_SOURCES
  main kernel kernel2 eeprom usb_ohci disc xbe xthread mmio priv bp nv2a pgraph display gpu_soft gpu_vk vk_shaders vk_ffp
  crypto des lift_rt guestmem nvnet net dsound_hle audio_hle input_hle)
list(TRANSFORM PORTABLE_SOURCES PREPEND "src/")
list(TRANSFORM PORTABLE_SOURCES APPEND ".cpp")

if(WIN32)
  set(PLATFORM_SOURCES src/plat_win.cpp src/input_win.cpp src/display_win.cpp)
else()
  set(PLATFORM_SOURCES src/plat_posix.cpp src/input_null.cpp src/display_null.cpp)
  if(APPLE)
    list(APPEND PLATFORM_SOURCES src/plat_apple.mm)
  endif()
endif()

# Kernel import stubs are generated from tools/kernel_imports.txt and the K_/H_ functions found in src/.
set(KERNEL_STUBS ${CMAKE_SOURCE_DIR}/src/gen/kernel_stubs.cpp)
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
if(IOS)
  # Cross compiling for iOS: generate the stubs first on the build machine (python tools/gen_kernel.py).
  if(NOT EXISTS ${KERNEL_STUBS})
    message(FATAL_ERROR "run 'python tools/gen_kernel.py' before configuring for iOS (it generates ${KERNEL_STUBS})")
  endif()
else()
  add_custom_command(OUTPUT ${KERNEL_STUBS}
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/gen_kernel.py
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_kernel.py ${CMAKE_SOURCE_DIR}/tools/kernel_imports.txt ${RUNTIME_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating kernel import stubs")
endif()

option(REFORGED_LIFTED_AS_MODULE "Build the translated code as a loadable module (lift_module) instead of linking it into the runtime" OFF)
set(LIFTED_SOURCES "")
if(REFORGED_LIFTED_DIR)
  file(GLOB LIFTED_SOURCES CONFIGURE_DEPENDS ${REFORGED_LIFTED_DIR}/*.cpp)
endif()
set(LIFTED_LINKED ${LIFTED_SOURCES})
if(REFORGED_LIFTED_AS_MODULE)
  set(LIFTED_LINKED "")
endif()

# ---- iOS: the runtime as a static library plus the app bundle (built unsigned; the user's sideloading tool signs it) ----
if(IOS)
  list(REMOVE_ITEM PLATFORM_SOURCES src/display_null.cpp)
  list(APPEND PLATFORM_SOURCES src/display_ios.mm)
  add_library(reforged_core STATIC ${PORTABLE_SOURCES} ${PLATFORM_SOURCES} ${KERNEL_STUBS})
  target_include_directories(reforged_core PUBLIC src)
  target_compile_definitions(reforged_core PRIVATE LIFT_NO_NATIVE LIFT_EXPLICIT_MMIO REFORGED_EMBEDDED)
  if(VULKAN_HEADERS_DIR)
    target_include_directories(reforged_core PRIVATE ${VULKAN_HEADERS_DIR})
  endif()
  if(SSE2NEON_DIR)
    target_include_directories(reforged_core PRIVATE ${SSE2NEON_DIR})
  endif()
  target_compile_options(reforged_core PRIVATE -Wno-unused-result -Wno-format-security -fno-strict-aliasing)
  target_link_libraries(reforged_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  target_link_libraries(reforged_core PUBLIC "-framework AudioToolbox" "-framework AVFoundation" "-framework QuartzCore" "-framework Metal")

  add_executable(ReforgedApp MACOSX_BUNDLE ios/main.mm ios/AppDelegate.mm)
  target_link_libraries(ReforgedApp PRIVATE reforged_core "-framework UIKit" "-framework Foundation")
  set_target_properties(ReforgedApp PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.reforged.app
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER dev.reforged.app
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
  return()
endif()

add_executable(reforged src/cli_main.cpp ${PORTABLE_SOURCES} ${PLATFORM_SOURCES} ${KERNEL_STUBS} ${LIFTED_LINKED})
target_include_directories(reforged PRIVATE src)
target_compile_definitions(reforged PRIVATE LIFT_NO_NATIVE LIFT_EXPLICIT_MMIO)
if(REFORGED_LIFTED_DIR AND NOT REFORGED_LIFTED_AS_MODULE)
  target_compile_definitions(reforged PRIVATE HAVE_LIFT)
  target_include_directories(reforged PRIVATE ${REFORGED_LIFTED_DIR})
endif()
if(VULKAN_HEADERS_DIR)
  target_include_directories(reforged PRIVATE ${VULKAN_HEADERS_DIR})
endif()
if(SSE2NEON_DIR)
  target_include_directories(reforged PRIVATE ${SSE2NEON_DIR})
endif()
target_link_libraries(reforged PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(reforged PRIVATE "-framework AudioToolbox")
endif()

if(MSVC)
  target_compile_definitions(reforged PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(reforged PRIVATE /EHsc /MP)
  target_link_libraries(reforged PRIVATE winmm user32 gdi32 ws2_32)
else()
  target_compile_options(reforged PRIVATE -Wno-unused-result -Wno-format-security -fno-strict-aliasing)
endif()

# The translated code as a separately built module: it talks to the runtime only through the HostApi table (src/lift_rt.h), so it can be built by
# a cross compiler without any of the runtime's headers or libraries (this is how the iOS module is produced).
if(REFORGED_LIFTED_DIR AND REFORGED_LIFTED_AS_MODULE)
  add_library(lift_module MODULE ${LIFTED_SOURCES} src/lift_module_main.cpp)
  set_target_properties(lift_module PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  target_compile_definitions(lift_module PRIVATE LIFT_MODULE LIFT_NO_NATIVE LIFT_EXPLICIT_MMIO)
  target_include_directories(lift_module PRIVATE src ${REFORGED_LIFTED_DIR})
  if(SSE2NEON_DIR)
    target_include_directories(lift_module PRIVATE ${SSE2NEON_DIR})
  endif()
  if(NOT WIN32)
    target_link_libraries(lift_module PRIVATE m)
  endif()
endif()

# Behavioural tests for the platform layer (same program on every OS).
enable_testing()
if(WIN32)
  set(PLAT_IMPL src/plat_win.cpp)
else()
  set(PLAT_IMPL src/plat_posix.cpp)
endif()
add_executable(plat_test tests/plat_test.cpp ${PLAT_IMPL})
target_include_directories(plat_test PRIVATE src)
target_link_libraries(plat_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(MSVC)
  target_compile_definitions(plat_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_link_libraries(plat_test PRIVATE winmm user32)
endif()
add_test(NAME plat_test COMMAND plat_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
