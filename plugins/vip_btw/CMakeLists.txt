# vip_btw — Plugify C++ plugin: port of the Pisex VIP module "[VIP] Buy Team Weapon".
#
#   plugify-vip_btw  the plugin shared library (entry "bin/vip_btw" -> bin/libvip_btw.so)

set(VIP_BTW_VERSION "1.0.1")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_* / __hudmenu_* (s2sdk_bindings, hudmenu_bindings - both come in through
#   pisexcompat's PUBLIC link) and __vip_* (vip_bindings, the generated consumer bindings of
#   plugins/vip).
if(NOT TARGET vip_bindings)
    message(FATAL_ERROR "vip_btw needs the generated vip consumer bindings (plugins/vip/CMakeLists.txt, target vip_bindings)")
endif()

add_library(plugify-vip_btw SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_btw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg/pisexcompat headers must not fail -Werror.
target_include_directories(plugify-vip_btw SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_link_libraries(plugify-vip_btw PRIVATE
        pisexcompat
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

# --- tests --------------------------------------------------------------------
# Engine-free logic tests; run by ./test.sh (build/tests).
add_executable(vip_btw_tests tests/vip_btw_tests.cpp)
target_include_directories(vip_btw_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_btw_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_btw_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_btw_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_btw_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
