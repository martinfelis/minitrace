cmake_minimum_required(VERSION 3.10)

# set the project name
project(MinitraceImGui)

# include glfw but disable documentation building
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation")
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs")
subdirs (glfw)

#
# compile dear imgui in a separater library
#
add_library (imgui
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_widgets.cpp
	imgui/imgui_demo.cpp
	imgui/examples/imgui_impl_glfw.cpp
	imgui/examples/imgui_impl_opengl3.cpp
	)

target_include_directories (imgui PRIVATE glfw/include glad/include imgui imgui/examples)

#
# demo executable
#
add_executable(minitraceimgui
	glad/src/glad.c
	minitrace.cc
	minitraceimgui.cc)

target_include_directories (minitraceimgui PRIVATE
	glad/include
	imgui
	imgui/examples
	)

target_compile_definitions (minitraceimgui PRIVATE
	-DMTR_ENABLED -DMTR_DEAR_IMGUI
	)

target_link_libraries (minitraceimgui
	glfw
	imgui
	GL)

#
# tests
#
enable_testing()

add_executable(runtests
	tests/main.cc
	tests/avltree_tests.cc
	)

target_include_directories (runtests PRIVATE
	imgui
	)

target_link_libraries (runtests
	imgui
	)

add_test(NAME MinitraceTests COMMAND runtests)
