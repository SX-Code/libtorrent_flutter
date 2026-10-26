cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
endif()

project(libtorrent_flutter_native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Inherit the dependency's ABI/feature macros. The bridge needs
# deprecated-functions=ON in 2.1.1 (torrent_info::files).
# Require the exported target: host pkg-config fallback loses target library
# locations when cross-compiling (notably with the Android NDK).
find_package(LibtorrentRasterbar 2.1.1 CONFIG REQUIRED)
set(LTF_TORRENT_TARGET LibtorrentRasterbar::torrent-rasterbar)
# libtorrent 2.1.1 exports a consumer-Debug assertion macro even when only
# a Release library is installed. Assertions change aux::unique_ptr's size,
# hence torrent_status's layout. Match the installed library, not the app.
get_target_property(LTF_IMPORTED_CONFIGS ${LTF_TORRENT_TARGET} IMPORTED_CONFIGURATIONS)
if(LTF_IMPORTED_CONFIGS AND NOT "DEBUG" IN_LIST LTF_IMPORTED_CONFIGS)
  get_target_property(LTF_TORRENT_DEFINITIONS ${LTF_TORRENT_TARGET} INTERFACE_COMPILE_DEFINITIONS)
  list(REMOVE_ITEM LTF_TORRENT_DEFINITIONS "$<$<CONFIG:Debug>:TORRENT_USE_ASSERTS>")
  set_property(TARGET ${LTF_TORRENT_TARGET} PROPERTY INTERFACE_COMPILE_DEFINITIONS "${LTF_TORRENT_DEFINITIONS}")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

option(LTF_STATIC_BRIDGE "Build a static bridge for iOS archive assembly" OFF)
if(LTF_STATIC_BRIDGE)
  add_library(libtorrent_flutter STATIC torrent_bridge.cpp torrent_bridge.h)
else()
  add_library(libtorrent_flutter SHARED torrent_bridge.cpp torrent_bridge.h)
endif()
target_compile_definitions(libtorrent_flutter PRIVATE TORRENT_BRIDGE_EXPORTS)
target_link_libraries(libtorrent_flutter PRIVATE ${LTF_TORRENT_TARGET} Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
  target_compile_definitions(libtorrent_flutter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  target_link_libraries(libtorrent_flutter PRIVATE ws2_32 crypt32 iphlpapi)
  if(MSVC)
    target_compile_options(libtorrent_flutter PRIVATE /utf-8 /bigobj)
  endif()
elseif(ANDROID)
  target_link_libraries(libtorrent_flutter PRIVATE log)
  target_link_options(libtorrent_flutter PRIVATE "-Wl,-z,max-page-size=16384")
elseif(APPLE)
  target_link_libraries(libtorrent_flutter PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
  set_target_properties(libtorrent_flutter PROPERTIES BUILD_WITH_INSTALL_NAME_DIR TRUE INSTALL_NAME_DIR "@rpath")
endif()

set_target_properties(libtorrent_flutter PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(NOT WIN32)
  set_target_properties(libtorrent_flutter PROPERTIES PREFIX "" OUTPUT_NAME "liblibtorrent_flutter")
endif()

option(LTF_BUILD_TESTS "Build native cache and stream regression tests" OFF)
if(LTF_BUILD_TESTS)
  enable_testing()
  add_executable(native_regression_test ../test/native_regression_test.cpp)
  target_compile_definitions(native_regression_test PRIVATE TORRENT_BRIDGE_EXPORTS)
  target_link_libraries(native_regression_test PRIVATE ${LTF_TORRENT_TARGET} Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(native_regression_test PRIVATE ws2_32 crypt32 iphlpapi)
    if(MSVC)
      target_compile_options(native_regression_test PRIVATE /utf-8 /bigobj)
    endif()
  elseif(APPLE)
    target_link_libraries(native_regression_test PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
  endif()
  add_test(NAME native_regression COMMAND native_regression_test)
  set_tests_properties(native_regression PROPERTIES TIMEOUT 90)
endif()
