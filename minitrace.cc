// minitrace
// Copyright 2014 by Henrik Rydgård
// http://www.github.com/hrydgard/minitrace
// Released under the MIT license.

// See minitrace.h for basic documentation.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#pragma warning (disable:4996)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define __thread __declspec(thread)
#define pthread_mutex_t CRITICAL_SECTION
#define pthread_mutex_init(a, b) InitializeCriticalSection(a)
#define pthread_mutex_lock(a) EnterCriticalSection(a)
#define pthread_mutex_unlock(a) LeaveCriticalSection(a)
#define pthread_mutex_destroy(a) DeleteCriticalSection(a)
#else
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "minitrace.h"

#ifdef __GNUC__
#define ATTR_NORETURN __attribute__((noreturn))
#else
#define ATTR_NORETURN
#endif

#define ARRAY_SIZE(x) sizeof(x)/sizeof(x[0])

// Ugh, this struct is already pretty heavy.
// Will probably need to move arguments to a second buffer to support more than one.
typedef struct raw_event {
	const char *name;
	const char *cat;
	void *id;
	int64_t ts;
	uint32_t pid;
	uint32_t tid;
	char ph;
	mtr_arg_type arg_type;
	const char *arg_name;
	union {
		const char *a_str;
		int a_int;
		double a_double;
	};
} raw_event_t;

static raw_event_t *buffer;
static volatile int count;
static int is_tracing = 0;
static int64_t time_offset;
static int first_line = 1;
static FILE *f;
static __thread int cur_thread_id;	// Thread local storage
static int cur_process_id;
static pthread_mutex_t mutex;

#ifdef __cplusplus
#ifdef MTR_DEAR_IMGUI
struct gui_draw_data;
static gui_draw_data* draw_data = NULL;
void gui_draw_data_reset ();
#endif
#endif

#define STRING_POOL_SIZE 100
static char *str_pool[100];

// Tiny portability layer.
// Exposes:
//	 get_cur_thread_id()
//	 get_cur_process_id()
//	 mtr_time_s()
//	 pthread basics
#ifdef _WIN32
static int get_cur_thread_id() {
	return (int)GetCurrentThreadId();
}
static int get_cur_process_id() {
	return (int)GetCurrentProcessId();
}

static uint64_t _frequency = 0;
static uint64_t _starttime = 0;
double mtr_time_s() {
	if (_frequency == 0) {
		QueryPerformanceFrequency((LARGE_INTEGER*)&_frequency);
		QueryPerformanceCounter((LARGE_INTEGER*)&_starttime);
	}
	__int64 time;
	QueryPerformanceCounter((LARGE_INTEGER*)&time);
	return ((double) (time - _starttime) / (double) _frequency);
}

// Ctrl+C handling for Windows console apps
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
	if (is_tracing && fdwCtrlType == CTRL_C_EVENT) {
		printf("Ctrl-C detected! Flushing trace and shutting down.\n\n");
		mtr_flush();
		mtr_shutdown();
	}
	ExitProcess(1);
}

void mtr_register_sigint_handler() {
	// For console apps:
	SetConsoleCtrlHandler(&CtrlHandler, TRUE);
}

#else

static inline int get_cur_thread_id() {
	return (int)(intptr_t)pthread_self();
}
static inline int get_cur_process_id() {
	return (int)getpid();
}

#if defined(BLACKBERRY)
double mtr_time_s() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time); // Linux must use CLOCK_MONOTONIC_RAW due to time warps
	return time.tv_sec + time.tv_nsec / 1.0e9;
}
#else
double mtr_time_s() {
	static time_t start;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (start == 0) {
		start = tv.tv_sec;
	}
	tv.tv_sec -= start;
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}
#endif	// !BLACKBERRY

static void termination_handler(int signum) ATTR_NORETURN;
static void termination_handler(int signum) {
	(void) signum;
	if (is_tracing) {
		printf("Ctrl-C detected! Flushing trace and shutting down.\n\n");
		mtr_flush();
		fwrite("\n]}\n", 1, 4, f);
		fclose(f);
	}
	exit(1);
}

void mtr_register_sigint_handler() {
#ifndef MTR_ENABLED
	return;
#endif
	// Avoid altering set-to-be-ignored handlers while registering.
	if (signal(SIGINT, &termination_handler) == SIG_IGN)
		signal(SIGINT, SIG_IGN);
}

#endif

void mtr_init_from_stream(void *stream) {
#ifndef MTR_ENABLED
    return;
#endif

#ifdef __cplusplus
#ifdef MTR_DEAR_IMGUI
    gui_draw_data_reset ();
#endif
#endif

	buffer = (raw_event_t *)malloc(INTERNAL_MINITRACE_BUFFER_SIZE * sizeof(raw_event_t));
	is_tracing = 1;
	count = 0;
	f = (FILE *)stream;
	const char *header = "{\"traceEvents\":[\n";
	fwrite(header, 1, strlen(header), f);
	time_offset = (uint64_t)(mtr_time_s() * 1000000);
	first_line = 1;
	pthread_mutex_init(&mutex, 0);
}

void mtr_init(const char *json_file) {
#ifndef MTR_ENABLED
	return;
#endif
	mtr_init_from_stream(fopen(json_file, "wb"));
}

void mtr_shutdown() {
	int i;
#ifndef MTR_ENABLED
	return;
#endif
	is_tracing = 0;
	mtr_flush();
	fwrite("\n]}\n", 1, 4, f);
	fclose(f);
	pthread_mutex_destroy(&mutex);
	f = 0;
	free(buffer);
	buffer = 0;
	for (i = 0; i < STRING_POOL_SIZE; i++) {
		if (str_pool[i]) {
			free(str_pool[i]);
			str_pool[i] = 0;
		}
	}
}

const char *mtr_pool_string(const char *str) {
	int i;
	for (i = 0; i < STRING_POOL_SIZE; i++) {
		if (!str_pool[i]) {
			str_pool[i] = (char*)malloc(strlen(str) + 1);
			strcpy(str_pool[i], str);
			return str_pool[i];
		} else {
			if (!strcmp(str, str_pool[i]))
				return str_pool[i];
		}
	}
	return "string pool full";
}

void mtr_start() {
#ifndef MTR_ENABLED
	return;
#endif
	is_tracing = 1;
}

void mtr_stop() {
#ifndef MTR_ENABLED
	return;
#endif
	is_tracing = 0;
}

// TODO: fwrite more than one line at a time.
void mtr_flush() {
#ifndef MTR_ENABLED
	return;
#endif
	int i = 0;
	char linebuf[1024];
	char arg_buf[1024];
	char id_buf[256];
	// We have to lock while flushing. So we really should avoid flushing as much as possible.

	pthread_mutex_lock(&mutex);
	int old_tracing = is_tracing;
	is_tracing = 0;	// Stop logging even if using interlocked increments instead of the mutex. Can cause data loss.

	for (i = 0; i < count; i++) {
		raw_event_t *raw = &buffer[i];
		int len;
		switch (raw->arg_type) {
		case MTR_ARG_TYPE_INT:
			snprintf(arg_buf, ARRAY_SIZE(arg_buf), "\"%s\":%i", raw->arg_name, raw->a_int);
			break;
		case MTR_ARG_TYPE_STRING_CONST:
			snprintf(arg_buf, ARRAY_SIZE(arg_buf), "\"%s\":\"%s\"", raw->arg_name, raw->a_str);
			break;
		case MTR_ARG_TYPE_STRING_COPY:
			if (strlen(raw->a_str) > 700) {
				snprintf(arg_buf, ARRAY_SIZE(arg_buf), "\"%s\":\"%.*s\"", raw->arg_name, 700, raw->a_str);
			} else {
				snprintf(arg_buf, ARRAY_SIZE(arg_buf), "\"%s\":\"%s\"", raw->arg_name, raw->a_str);
			}
			break;
		case MTR_ARG_TYPE_NONE:
			arg_buf[0] = '\0';
			break;
		}
		if (raw->id) {
			switch (raw->ph) {
			case 'S':
			case 'T':
			case 'F':
				// TODO: Support full 64-bit pointers
				snprintf(id_buf, ARRAY_SIZE(id_buf), ",\"id\":\"0x%08x\"", (uint32_t)(uintptr_t)raw->id);
				break;
			case 'X':
				snprintf(id_buf, ARRAY_SIZE(id_buf), ",\"dur\":%i", (int)raw->a_double);
				break;
			}
		} else {
			id_buf[0] = 0;
		}
		const char *cat = raw->cat;
#ifdef _WIN32
		// On Windows, we often end up with backslashes in category.
		char temp[256];
		{
			int len = (int)strlen(cat);
			int i;
			if (len > 255) len = 255;
			for (i = 0; i < len; i++) {
				temp[i] = cat[i] == '\\' ? '/' : cat[i];
			}
			temp[len] = 0;
			cat = temp;
		}
#endif

		len = snprintf(linebuf, ARRAY_SIZE(linebuf), "%s{\"cat\":\"%s\",\"pid\":%i,\"tid\":%i,\"ts\":%" PRId64 ",\"ph\":\"%c\",\"name\":\"%s\",\"args\":{%s}%s}",
				first_line ? "" : ",\n",
				cat, raw->pid, raw->tid, raw->ts - time_offset, raw->ph, raw->name, arg_buf, id_buf);
		fwrite(linebuf, 1, len, f);
		first_line = 0;
	}
	count = 0;
	is_tracing = old_tracing;
	pthread_mutex_unlock(&mutex);
}

void internal_mtr_raw_event(const char *category, const char *name, char ph, void *id) {
#ifndef MTR_ENABLED
	return;
#endif
	if (!is_tracing || count >= INTERNAL_MINITRACE_BUFFER_SIZE)
		return;
	double ts = mtr_time_s();
	if (!cur_thread_id) {
		cur_thread_id = get_cur_thread_id();
	}
	if (!cur_process_id) {
		cur_process_id = get_cur_process_id();
	}

#if 0 && _WIN32	// This should work, feel free to enable if you're adventurous and need performance.
	int bufPos = InterlockedExchangeAdd((LONG volatile *)&count, 1);
	raw_event_t *ev = &buffer[bufPos];
#else
	pthread_mutex_lock(&mutex);
	raw_event_t *ev = &buffer[count];
	count++;
	pthread_mutex_unlock(&mutex);
#endif

	ev->cat = category;
	ev->name = name;
	ev->id = id;
	ev->ph = ph;
	if (ev->ph == 'X') {
		double x;
		memcpy(&x, id, sizeof(double));
		ev->ts = (int64_t)(x * 1000000);
		ev->a_double = (ts - x) * 1000000;
	} else {
		ev->ts = (int64_t)(ts * 1000000);
	}
	ev->tid = cur_thread_id;
	ev->pid = cur_process_id;
	ev->arg_type = MTR_ARG_TYPE_NONE;
}

void internal_mtr_raw_event_arg(const char *category, const char *name, char ph, void *id, mtr_arg_type arg_type, const char *arg_name, void *arg_value) {
#ifndef MTR_ENABLED
	return;
#endif
	if (!is_tracing || count >= INTERNAL_MINITRACE_BUFFER_SIZE)
		return;
	if (!cur_thread_id) {
		cur_thread_id = get_cur_thread_id();
	}
	if (!cur_process_id) {
		cur_process_id = get_cur_process_id();
	}
	double ts = mtr_time_s();

#if 0 && _WIN32	// This should work, feel free to enable if you're adventurous and need performance.
	int bufPos = InterlockedExchangeAdd((LONG volatile *)&count, 1);
	raw_event_t *ev = &buffer[bufPos];
#else
	pthread_mutex_lock(&mutex);
	raw_event_t *ev = &buffer[count];
	count++;
	pthread_mutex_unlock(&mutex);
#endif

	ev->cat = category;
	ev->name = name;
	ev->id = id;
	ev->ts = (int64_t)(ts * 1000000);
	ev->ph = ph;
	ev->tid = cur_thread_id;
	ev->pid = cur_process_id;
	ev->arg_type = arg_type;
	ev->arg_name = arg_name;
	switch (arg_type) {
	case MTR_ARG_TYPE_INT: ev->a_int = (int)(uintptr_t)arg_value; break;
	case MTR_ARG_TYPE_STRING_CONST:	ev->a_str = (const char*)arg_value; break;
	case MTR_ARG_TYPE_STRING_COPY: ev->a_str = strdup((const char*)arg_value); break;
	case MTR_ARG_TYPE_NONE: break;
	}
}

#ifdef __cplusplus

#ifdef MTR_DEAR_IMGUI
// Flame graph type rendering of profile data using Dear Imgui
#include "imgui.h"

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep(x/1000)
#else
#include <unistd.h>
#endif

// Linked list for events
//
// Used for querying overlapping intervals.
typedef struct IntrvlListNode {
	raw_event* event;
	struct IntrvlListNode *next;
} IntrvlListNode;

IntrvlListNode* IntrvlListNodeCreate () {
	return (IntrvlListNode*) calloc (sizeof (IntrvlListNode), 1);
}

void IntrvlListNodeDestroy (IntrvlListNode* node) {
	if (node == NULL) {
		return;
	}

	IntrvlListNodeDestroy (node->next);
	node->next = NULL;
	free (node);
}



#define USE_AVL_TREE

// Augmented Tree that allows querying for overlapping intervals
//
typedef struct IntrvlTreeNode {
	struct IntrvlTreeNode *left;
	struct IntrvlTreeNode *right;
	raw_event* event;
	int64_t max_upper;
#ifdef USE_AVL_TREE
	int height;
#endif
} IntrvlTreeNode;

typedef struct IntrvlTreeNodeAllocNode {
	struct IntrvlTreeNode* nodes;
	struct IntrvlTreeNodeAllocNode* next;
} IntrvlTreeNodeAllocNode;

typedef struct IntrvlTreeNodeAllocData {
	struct IntrvlTreeNodeAllocNode* blocks;
	struct IntrvlTreeNodeAllocNode* cur_block;
	int cur_idx;
	int block_size;
	int block_count;
} IntrvlTreeNodeAllocData;

static IntrvlTreeNodeAllocData intrvl_tree_node_alloc_data = { NULL, NULL, 0, 1024, 0 };

IntrvlTreeNode* AllocIntrvlTreeNode () {
	IntrvlTreeNodeAllocData* alloc_data = &intrvl_tree_node_alloc_data;

	if (alloc_data->cur_block == NULL 
			|| alloc_data->cur_idx == alloc_data->block_size) {
		IntrvlTreeNodeAllocNode* new_block = (IntrvlTreeNodeAllocNode*) calloc (sizeof (IntrvlTreeNodeAllocNode), 1);
		new_block->next = alloc_data->blocks;
		alloc_data->blocks = new_block;
		alloc_data->block_count++;

		alloc_data->cur_block = new_block;
		alloc_data->cur_block->nodes = (IntrvlTreeNode*) calloc (sizeof (IntrvlTreeNode), alloc_data->block_size);
		alloc_data->cur_idx = 0;
	}

	return &alloc_data->cur_block->nodes[alloc_data->cur_idx++];
}

void AllocIntrvlDestroy () {
	IntrvlTreeNodeAllocData* alloc_data = &intrvl_tree_node_alloc_data;
	IntrvlTreeNodeAllocNode* alloc_node = alloc_data->blocks;

	IntrvlTreeNodeAllocNode* temp_node = NULL;
	while (alloc_node != NULL) {
		temp_node = alloc_node->next;
		free (alloc_node->nodes);
		free (alloc_node);
		alloc_node = temp_node;
	}
}

IntrvlTreeNode* IntrvlTreeNodeCreate () {
	IntrvlTreeNode* result = AllocIntrvlTreeNode();
#ifdef USE_AVL_TREE
	result->height = 1;
#endif
	return result;
}

void IntrvlTreeNodeDestroy (IntrvlTreeNode* node) {
	if (node == NULL) {
		return;
	}

	IntrvlTreeNodeDestroy (node->left);
	node->left = NULL;

	IntrvlTreeNodeDestroy (node->right);
	node->right = NULL;

	free (node);
}

#ifdef USE_AVL_TREE
int IntrvlTreeNodeHeight (IntrvlTreeNode* node) {
	if (node == NULL) {
		return 0;
	}
	return node->height;
}

struct IntrvlTreeNode* IntrvlTreeRightRot (IntrvlTreeNode* z) {
	struct IntrvlTreeNode* y = z->left;
	struct IntrvlTreeNode* T2 = y->right;

	y->right = z;
	y->max_upper = y->event->ts + (int) y->event->a_double;

	z->left = T2;
	z->max_upper = z->event->ts + (int) z->event->a_double;

	int y_left_height = IntrvlTreeNodeHeight(z->left);
	int y_right_height = IntrvlTreeNodeHeight(z->right);
	z->height = y_left_height > y_right_height + 1 ? y_left_height : y_right_height + 1;
	if (z->left && z->left->max_upper > z->max_upper) {
		z->max_upper = z->left->max_upper;
	}
	if (z->right && z->right->max_upper > z->max_upper) {
		z->max_upper = z->right->max_upper;
	}

	int x_left_height = IntrvlTreeNodeHeight(y->left);
	int x_right_height = IntrvlTreeNodeHeight(y->right);
	y->height = x_left_height > x_right_height + 1 ? x_left_height : x_right_height + 1;
	if (y->left && y->left->max_upper > y->max_upper) {
		y->max_upper = y->left->max_upper;
	}
	if (y->right && y->right->max_upper > y->max_upper) {
		y->max_upper = y->right->max_upper;
	}

	return y;
}

struct IntrvlTreeNode* IntrvlTreeLeftRot (IntrvlTreeNode* z) {
	struct IntrvlTreeNode* y = z->right;
	struct IntrvlTreeNode* T2 = y->left;

	y->left = z;
	y->max_upper = y->event->ts + (int) y->event->a_double;

	z->right = T2;
	z->max_upper = z->event->ts + (int) z->event->a_double;

	int x_left_height = IntrvlTreeNodeHeight(z->left);
	int x_right_height = IntrvlTreeNodeHeight(z->right);
	z->height = x_left_height > x_right_height + 1 ? x_left_height : x_right_height + 1;
	if (z->left && z->left->max_upper > z->max_upper) {
		z->max_upper = z->left->max_upper;
	}
	if (z->right && z->right->max_upper > z->max_upper) {
		z->max_upper = z->right->max_upper;
	}

	int y_left_height = IntrvlTreeNodeHeight(y->left);
	int y_right_height = IntrvlTreeNodeHeight(y->right);
	y->height = y_left_height > y_right_height + 1 ? y_left_height : y_right_height + 1;
	if (y->left && y->left->max_upper > y->max_upper) {
		y->max_upper = y->left->max_upper;
	}
	if (y->right && y->right->max_upper > y->max_upper) {
		y->max_upper = y->right->max_upper;
	}

	return y;
}

struct IntrvlTreeNode* IntrvlTreeNodeAddEvent (IntrvlTreeNode* node, raw_event* event) {
	if (node == NULL) {
		node = IntrvlTreeNodeCreate();
		node->max_upper = event->ts + (int64_t)event->a_double;
		node->event = event;
		node->height = 1;
		return node;
	}

	if (event->ts < node->event->ts) {
		node->left = IntrvlTreeNodeAddEvent (node->left, event);
		if (node->left->max_upper > node->max_upper) {
			node->max_upper = node->left->max_upper;
		}
	} else {
		node->right = IntrvlTreeNodeAddEvent (node->right, event);
		if (node->right->max_upper > node->max_upper) {
			node->max_upper = node->right->max_upper;
		}
	}

	int left_height = IntrvlTreeNodeHeight(node->left);
	int right_height = IntrvlTreeNodeHeight(node->right);

	node->height = 1 + (left_height > right_height ? left_height : right_height);
	int balance = left_height - right_height;

	if (balance > 1 && node->event->ts > node->left->event->ts) {
		return IntrvlTreeRightRot(node);
	}

	if (balance < -1 && node->event->ts < node->right->event->ts) {
		return IntrvlTreeLeftRot(node);
	}

	if (balance > 1 && node->event->ts < node->left->event->ts) {
		node->left = IntrvlTreeLeftRot(node->left);
		return IntrvlTreeRightRot(node);
	}

	if (balance < -1 && node->event->ts > node->right->event->ts) {
		node->right = IntrvlTreeRightRot(node->right);
		return IntrvlTreeLeftRot(node);
	}

	return node;
}
#else
void IntrvlTreeNodeAddEvent (IntrvlTreeNode** node, raw_event* event) {
	if (*node == NULL) {
		*node = IntrvlTreeNodeCreate();
		(*node)->max_upper = event->ts + (int64_t)event->a_double;
		(*node)->event = event;
	} else if (event->ts < (*node)->event->ts) {
		IntrvlTreeNodeAddEvent (&(*node)->left, event);
		if ((*node)->left->max_upper > (*node)->max_upper) {
			(*node)->max_upper = (*node)->left->max_upper;
		}
	} else {
		IntrvlTreeNodeAddEvent (&(*node)->right, event);
		if ((*node)->right->max_upper > (*node)->max_upper) {
			(*node)->max_upper = (*node)->right->max_upper;
		}
	}
}
#endif

void IntrvlTreeNodeQuery (IntrvlTreeNode* node, int64_t start, int64_t end, IntrvlListNode* head) {
	if (node == NULL) {
		return;
	}

  if (node->event->ts <= end && node->event->ts + (int) node->event->a_double >= start) {
		IntrvlListNode* list_node = IntrvlListNodeCreate();
		list_node->next = head->next;
		head->next = list_node;
		list_node->event = node->event;
	}

	if (node->right && node->right->event->ts <= end) {
    IntrvlTreeNodeQuery(node->right, start, end, head);
  }

	if (node->left && node->left->max_upper >= start) {
	  IntrvlTreeNodeQuery (node->left, start, end, head);
  }
}

int IntrvlTreeNodeCountEnclosing (IntrvlTreeNode* node, int64_t ts) {
  if (node == NULL) {
    return 0;
  }

  int interval_count = 0;

  if (node->event->ts <= ts && node->event->ts + (int) node->event->a_double >= ts) {
    interval_count = 1;
  }

  if (node->right && node->right->event->ts <= ts) {
    interval_count += IntrvlTreeNodeCountEnclosing(node->right, ts);
  }

  if (node->left && node->left->max_upper >= ts) {
    interval_count += IntrvlTreeNodeCountEnclosing(node->left, ts);
  }

  return interval_count;
}

void IntrvlTreeNodePrint (IntrvlTreeNode* node, uint64_t time_offset) {
	if (node == NULL) {
		printf ("Null");
		return;
	}

	IntrvlTreeNodePrint (node->left, time_offset);

	printf ("Event %s [%ld,%ld] height %d, max_upper %ld", node->event->name, node->event->ts - time_offset,
			node->event->ts + (int) node->event->a_double - time_offset,
			node->height,
			node->max_upper - time_offset);

	IntrvlTreeNodePrint (node->right, time_offset);
}

int IntrvlTreeNodeDepth (IntrvlTreeNode* node) {
	if (node == NULL)
		return 0;

	int left_depth = 1 + IntrvlTreeNodeDepth (node->left);
	int right_depth = 1 + IntrvlTreeNodeDepth (node->right);


	return left_depth > right_depth ? left_depth : right_depth;
}

int CalcIntrvlDepth (raw_event* event, IntrvlTreeNode* tree_node) {
  return IntrvlTreeNodeCountEnclosing (tree_node, event->ts) - 1;
}

void c() {
	MTR_SCOPE("c++", "c()");
	usleep(10);
}

void b() {
	MTR_SCOPE("c++", "b()");
	usleep(20);
	c();
	usleep(10);
}

void a() {
	MTR_SCOPE("c++", "a()");
	usleep(20);
	b();
	usleep(10);
}

void mtr_reinit() {
    fseek (f, 0, SEEK_SET);
    mtr_init_from_stream(f);
}

void generate_trace_data() {
    mtr_reinit();
    int i;

//	MTR_META_PROCESS_NAME("minitrace_test");
//	MTR_META_THREAD_NAME("main thread");
//
//	int long_running_thing_1;
//	int long_running_thing_2;
//
//	MTR_START("background", "long_running", &long_running_thing_1);
//	MTR_START("background", "long_running", &long_running_thing_2);

//	MTR_COUNTER("main", "greebles", 3);
//	MTR_BEGIN("main", "outer");
//	usleep(80000);
//	for (i = 0; i < 2; i++) {
//		MTR_BEGIN("main", "inner");
//		usleep(4000);
//		MTR_END("main", "inner");
//		usleep(1000);
//        a();
//		MTR_COUNTER("main", "greebles", 3 * i + 10);
//	}

	for (i = 0; i < 2; i++) {
		MTR_BEGIN("main", "inner");
		a();
		usleep(10);
		MTR_END("main", "inner");
		usleep(20);
//		MTR_COUNTER("main", "greebles", 3 * i + 10);
	}

	//    MTR_BEGIN ("main", "tinyblock");
//	for (i = 0; i < 3; i++) {
//		MTR_BEGIN("main", "tiny");
//		usleep(200);
//		MTR_END("main", "tiny");
//		usleep(100);
//	}
//    MTR_END ("main", "tinyblock");
//	MTR_STEP("background", "long_running", &long_running_thing_1, "middle step");
//	usleep(80000);
//	MTR_END("main", "outer");
//	MTR_COUNTER("main", "greebles", 0);

//	usleep(10000);
//	a();

//    usleep(10000);
//	MTR_BEGIN("main", "wrap_a");
//    usleep(1300);
//	a();
//	MTR_END("main", "wrap_a");


//	usleep(50000);
//	MTR_INSTANT("main", "the end");
//	usleep(10000);
//	MTR_FINISH("background", "long_running", &long_running_thing_1);
//	MTR_FINISH("background", "long_running", &long_running_thing_2);
}

typedef struct gui_draw_data {
	IntrvlTreeNode* interval_tree_root;
	int is_processed;
	int64_t ts_start;
	int64_t ts_end;
	raw_event_t* clicked_item;
	double dur_generation;
	double dur_processing;
	double dur_intrvl_merge;
	double dur_build_tree;
	double dur_calc_depths;
	int interval_count;
} gui_draw_data;

static struct gui_draw_data _draw_data = { NULL, 0, 0, 0};

void gui_draw_data_reset () {
	if (draw_data == NULL) {
		return;
	}

	AllocIntrvlDestroy ();

	memset (draw_data, 0, sizeof (gui_draw_data));
	draw_data->interval_tree_root = NULL;
	draw_data->clicked_item = NULL;
	draw_data->is_processed = 0.;
	draw_data->dur_generation = 0.;
	draw_data->dur_processing = 0.;
	draw_data->dur_intrvl_merge = 0.;
	draw_data->dur_build_tree = 0.;
	draw_data->dur_calc_depths = 0.;
	draw_data->interval_count = 0;
}

#define INTERVAL_SEARCH_BUFFER_SIZE 10000
raw_event_t* intrvl_srch_buffer[INTERVAL_SEARCH_BUFFER_SIZE];
int intrvl_srch_buf_idx = 0;

void mtr_imgui_preprocess () {
	if (draw_data == NULL) {
		draw_data = &_draw_data;
	}

	if (draw_data->is_processed)
		return;

	printf ("Starting processing of trace data...");
	draw_data->dur_processing = mtr_time_s();

	double ts_intrvl_merge_start = mtr_time_s();
	intrvl_srch_buf_idx = -1;

	// Preprocess: replace start (S,B) and end events (F,E) with complete events (X)
	raw_event_t *start_event = NULL;
	for (int i = 0; i < count; i++) {
		raw_event_t *raw_i = &buffer[i];

		if (raw_i->ph == 'S' || raw_i->ph == 'B') {
			for (int j = i + 1; j < count; j++) {
				raw_event_t *raw_j = &buffer[j];
				if (raw_i->name == raw_j->name
						&& raw_i->cat == raw_j->cat
						&& raw_i->id == raw_j->id
						&& raw_i->pid == raw_j->pid
						&& raw_i->tid == raw_j->tid
						&& (raw_j->ph == 'F' || raw_j->ph == 'E')) { 
					raw_i->ph = 'X';
					raw_i->a_double = (raw_j->ts - raw_i->ts);
					raw_j->ph = '_';

					break;
				}
			}
		}


#ifdef USE_SEARCH_BUFFER
		if (raw->ph == 'S' || raw->ph == 'B') {
			intrvl_srch_buffer[intrvl_srch_buf_idx++] = raw;
			assert (intrvl_srch_buf_idx < INTERVAL_SEARCH_BUFFER_SIZE);

			continue;
		}

		if (raw->ph == 'F' || raw->ph == 'E') {
			for (int j = intrvl_srch_buf_idx - 1; j >= 0; j--) {
				raw_event_t* raw_start_buf = intrvl_srch_buffer[j];

				if (raw->name == raw_start_buf->name
						&& raw->cat == raw_start_buf->cat
						&& raw->id == raw_start_buf->id
						&& raw->pid == raw_start_buf->pid
						&& raw->tid == raw_start_buf->tid
					 ) {
					raw_start_buf->ph = 'X';
					raw_start_buf->a_double = (raw->ts - raw_start_buf->ts);

					// Invalidate the current one
					raw->ph = '_';

					// Remove start entry by swapping it with the last added
					intrvl_srch_buffer[j] = intrvl_srch_buffer[intrvl_srch_buf_idx];
					intrvl_srch_buffer[intrvl_srch_buf_idx] = NULL;
					intrvl_srch_buf_idx--;

					break;
				}
			}
		}
#endif
	}

	draw_data->dur_intrvl_merge = mtr_time_s() - ts_intrvl_merge_start;

	int64_t ts_start = 0;
	int64_t ts_end = 0;

	if (count > 0) {
		ts_start = buffer[0].ts;
	}

	double ts_build_tree = mtr_time_s();
	// Sort events into the interval tree
	for (int i = 0; i < count; i++) {
		raw_event_t *raw = &buffer[i];

		int64_t raw_end = raw->ts + (int) raw->a_double;
		switch (raw->ph) {
			case 'X': ts_start = ts_start > raw->ts ? raw->ts : ts_start;
								ts_end = ts_end < raw_end ? raw_end: ts_end; break;
								break;
			default: break;
		}

		if (raw->ph == 'X') {
#ifdef USE_AVL_TREE
			draw_data->interval_tree_root = IntrvlTreeNodeAddEvent (draw_data->interval_tree_root, raw);
#else
			IntrvlTreeNodeAddEvent (&draw_data->interval_tree_root, raw);
#endif
			draw_data->interval_count++;
		}
	}
	draw_data->dur_build_tree = mtr_time_s() - ts_build_tree;

	IntrvlTreeNodePrint (draw_data->interval_tree_root, ts_start);

	// Compute the depth of the events
	double ts_calc_depths = mtr_time_s();
	for (int i = 0; i < count; i++) {
		raw_event_t *raw = &buffer[i];
		uint32_t row_index = CalcIntrvlDepth (raw, draw_data->interval_tree_root);

		raw->pid = row_index;
	}
	draw_data->dur_calc_depths = mtr_time_s() - ts_calc_depths;

	draw_data->ts_start = ts_start;
	draw_data->ts_end = ts_end;

	draw_data->dur_processing = mtr_time_s() - draw_data->dur_processing;
	draw_data->is_processed = 1;
	printf ("Trace data processed!");

}

void mtr_imgui_draw() {
	ImGui::BeginChild ("ProfilerWrapper");

	// Header
	ImGui::SameLine();
	if (ImGui::Button ("Generate Data")) {
		printf ("Generating new profiler data...");
		double ts_gen_start = mtr_time_s();
		generate_trace_data();
		draw_data->dur_generation = mtr_time_s() - ts_gen_start;
		printf ("New profiler data generated!");
	}
	ImGui::SameLine();
	ImGui::Text ("%d Events", count);
	ImGui::SameLine();
	if (ImGui::Button ("Flush Data")) {
		mtr_flush();
	}
	bool reset_zoom = 0;
	ImGui::SameLine();
	if (ImGui::Button ("Reset Zoom")) {
		reset_zoom = 1;
		draw_data->clicked_item = NULL;
	}

	mtr_imgui_preprocess();

	int64_t ts_start = draw_data->ts_start;
	int64_t ts_end = draw_data->ts_end;

	if (draw_data->clicked_item) {
		ts_start = draw_data->clicked_item->ts;
		ts_end = draw_data->clicked_item->ts + (int) draw_data->clicked_item->a_double;
	}

	ImGui::Text ("Start %ld end %ld duration %ld size %d clicked %p", 
			ts_start, ts_end, 
			ts_end - ts_start, count * sizeof(raw_event),
			draw_data->clicked_item);
	ImGui::Text ("Tree: depth %d", IntrvlTreeNodeDepth (draw_data->interval_tree_root));

	// Content
	float row_height = ImGui::GetTextLineHeight();

	ImGui::BeginChild("ProfilerContent");
	const ImVec2 position = ImGui::GetWindowPos();
	const ImVec2 size = ImGui::GetWindowSize();
	const ImVec4 clip_rect (position.x, position.y,
			position.x + size.x, position.y + size.y - 2 * row_height);

	float pixel_per_ns = (float) (size.x / (ts_end - ts_start));
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	int row_index = 0;

	double dur_stats_render = mtr_time_s();
	raw_event_t *hovered_item = NULL;
	for (int i = 0; i < count; i++) {
		raw_event_t *raw = &buffer[i];

		if (raw->ph == 'X') {
			float width = ((int) raw->a_double) * pixel_per_ns;

			if (raw->ts > ts_end
					|| raw->ts + (int) raw->a_double < ts_start
					|| width < 1.0f)
				continue;

			row_index = raw->pid;
			const ImVec2 rect_start (position.x + (raw->ts - ts_start) * (pixel_per_ns), position.y + row_index * row_height);
			const ImVec2 rect_end (position.x + (raw->ts + (int) raw->a_double - ts_start) * pixel_per_ns, position.y + (row_index + 1) * row_height);

			ImGui::SetCursorScreenPos(ImVec2(rect_start.x, rect_start.y));
			float height = rect_end.y - rect_start.y;
			ImGui::Dummy (ImVec2(width, height));

			draw_list->AddRectFilled (rect_start, rect_end, ImColor (0.5f, 0.4f + 0.05 * row_index, 0.4f + 0.05 * row_index, 0.92f));
			if (ImGui::IsItemHovered()) {
				draw_list->AddRectFilled (rect_start, rect_end, ImColor (1.0f, 0.f, 0.f));
				hovered_item = raw;
			}

			if (ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0)) {
				draw_data->clicked_item = raw;
			}

			const ImVec2 text_pos (rect_start.x, rect_start.y);
			const ImVec4 text_clip (text_pos.x, text_pos.y, rect_end.x, rect_end.y);
			draw_list->AddText (ImGui::GetFont(), ImGui::GetFontSize(), text_pos, ImColor (1.0f, 1.0f, 1.0f), raw->name, raw->name + strlen(raw->name), 0.0f, &text_clip); 
		}
	}
	dur_stats_render = mtr_time_s() - dur_stats_render;
	ImGui::EndChild(); // Content

	ImGui::SetCursorScreenPos(ImVec2(position.x, position.y + 15 * row_height));
	ImGui::Spacing();
	ImGui::Separator();

	double ts_stats_start = mtr_time_s();
	if (hovered_item != NULL) {
		double dur_sum = 0., dur_max = 0., dur_min = 0.;
		int raw_count = 0;
		for (int i = 0; i < count; i++) {
			raw_event_t *raw = &buffer[i];
			if (hovered_item->name == raw->name
					&& hovered_item->cat == raw->cat
					//                    && hovered_item->pid == raw->pid
					&& hovered_item->tid == raw->tid
					&& raw->ph == 'X') {
				if (raw_count == 0) {
					dur_min = raw->a_double;
				}

				raw_count++;
				dur_sum += raw->a_double;
				dur_max = raw->a_double > dur_max ? raw->a_double : dur_max;
				dur_min = raw->a_double < dur_min ? raw->a_double : dur_min;
			}
		}
		ImGui::Text("%s (count: %d)\nDur: %7.4f\nAvg: %7.4f\nMin: %7.4f\nMax: %7.4f",
				hovered_item->name,
				raw_count,
				hovered_item->a_double * 1.0e-6,
				(dur_sum / raw_count) * 1.0e-6, dur_min * 1.0e-6, dur_max * 1.0e-6);
	}
	ImGui::Text("Intervals %d\nGeneration %7.4f\nProcessing %7.4f\nMerge Intervals %7.4f\nBuild Tree %7.4f\nCalc Depths %7.4f\nRender %7.4f\nStats %7.4f\nNode Blocks %d\n", 
			draw_data->interval_count,
			draw_data->dur_generation,
			draw_data->dur_processing,
			draw_data->dur_intrvl_merge,
			draw_data->dur_build_tree,
			draw_data->dur_calc_depths,
			dur_stats_render,
			(mtr_time_s() - ts_stats_start),
			intrvl_tree_node_alloc_data.block_count);

	ImGui::EndChild(); // ProfilerWrapper

}
#endif

#endif
