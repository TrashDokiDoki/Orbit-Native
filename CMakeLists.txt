cmake_minimum_required(VERSION 3.21)
project(OrbitNative VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
add_executable(OrbitNative WIN32
    native/main.cpp native/media.cpp native/media.h native/workspace.cpp native/workspace.h native/art.qrc)
target_link_libraries(OrbitNative PRIVATE Qt6::Widgets Qt6::Network)
target_compile_definitions(OrbitNative PRIVATE ORBIT_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(OrbitNative PRIVATE user32)
    if(MSVC)
        target_compile_options(OrbitNative PRIVATE /utf-8)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    target_link_libraries(OrbitNative PRIVATE Qt6::DBus)
endif()

enable_testing()
add_test(NAME launch COMMAND OrbitNative --smoke-test)
set_tests_properties(launch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
install(TARGETS OrbitNative RUNTIME DESTINATION bin)

set(CPACK_PACKAGE_NAME "OrbitNative")
set(CPACK_PACKAGE_VENDOR "Orbit")
set(CPACK_PACKAGE_CONTACT "Orbit Native contributors <orbit@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TrashDokiDoki/Orbit-Native")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native desktop island and launcher")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/LICENSE")
if(WIN32)
    qt_generate_deploy_app_script(TARGET OrbitNative OUTPUT_SCRIPT deploy_script)
    install(SCRIPT "${deploy_script}")
    if(MSVC)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
        include(InstallRequiredSystemLibraries)
    endif()
    install(FILES resources/app/LICENSE DESTINATION ".")
    set(CPACK_GENERATOR NSIS)
    set(CPACK_PACKAGE_FILE_NAME "OrbitNative-${PROJECT_VERSION}-windows-x64")
    set(CPACK_NSIS_DISPLAY_NAME "Orbit Native")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Orbit Native")
    set(CPACK_PACKAGE_EXECUTABLES "OrbitNative" "Orbit Native")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "OrbitNative.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES packaging/orbit-native.desktop DESTINATION share/applications)
    install(FILES resources/app/icon.png DESTINATION share/icons/hicolor/256x256/apps RENAME orbit-native.png)
    install(FILES resources/app/LICENSE DESTINATION share/doc/orbit-native RENAME copyright)
    set(CPACK_GENERATOR DEB)
    set(CPACK_DEBIAN_PACKAGE_NAME orbit-native)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
endif()
include(CPack)

option(ORBIT_WEB_SHELL "Build the experimental Orbit interface with system webviews" OFF)
if(ORBIT_WEB_SHELL)
    include(FetchContent)
    set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG 0.12.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(webview)
    add_executable(OrbitShell WIN32 shell/main.cpp shell/shell.qrc native/media.cpp)
    target_include_directories(OrbitShell PRIVATE native)
    target_link_libraries(OrbitShell PRIVATE Qt6::Widgets Qt6::Network webview::core)
    target_compile_definitions(OrbitShell PRIVATE QT_NO_KEYWORDS ORBIT_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_link_libraries(OrbitShell PRIVATE user32 comctl32 gdi32)
        if(MSVC)
            target_compile_options(OrbitShell PRIVATE /utf-8)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(OrbitShell PRIVATE Qt6::DBus)
    endif()
    add_custom_command(TARGET OrbitShell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OrbitShell>/orbit-shell-ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/style.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/mascot.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/core.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/renderer.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/pug.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/icon.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/settings.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/settings.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/settings.js"
            "$<TARGET_FILE_DIR:OrbitShell>/orbit-shell-ui")
    add_test(NAME shell-ui COMMAND OrbitShell --smoke-test)
    set_tests_properties(shell-ui PROPERTIES TIMEOUT 40)
endif()
