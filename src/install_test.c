/**
 * Tests for the record the installer leaves behind.
 *
 * The record lives on the machine, so the test speaks about the copy it runs from: the test
 * binary is not in the folder the installer uses, and the answer for it is the portable one.
 */
#include "install.h"

#include <ovtest.h>

/**
 * @brief A copy the installer did not put in its folder is not the installed copy
 *
 * The test binary runs from the build folder.  Whether the machine carries an install record
 * or not, this copy is not the one it names.
 */
static void test_a_copy_outside_the_recorded_folder_is_portable(void) {
  struct ov_error err = {0};

  if (!TEST_SUCCEEDED(install_init(&err), &err)) {
    return;
  }
  TEST_CHECK(!install_is_installed());
}

TEST_LIST = {
    {"a_copy_outside_the_recorded_folder_is_portable", test_a_copy_outside_the_recorded_folder_is_portable},
    {NULL, NULL},
};
