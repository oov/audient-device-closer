#include <windows.h>

#include <cfgmgr32.h>

#include "mock/removal.h"

#include "removal_manager.h"

#include <ovtest.h>

#include <ovarray.h>

/**
 * @brief Ask the machine to release a device and take its answer
 *
 * The removal runs on a thread of its own, so the test waits for the answer it asked for to
 * arrive before it reads it.
 *
 * @param devnode the device to release
 * @param err receives the refusal
 * @return true when the device was released
 */
static bool remove_and_wait(wchar_t const *const devnode, struct ov_error *const err) {
  struct removal_op *op = removal_manager()->remove_start(devnode, NULL, NULL, err);

  if (op == NULL) {
    return false;
  }
  return removal_manager()->op_release(&op);
}

/**
 * @brief A missing devnode is a failure the caller can explain, and it says why
 */
static void test_remove_null(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(remove_and_wait(NULL, &err), &err, ov_error_type_app, app_error_removal_refused);
}

/**
 * @brief A devnode the system does not know is a failure of the call that looked for it
 */
static void test_remove_missing_devnode(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(remove_and_wait(L"adc-no-such-device", &err), &err, ov_error_type_app, app_error_removal_call_failed);
}

/**
 * @brief A devnode that cannot be one the system made is refused before any call
 *
 * The copy of it into the buffer the CM calls take would cut it into another one.
 */
static void test_remove_refuses_a_long_devnode(void) {
  struct ov_error err = {0};
  wchar_t devnode[MAX_DEVICE_ID_LEN + 8];

  for (size_t i = 0; i < (sizeof(devnode) / sizeof(devnode[0])) - 1; i++) {
    devnode[i] = L'x';
  }
  devnode[(sizeof(devnode) / sizeof(devnode[0])) - 1] = L'\0';
  TEST_FAILED_WITH(remove_and_wait(devnode, &err), &err, ov_error_type_app, app_error_removal_call_failed);
}

/**
 * @brief The release goes through the machine that is installed
 *
 * The whole point of the stand-in is that the program reads the machine through one place,
 * so a script installed here has to be what the machine answers with.
 */
static void test_mock_answers_the_removal(void) {
  struct removal_mock_script const vetoed = {
      .refusals = 1,
      .code = app_error_removal_veto,
      .vetoed_device = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2",
  };
  struct ov_error err = {0};

  TEST_ASSERT(removal_mock_install(&vetoed));

  TEST_CASE("the first removal is refused as a veto");
  TEST_FAILED_WITH(
      remove_and_wait(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &err), &err, ov_error_type_app, app_error_removal_veto);
  TEST_CHECK_(removal_mock_remove_count() == 1, "want one removal asked for, got %zu", removal_mock_remove_count());

  TEST_CASE("the second one goes through");
  TEST_SUCCEEDED(remove_and_wait(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &err), &err);
  TEST_CHECK_(removal_mock_remove_count() == 2, "want two removals asked for, got %zu", removal_mock_remove_count());

  removal_mock_reset();
}

/**
 * @brief The stand-in can refuse for a reason other than a veto
 */
static void test_mock_refuses_for_another_reason(void) {
  struct removal_mock_script const denied = {
      .refusals = 1,
      .code = app_error_removal_access_denied,
  };
  struct ov_error err = {0};

  TEST_ASSERT(removal_mock_install(&denied));
  TEST_FAILED_WITH(remove_and_wait(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &err),
                   &err,
                   ov_error_type_app,
                   app_error_removal_access_denied);
  removal_mock_reset();
}

/**
 * @brief A veto is told apart from every other failure by one call
 *
 * The caller closes what holds the device and asks again for a veto, and gives up for
 * anything else, so the test of the code has to be the veto and nothing but the veto.
 */
static void test_error_is_veto(void) {
  struct ov_error veto = {0};
  struct ov_error denied = {0};
  struct ov_error generic = {0};

  OV_ERROR_SET(&veto, ov_error_type_app, app_error_removal_veto, "the removal was vetoed");
  TEST_CHECK(removal_error_is_veto(&veto) == true);

  OV_ERROR_SET(&denied, ov_error_type_app, app_error_removal_access_denied, "not allowed");
  TEST_CHECK(removal_error_is_veto(&denied) == false);

  OV_ERROR_SET_GENERIC(&generic, ov_error_generic_fail);
  TEST_CHECK(removal_error_is_veto(&generic) == false);

  OV_ERROR_DESTROY(&veto);
  OV_ERROR_DESTROY(&denied);
  OV_ERROR_DESTROY(&generic);
}

TEST_LIST = {
    {"remove_null", test_remove_null},
    {"remove_missing_devnode", test_remove_missing_devnode},
    {"remove_refuses_a_long_devnode", test_remove_refuses_a_long_devnode},
    {"mock_answers_the_removal", test_mock_answers_the_removal},
    {"mock_refuses_for_another_reason", test_mock_refuses_for_another_reason},
    {"error_is_veto", test_error_is_veto},
    {NULL, NULL},
};
