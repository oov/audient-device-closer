#include "async.c"

#include <ovtest.h>

/** @brief What the bodies left behind, read once their thread was waited for */
static struct {
  bool body_ran;    //!< the body was reached
  bool done_ran;    //!< the end was called
  bool saw_stop;    //!< the body noticed the ask to stop
  bool stop_called; //!< the call named when the operation was started was made
} g_probe;

/** @brief The event a body waits on, made by the test on its behalf */
static HANDLE g_event = NULL;

/** @brief How long a test waits for what it started, before it says it did not happen */
enum { kTestWaitMs = 5000 };

/**
 * @brief What the caller is told, either way
 *
 * One call for both moments: the ask to stop, made on the thread that asked, and the body
 * being over, made on the thread of the operation.
 *
 * @param userdata unused
 * @param stopped true when the caller asked the operation to stop
 */
static void note(void *const userdata, bool const stopped) {
  (void)userdata;
  if (stopped) {
    g_probe.stop_called = true;
    SetEvent(g_event);
    return;
  }
  g_probe.done_ran = true;
}

static void body_mark(struct async_context *const ctx) {
  (void)ctx;
  g_probe.body_ran = true;
}

static void body_wait_until_stopping(struct async_context *const ctx) {
  (void)ctx;
  if (WaitForSingleObject(g_event, kTestWaitMs) == WAIT_OBJECT_0) {
    g_probe.saw_stop = true;
  }
}

static void body_says_whether_it_was_stopped(struct async_context *const ctx) { g_probe.saw_stop = ctx->stopped(ctx); }

/**
 * @brief A body runs on its own thread and the end is called from there
 */
static void test_the_body_runs_and_the_end_is_called(void) {
  struct ov_error err = {0};
  struct async *a = NULL;

  g_probe.body_ran = false;
  g_probe.done_ran = false;
  a = async_start(body_mark, NULL, note, &err);
  if (!TEST_SUCCEEDED(a != NULL, &err)) {
    return;
  }
  TEST_SUCCEEDED(async_release(&a, &err), &err);

  TEST_CASE("the body was reached");
  TEST_CHECK(g_probe.body_ran);
  TEST_CASE("the end was called");
  TEST_CHECK(g_probe.done_ran);
  TEST_CASE_(NULL);
  TEST_CHECK(a == NULL);
}

/**
 * @brief An operation without a body is refused
 */
static void test_a_body_is_required(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(async_start(NULL, NULL, NULL, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief The call named when the operation was started is made when it is asked to stop
 *
 * It is named there and nowhere else, so there is no moment in which the operation can be
 * asked to stop and nothing is there to hear it.
 */
static void test_the_ask_to_stop_makes_the_call(void) {
  struct ov_error err = {0};
  struct async *a = NULL;

  g_probe.saw_stop = false;
  g_probe.stop_called = false;
  g_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(g_event != NULL)) {
    return;
  }
  a = async_start(body_wait_until_stopping, NULL, note, &err);
  if (!TEST_SUCCEEDED(a != NULL, &err)) {
    CloseHandle(g_event);
    g_event = NULL;
    return;
  }
  async_cancel(a);
  TEST_SUCCEEDED(async_release(&a, &err), &err);
  CloseHandle(g_event);
  g_event = NULL;
  TEST_CHECK(g_probe.stop_called);
  TEST_CHECK(g_probe.saw_stop);
}

/**
 * @brief A body that asks instead of waiting is told the same way
 *
 * An operation started without a call is one whose body only asks: it is told the answer,
 * and nothing is made for the ask.
 */
static void test_a_body_that_only_asks_is_told(void) {
  struct ov_error err = {0};
  struct async *a = NULL;

  g_probe.saw_stop = false;
  g_probe.stop_called = false;
  a = async_start(body_says_whether_it_was_stopped, NULL, NULL, &err);
  if (!TEST_SUCCEEDED(a != NULL, &err)) {
    return;
  }
  async_cancel(a);
  TEST_SUCCEEDED(async_release(&a, &err), &err);
  TEST_CHECK(g_probe.saw_stop);
  TEST_CHECK(!g_probe.stop_called); // nothing was named, so nothing was made
}

/**
 * @brief Releasing what is not there must not crash
 */
static void test_releasing_nothing_is_allowed(void) {
  struct ov_error err = {0};
  struct async *a = NULL;

  TEST_SUCCEEDED(async_release(&a, &err), &err);
  TEST_SUCCEEDED(async_release(NULL, &err), &err);
  TEST_CHECK(a == NULL);
}

TEST_LIST = {
    {"body_runs_and_the_end_is_called", test_the_body_runs_and_the_end_is_called},
    {"a_body_is_required", test_a_body_is_required},
    {"the_ask_to_stop_makes_the_call", test_the_ask_to_stop_makes_the_call},
    {"a_body_that_only_asks_is_told", test_a_body_that_only_asks_is_told},
    {"releasing_nothing_is_allowed", test_releasing_nothing_is_allowed},
    {NULL, NULL},
};
