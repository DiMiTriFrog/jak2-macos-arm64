set(IMGUI_SOURCES
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
        imgui_impl_sdl3.cpp
        imgui_stdlib.cpp
        imgui_style.cpp)
if(NOT OG_TARGET_IOS)
        list(APPEND IMGUI_SOURCES imgui_impl_opengl3.cpp)
endif()
add_library(imgui ${IMGUI_SOURCES})

target_link_libraries(imgui SDL3::SDL3)
