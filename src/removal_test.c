#include <windows.h>

#include <cfgmgr32.h>

#include "removal.c"

#include <ovtest.h>

#include <ovarray.h>

/**
 * @brief The CONFIGRET of the removal call maps to the code the dialog explains
 *
 * The codes are the ones CM_Query_And_Remove_SubTreeW really answers with.
 */
static void test_error_from_configret(void) {
  TEST_CHECK(removal_error_from_configret(CR_ACCESS_DENIED) == removal_error_access_denied);
  TEST_CHECK(removal_error_from_configret(CR_NO_SUCH_DEVNODE) == removal_error_device_gone);
  TEST_CHECK(removal_error_from_configret(CR_INVALID_DEVNODE) == removal_error_device_gone);
  TEST_CHECK(removal_error_from_configret(CR_NO_SUCH_VALUE) == removal_error_device_gone);
  TEST_CHECK(removal_error_from_configret(0xFFFFFFFF) == removal_error_refused);
  TEST_CHECK(removal_error_from_configret(CR_OUT_OF_MEMORY) == removal_error_refused);
}

/**
 * @brief A missing devnode is a failure the caller can explain, and it says why
 */
static void test_close_null(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(removal_close(NULL, &err), &err, ov_error_type_removal, removal_error_refused);
}

/**
 * @brief A devnode the system does not know is a failure of the call that looked for it
 */
static void test_close_missing_devnode(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(removal_close("adc-no-such-device", &err), &err, ov_error_type_removal, removal_error_call_failed);
}

/**
 * @brief A devnode that cannot be one the system made is refused before any call
 *
 * The copy of it into the buffer the CM calls take would cut it into another one.
 */
static void test_close_refuses_a_long_devnode(void) {
  struct ov_error err = {0};
  char devnode[MAX_DEVICE_ID_LEN + 8];

  memset(devnode, 'x', sizeof(devnode) - 1);
  devnode[sizeof(devnode) - 1] = '\0';
  TEST_FAILED_WITH(removal_close(devnode, &err), &err, ov_error_type_removal, removal_error_call_failed);
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

  OV_ERROR_SET(&veto, ov_error_type_removal, removal_error_veto, "the removal was vetoed");
  TEST_CHECK(removal_error_is_veto(&veto) == true);

  OV_ERROR_SET(&denied, ov_error_type_removal, removal_error_access_denied, "not allowed");
  TEST_CHECK(removal_error_is_veto(&denied) == false);

  OV_ERROR_SET_GENERIC(&generic, ov_error_generic_fail);
  TEST_CHECK(removal_error_is_veto(&generic) == false);

  OV_ERROR_DESTROY(&veto);
  OV_ERROR_DESTROY(&denied);
  OV_ERROR_DESTROY(&generic);
}

TEST_LIST = {
    {"error_from_configret", test_error_from_configret},
    {"close_null", test_close_null},
    {"close_missing_devnode", test_close_missing_devnode},
    {"close_refuses_a_long_devnode", test_close_refuses_a_long_devnode},
    {"error_is_veto", test_error_is_veto},
    {NULL, NULL},
};
