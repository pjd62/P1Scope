cmake_minimum_required(VERSION 4.3)
project(p1scope)
set(CMAKE_CXX_STANDARD 20)
option(P1SCOPE_BUILD_DESKTOP "Build the GLFW desktop dashboard" ON)
find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

set(P1SCOPE_COMMON main.cpp HomeWizardDiscovery.cxx HueTemperatures.cxx HttpServer.cxx)
function(configure_p1scope target)
    target_link_libraries(${target} PRIVATE PkgConfig::AVAHI CURL::libcurl
        nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_definitions(${target} PRIVATE
        P1SCOPE_GROUPS_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/groups.json"
        P1SCOPE_HUE_CREDENTIALS="${CMAKE_CURRENT_SOURCE_DIR}/.hue-credentials.json"
        P1SCOPE_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")
endfunction()

add_executable(p1scope-server ${P1SCOPE_COMMON})
target_compile_definitions(p1scope-server PRIVATE P1SCOPE_HEADLESS)
configure_p1scope(p1scope-server)

if(P1SCOPE_BUILD_DESKTOP)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/implot)
    add_executable(p1scope ${P1SCOPE_COMMON}
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMPLOT_DIR}/implot.cpp ${IMPLOT_DIR}/implot_items.cpp)
    target_include_directories(p1scope PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMPLOT_DIR})
    configure_p1scope(p1scope)
    target_link_libraries(p1scope PRIVATE glfw OpenGL::GL)
endif()
