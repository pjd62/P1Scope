cmake_minimum_required(VERSION 4.3)
project(p1scope)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/implot)

add_executable(p1scope main.cpp
        HomeWizardDiscovery.cxx

        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp

        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp

        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
)

target_include_directories(p1scope PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
)
target_link_libraries(p1scope PRIVATE
        PkgConfig::AVAHI
        CURL::libcurl
        nlohmann_json::nlohmann_json
        glfw
        OpenGL::GL
)
# Keep one editable configuration, also when launched from a build directory.
target_compile_definitions(p1scope PRIVATE
        P1SCOPE_GROUPS_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/groups.json"
)
