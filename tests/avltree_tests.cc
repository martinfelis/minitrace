#include "catch.hpp"

#define MTR_ENABLED
#define MTR_DEAR_IMGUI

#include "../minitrace.cc"

void reset_events () {
	count = 0;

	if (draw_data) {
    draw_data->is_processed = false;
  }
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

void add_nested (int64_t start) {
  raw_event_t* raw = NULL;

  // add outer
  raw = &buffer[count++];
  raw->cat = 0;
  raw->pid = 0;
  raw->id = 0;
  raw->tid = 0;
  raw->ts = start;
  raw->a_double = 300;
  raw->ph = 'X';
  raw->name = "outer";

  // add a()
  raw = &buffer[count++];
  raw->cat = 0;
  raw->pid = 0;
  raw->id = 0;
  raw->tid = 0;
  raw->ts = start + 10;
  raw->a_double = 200;
  raw->ph = 'X';
  raw->name = "a()";

  // add c()
  raw = &buffer[count++];
  raw->cat = 0;
  raw->pid = 0;
  raw->id = 0;
  raw->tid = 0;
  raw->ts = start + 30;
  raw->a_double = 50;
  raw->ph = 'X';
  raw->name = "c()";

  // add b()
  raw = &buffer[count++];
  raw->cat = 0;
  raw->pid = 0;
  raw->id = 0;
  raw->tid = 0;
  raw->ts = start + 20;
  raw->a_double = 100;
  raw->ph = 'X';
  raw->name = "b()";


}

TEST_CASE ("simple_init", "simple") {
	mtr_init ("test.json");
}

TEST_CASE ("nested", "nested") {
  reset_events();

  add_nested(0);
  add_nested(400);
  add_nested(800);
  add_nested(1200);
  add_nested(1600);
  add_nested(2000);
  mtr_imgui_preprocess();

  REQUIRE(count == 24);
}
