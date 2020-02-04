#include "catch.hpp"

#define MTR_ENABLED
#define MTR_DEAR_IMGUI

#include "../minitrace.cc"

void reset_events () {
	count = 0;
}

void add_event (int64_t start, int64_t end, const char* name) {
	raw_event_t* raw = &buffer[count++];

	raw->cat = 0;
	raw->pid = 0;
	raw->id = 0;
	raw->tid = 0;
	raw->ts = start;
	raw->a_double = end - start;
	raw->ph = 'X';
	raw->name = name;

}

TEST_CASE ("simple_init", "simple") {
	mtr_init ("test.json");
}

TEST_CASE ("simple", "simple2") {
	IntrvlTreeNode* tree = NULL;

	reset_events();

	add_event(3,20, "outer");
	add_event(4,16, "inner");
	add_event(6, 8, "a1");
  add_event(9, 10, "a2");
	add_event(11, 12, "a3");
  add_event(13, 15, "a4");

  mtr_imgui_preprocess();

  REQUIRE (CalcIntrvlDepth (&buffer[0], draw_data->interval_tree_root) == 0);
  REQUIRE (CalcIntrvlDepth (&buffer[1], draw_data->interval_tree_root) == 1);
  REQUIRE (CalcIntrvlDepth (&buffer[2], draw_data->interval_tree_root) == 2);
  REQUIRE (CalcIntrvlDepth (&buffer[3], draw_data->interval_tree_root) == 2);
  REQUIRE (CalcIntrvlDepth (&buffer[4], draw_data->interval_tree_root) == 2);
  REQUIRE (CalcIntrvlDepth (&buffer[5], draw_data->interval_tree_root) == 2);
}
