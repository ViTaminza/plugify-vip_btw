# vip_btw — Plugify C++ plugin: port of the Pisex VIP module "[VIP] Buy Team Weapon".
#
# Pure s2sdk: no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). All safety rules (map
# gate, slot/controller/pawn checks, schema offsets, KV1 config, console/chat command registration)
# live in src/.
#
#   plugify-vip_btw  the plugin shared library (entry "bin/vip_btw" -> bin/libvip_btw.so)
#   vip_btw_tests    engine-free unit tests: the pure helpers, the KV1 reader, and the map gate,
#                    client guards and command/event dispatch driven through fake __s2sdk_* and
#                    __vip_* bindings (build/tests/vip_btw_tests)

set(VIP_BTW_VERSION "1.1.1")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_btw"), so vip_bindings already exists;
# a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_btw SHARED
        src/plugin.cpp
        src/kv1.cpp
        # The __s2sdk_* / __vip_* binding globals the plugify module fills from the manifest's
        # dependencies; compiled in, never linked as a library.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_btw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_btw SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_btw PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_btw PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_btw PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_btw"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_btw PROPERTIES OUTPUT_NAME vip_btw)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_btw PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_btw.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_btw.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# vip_btw_tests: no engine. The plugin TU (src/plugin.cpp) and the KV1 reader are compiled into
# the test and every __s2sdk_* / __vip_* binding slot is a recording fake: the map gate (B1/B2),
# IsConnected/IsInGame/IsAlive and the schema offsets (C2/C8), the guarded PrintToChat, the
# round_start message, the buy path and the console/chat command dispatch with a clean unload
# (C11/C12). No pisexcompat.
add_executable(vip_btw_tests
        tests/vip_btw_tests.cpp
        src/kv1.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(vip_btw_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_btw_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_btw_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_btw_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_btw_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_btw"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        VIP_BTW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(vip_btw_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
