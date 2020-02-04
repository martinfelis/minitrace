#include "catch.hpp"

#define MTR_ENABLED
#define MTR_DEAR_IMGUI

#include "../minitrace.cc"

TEST_CASE ("simple", "simple2") {
	REQUIRE ( 1 == 1);
}
