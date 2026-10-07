#include <ovtest.h>

#include "device.h"

/**
 * @brief A healthy device: the problem code is CM_PROB_NONE and an endpoint is present
 *
 * CM_PROB_NONE is 0.  Everything else must not be treated as the sleep-resume failure.
 */
static void test_evaluate_healthy(void) {
  TEST_CHECK(device_evaluate(0, 1, 1, 1, 1) == DEVICE_STATE_HEALTHY);
  TEST_CHECK(device_evaluate(0, 1, 1, 0, 0) == DEVICE_STATE_HEALTHY);
  TEST_CHECK(device_evaluate(0, 0, 0, 1, 1) == DEVICE_STATE_HEALTHY);
}

/**
 * @brief The failure this tool cures: devnode fine, interfaces registered, none present
 */
static void test_evaluate_faulted(void) {
  TEST_CHECK(device_evaluate(0, 0, 1, 0, 1) == DEVICE_STATE_FAULTED);
  TEST_CHECK(device_evaluate(0, 0, 3, 0, 3) == DEVICE_STATE_FAULTED);
  TEST_CHECK(device_evaluate(0, 0, 1, 0, 0) == DEVICE_STATE_FAULTED);
  TEST_CHECK(device_evaluate(0, 0, 0, 0, 1) == DEVICE_STATE_FAULTED);
}

/**
 * @brief A problem code is another kind of failure, the device must NOT be removed for it
 */
static void test_evaluate_problem(void) {
  TEST_CHECK(device_evaluate(10, 0, 1, 0, 1) == DEVICE_STATE_PROBLEM); // CM_PROB_FAILED_START
  TEST_CHECK(device_evaluate(22, 0, 1, 0, 1) == DEVICE_STATE_PROBLEM); // CM_PROB_DISABLED
  TEST_CHECK(device_evaluate(10, 1, 1, 1, 1) == DEVICE_STATE_PROBLEM); // present but disabled etc.
}

/**
 * @brief Nothing registered at all is not the sleep-resume failure either
 */
static void test_evaluate_unknown(void) { TEST_CHECK(device_evaluate(0, 0, 0, 0, 0) == DEVICE_STATE_UNKNOWN); }

/**
 * @brief Freeing a string that is not there must not crash
 */
static void test_string_free_null(void) {
  char *p = NULL;
  device_string_free(&p);
  TEST_CHECK(p == NULL);
  device_string_free(NULL);
}

TEST_LIST = {
    {"evaluate_healthy", test_evaluate_healthy},
    {"evaluate_faulted", test_evaluate_faulted},
    {"evaluate_problem", test_evaluate_problem},
    {"evaluate_unknown", test_evaluate_unknown},
    {"string_free_null", test_string_free_null},
    {NULL, NULL},
};
