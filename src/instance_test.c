/**
 * Tests for the mutex that marks the single running copy of this program.
 *
 * The rule these check is the one a person at the machine feels: starting the program twice
 * opens one window, not two.  A test takes a name of its own, so a copy of the program that
 * happens to run while the tests run cannot answer for the copy under test.
 */
#include "instance.c"

#include <ovtest.h>

#include <windows.h>

static wchar_t const test_mutex[] = L"audient-device-closer-test.Mutex";

/**
 * @brief The first copy to take the mutex is the running one, the copy after it is told so
 */
static void test_take_marks_the_first_copy(void) {
  struct ov_error err = {0};
  struct instance first = {0};
  struct instance second = {0};

  TEST_CASE_("the first copy takes the mutex");
  first = take_named(test_mutex, &err);
  if (!TEST_SUCCEEDED(first.handle != NULL, &err)) {
    goto cleanup;
  }

  TEST_CASE("the copy that comes after is told that the first one runs, and holds nothing");
  second = take_named(test_mutex, &err);
  TEST_CHECK(second.handle == NULL);
  TEST_CHECK(ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)));
  OV_ERROR_DESTROY(&err);

  TEST_CASE("the name is free again once both copies let it go");
  TEST_SUCCEEDED(instance_release(&second, &err), &err);
  TEST_SUCCEEDED(instance_release(&first, &err), &err);
  first = take_named(test_mutex, &err);
  if (!TEST_SUCCEEDED(first.handle != NULL, &err)) {
    goto cleanup;
  }

  TEST_CASE_(NULL);

cleanup:
  TEST_SUCCEEDED(instance_release(&second, &err), &err);
  TEST_SUCCEEDED(instance_release(&first, &err), &err);
}

/**
 * @brief A mutex without a name is not a mutex
 */
static void test_refused_arguments(void) {
  struct ov_error err = {0};
  struct instance taken = {0};

  TEST_CASE("a take without a name is refused");
  taken = take_named(NULL, &err);
  TEST_CHECK(taken.handle == NULL);
  TEST_CHECK(ov_error_is(&err, ov_error_type_generic, ov_error_generic_invalid_argument));
  OV_ERROR_DESTROY(&err);

  TEST_CASE_(NULL);
}

/**
 * @brief Handing back accepts an instance that is already gone
 */
static void test_release_accepts_nothing(void) {
  struct ov_error err = {0};

  TEST_SUCCEEDED(instance_release(NULL, &err), &err);
  {
    struct instance none = {0};

    TEST_SUCCEEDED(instance_release(&none, &err), &err);
    TEST_CHECK(none.handle == NULL);
  }
}

TEST_LIST = {
    {"take_marks_the_first_copy", test_take_marks_the_first_copy},
    {"refused_arguments", test_refused_arguments},
    {"release_accepts_nothing", test_release_accepts_nothing},
    {NULL, NULL},
};
