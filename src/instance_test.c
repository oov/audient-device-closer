/**
 * Tests for the mutex that marks the single running copy of this program.
 *
 * The rule these check is the one a person at the machine feels: starting the program twice
 * opens one window, not two.  A test takes a name of its own, so a copy of the program that
 * happens to run while the tests run cannot answer for the copy under test.
 */
#include <ovtest.h>

#include <windows.h>

#include "instance.h"

static wchar_t const test_mutex[] = L"audient-device-closer-test.Mutex";

/**
 * @brief The first copy to take the mutex is the running one, the copy after it is not
 */
static void test_take_marks_the_first_copy(void) {
  struct ov_error err = {0};
  HANDLE first = NULL;
  HANDLE second = NULL;
  bool other_running = true;

  TEST_CASE("the first copy finds nobody");
  other_running = true;
  first = instance_take(test_mutex, &other_running, &err);
  if (!TEST_CHECK(first != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(other_running == false);

  TEST_CASE("the copy that comes after finds the first one");
  other_running = false;
  second = instance_take(test_mutex, &other_running, &err);
  if (!TEST_CHECK(second != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(other_running == true);

  TEST_CASE("the name is free again once both copies let it go");
  instance_release(&second);
  instance_release(&first);
  other_running = true;
  first = instance_take(test_mutex, &other_running, &err);
  if (!TEST_CHECK(first != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(other_running == false);

  TEST_CASE_(NULL);

cleanup:
  instance_release(&second);
  instance_release(&first);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief A take without the room for its answer is refused
 */
static void test_refused_arguments(void) {
  struct ov_error err = {0};
  bool other_running = false;

  TEST_CASE("the answer of the take has to be somewhere to go");
  TEST_FAILED_WITH(instance_take(test_mutex, NULL, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  TEST_CASE("a mutex without a name is not a mutex");
  TEST_FAILED_WITH(instance_take(NULL, &other_running, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  TEST_CASE_(NULL);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief Handing back accepts a handle that is already gone
 */
static void test_release_accepts_nothing(void) {
  instance_release(NULL);
  {
    HANDLE none = NULL;
    instance_release(&none);
    TEST_CHECK(none == NULL);
  }
}

TEST_LIST = {
    {"take_marks_the_first_copy", test_take_marks_the_first_copy},
    {"refused_arguments", test_refused_arguments},
    {"release_accepts_nothing", test_release_accepts_nothing},
    {NULL, NULL},
};
