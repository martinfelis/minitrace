cmake_minimum_required(VERSION 3.10)

# set the project name
project(MinitraceImGui)

# include glfw but disable documentation building
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation")
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs")
subdirs (glfw)

# add the executable
add_executable(minitraceimgui
	glad/src/glad.c
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_widgets.cpp
	imgui/imgui_demo.cpp
	imgui/examples/imgui_impl_glfw.cpp
	imgui/examples/imgui_impl_opengl3.cpp
	minitraceimgui.cc)

target_include_directories (minitraceimgui PRIVATE
	glad/include
	imgui
	imgui/examples
	)

##target_compile_definitions (minitraceimgui PRIVATE
##	-DIMGUI_IMPL_OPENGL_LOADER_GLAD
##	)


target_link_libraries (minitraceimgui
	glfw
	GL)

