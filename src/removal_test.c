#include <windows.h>

#include <cfgmgr32.h>
#include <ovtest.h>

#include <string.h>

#include <ovarray.h>

#include "removal.h"

/**
 * @brief The CONFIGRET of the removal call maps to the result the caller acts on
 */
static void test_result_from_configret(void) {
  TEST_CHECK(removal_result_from_configret(CR_SUCCESS) == REMOVAL_RESULT_OK);
  TEST_CHECK(removal_result_from_configret(CR_REMOVE_VETOED) == REMOVAL_RESULT_VETO);
  TEST_CHECK(removal_result_from_configret(CR_ACCESS_DENIED) == REMOVAL_RESULT_FAILED);
  TEST_CHECK(removal_result_from_configret(CR_INVALID_DEVNODE) == REMOVAL_RESULT_FAILED);
  TEST_CHECK(removal_result_from_configret(0xFFFFFFFF) == REMOVAL_RESULT_FAILED);
}

/**
 * @brief The reason is what the dialog offers the user when the system refused
 *
 * The codes are the ones CM_Query_And_Remove_SubTreeW really answers with.
 */
static void test_reason_from_configret(void) {
  TEST_CHECK(removal_reason_from_configret(CR_SUCCESS) == REMOVAL_REASON_NONE);
  TEST_CHECK(removal_reason_from_configret(CR_ACCESS_DENIED) == REMOVAL_REASON_ACCESS_DENIED);
  TEST_CHECK(removal_reason_from_configret(CR_NO_SUCH_DEVNODE) == REMOVAL_REASON_DEVICE_GONE);
  TEST_CHECK(removal_reason_from_configret(CR_INVALID_DEVNODE) == REMOVAL_REASON_DEVICE_GONE);
  TEST_CHECK(removal_reason_from_configret(CR_NO_SUCH_VALUE) == REMOVAL_REASON_DEVICE_GONE);
  TEST_CHECK(removal_reason_from_configret(CR_REMOVE_VETOED) == REMOVAL_REASON_FAILED);
  TEST_CHECK(removal_reason_from_configret(0xFFFFFFFF) == REMOVAL_REASON_FAILED);
  TEST_CHECK(removal_reason_from_configret(CR_OUT_OF_MEMORY) == REMOVAL_REASON_FAILED);
}

/**
 * @brief The reason names are the words the dialog shows
 */
static void test_reason_name(void) {
  TEST_CHECK(strcmp(removal_reason_name(REMOVAL_REASON_NONE), "none") == 0);
  TEST_CHECK(strcmp(removal_reason_name(REMOVAL_REASON_ACCESS_DENIED), "access denied") == 0);
  TEST_CHECK(strcmp(removal_reason_name(REMOVAL_REASON_CALL_FAILED), "the removal call failed") == 0);
  TEST_CHECK(strcmp(removal_reason_name(REMOVAL_REASON_DEVICE_GONE), "the device is gone") == 0);
  TEST_CHECK(strcmp(removal_reason_name(REMOVAL_REASON_FAILED), "the system refused") == 0);
}

/**
 * @brief The result names are the words the log records
 */
static void test_result_name(void) {
  TEST_CHECK(strcmp(removal_result_name(REMOVAL_RESULT_OK), "ok") == 0);
  TEST_CHECK(strcmp(removal_result_name(REMOVAL_RESULT_VETO), "veto") == 0);
  TEST_CHECK(strcmp(removal_result_name(REMOVAL_RESULT_FAILED), "failed") == 0);
}

/**
 * @brief A missing devnode is a failure, and it carries the reason the dialog can explain
 */
static void test_close_null(void) {
  struct ov_error err = {0};
  enum removal_reason reason = REMOVAL_REASON_NONE;
  int code = 0;

  if (!TEST_CHECK(removal_close(NULL, &reason, &err) == REMOVAL_RESULT_FAILED)) {
    goto cleanup;
  }
  TEST_CHECK(ov_error_get_code(&err, ov_error_type_generic, &code));
  TEST_CHECK(reason == REMOVAL_REASON_FAILED);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief The reason is optional, a caller that only wants the result must not crash
 */
static void test_close_null_reason(void) {
  struct ov_error err = {0};

  TEST_CHECK(removal_close(NULL, NULL, &err) == REMOVAL_RESULT_FAILED);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief The error has to name the call that failed
 *
 * One helper that raises every CONFIGRET error records its own line instead, and then the
 * log points at the one place all of them share instead of the call that failed.
 */
static void test_close_error_names_the_call_site(void) {
  struct ov_error err = {0};
  char *msg = NULL;
  enum removal_reason reason = REMOVAL_REASON_NONE;

  if (!TEST_CHECK(removal_close("adc-no-such-device", &reason, &err) == REMOVAL_RESULT_FAILED)) {
    goto cleanup;
  }
  if (!TEST_CHECK(ov_error_to_string(&err, &msg, true, NULL) && (msg != NULL))) {
    goto cleanup;
  }
  if (!TEST_CHECK(strstr(msg, "removal_close()") != NULL)) {
    TEST_MSG("want the call site, got [%hs]", msg);
  }
  if (!TEST_CHECK(strstr(msg, "set_configret_error") == NULL)) {
    TEST_MSG("want no helper in the position, got [%hs]", msg);
  }

cleanup:
  if (msg != NULL) {
    OV_ARRAY_DESTROY(&msg);
  }
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief A devnode that cannot be one the system made is refused before any call
 *
 * The copy of it into the buffer the CM calls take would cut it into another one.
 */
static void test_close_refuses_a_long_devnode(void) {
  struct ov_error err = {0};
  enum removal_reason reason = REMOVAL_REASON_NONE;
  char devnode[MAX_DEVICE_ID_LEN + 8];

  memset(devnode, 'x', sizeof(devnode) - 1);
  devnode[sizeof(devnode) - 1] = '\0';
  TEST_CHECK(removal_close(devnode, &reason, &err) == REMOVAL_RESULT_FAILED);
  TEST_CHECK(reason == REMOVAL_REASON_CALL_FAILED);
  TEST_CHECK(ov_error_is(&err, ov_error_type_generic, ov_error_generic_invalid_argument));
  OV_ERROR_REPORT(&err, NULL);
}

TEST_LIST = {
    {"result_from_configret", test_result_from_configret},
    {"reason_from_configret", test_reason_from_configret},
    {"reason_name", test_reason_name},
    {"result_name", test_result_name},
    {"close_null", test_close_null},
    {"close_null_reason", test_close_null_reason},
    {"close_error_names_the_call_site", test_close_error_names_the_call_site},
    {"close_refuses_a_long_devnode", test_close_refuses_a_long_devnode},
    {NULL, NULL},
};
