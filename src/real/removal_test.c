/**
 * Tests for the machine that releases a device through the configuration manager.
 *
 * What can be checked without an elevation is the part that runs before the removal call
 * itself: the table that turns a CONFIGRET into the code the dialog explains, and the
 * refusals of a devnode that cannot be one the system made.  The removal call needs the
 * administrator, so its answers -- the success, the veto and the denial -- are left to a
 * person who runs the program by hand.
 */
#include <windows.h>

#include <cfgmgr32.h>

#include <ovtest.h>

#include "removal_manager.h"

#include "real/removal.c"

/**
 * @brief The CONFIGRET of the removal call maps to the code the dialog explains
 *
 * The codes are the ones CM_Query_And_Remove_SubTreeW really answers with.
 */
static void test_error_from_configret(void) {
  TEST_CHECK(removal_error_from_configret(CR_ACCESS_DENIED) == app_error_removal_access_denied);
  TEST_CHECK(removal_error_from_configret(CR_NO_SUCH_DEVNODE) == app_error_removal_device_gone);
  TEST_CHECK(removal_error_from_configret(CR_INVALID_DEVNODE) == app_error_removal_device_gone);
  TEST_CHECK(removal_error_from_configret(CR_NO_SUCH_VALUE) == app_error_removal_device_gone);
  TEST_CHECK(removal_error_from_configret(0xFFFFFFFF) == app_error_removal_refused);
  TEST_CHECK(removal_error_from_configret(CR_OUT_OF_MEMORY) == app_error_removal_refused);
}

/**
 * @brief Ask the machine of the system to release a device and take its answer
 *
 * @param machine the machine to ask
 * @param devnode the device to release
 * @param err receives the refusal
 * @return true when the device was released
 */
static bool remove_and_wait(struct removal_manager const *const machine, wchar_t const *const devnode, struct ov_error *const err) {
  struct removal_op *op = machine->remove_start(devnode, NULL, NULL, err);

  if (op == NULL) {
    return false;
  }
  return machine->op_release(&op);
}

/**
 * @brief A devnode the system does not know is a failure of the call that looked for it
 */
static void test_a_missing_devnode_is_a_failed_call(void) {
  struct ov_error err = {0};
  struct removal_manager const *const machine = removal_manager_system();

  TEST_FAILED_WITH(remove_and_wait(machine, L"adc-no-such-device", &err), &err, ov_error_type_app, app_error_removal_call_failed);
}

/**
 * @brief A devnode too long for the system call is refused before any call
 *
 * The copy of it into the buffer the CM calls take would cut it into another devnode.
 */
static void test_a_long_devnode_is_refused(void) {
  struct ov_error err = {0};
  struct removal_manager const *const machine = removal_manager_system();
  wchar_t devnode[MAX_DEVICE_ID_LEN + 8];

  for (size_t i = 0; i < (sizeof(devnode) / sizeof(devnode[0])) - 1; i++) {
    devnode[i] = L'x';
  }
  devnode[(sizeof(devnode) / sizeof(devnode[0])) - 1] = L'\0';
  TEST_FAILED_WITH(remove_and_wait(machine, devnode, &err), &err, ov_error_type_app, app_error_removal_call_failed);
}

/**
 * @brief The removal without a devnode is refused by the machine as well
 */
static void test_no_devnode_is_refused(void) {
  struct ov_error err = {0};
  struct removal_manager const *const machine = removal_manager_system();

  TEST_FAILED_WITH(remove_and_wait(machine, NULL, &err), &err, ov_error_type_app, app_error_removal_refused);
}

TEST_LIST = {
    {"error_from_configret", test_error_from_configret},
    {"a_missing_devnode_is_a_failed_call", test_a_missing_devnode_is_a_failed_call},
    {"a_long_devnode_is_refused", test_a_long_devnode_is_refused},
    {"no_devnode_is_refused", test_no_devnode_is_refused},
    {NULL, NULL},
};
