#include <ovtest.h>

#include "loggate.h"

/**
 * @brief An unchanged failure must reach the log only once
 *
 * The poll runs every few seconds.
 */
static void test_first_and_repeat(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(&gate, "device is busy"))) {
    goto cleanup;
  }
  TEST_CHECK(!loggate_should_emit(&gate, "device is busy"));
  TEST_CHECK(!loggate_should_emit(&gate, "device is busy"));

cleanup:
  loggate_release(&gate);
}

/**
 * @brief A different message must get through, including a change back to an earlier one
 */
static void test_change(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(&gate, "problem code 10"))) {
    goto cleanup;
  }
  TEST_CHECK(loggate_should_emit(&gate, "no interface registered"));
  TEST_CHECK(!loggate_should_emit(&gate, "no interface registered"));
  TEST_CHECK(loggate_should_emit(&gate, "problem code 10"));

cleanup:
  loggate_release(&gate);
}

/**
 * @brief Only the tail differing still counts as a change, the messages must not be merged
 */
static void test_similarity(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(&gate, "service audientusbaudioks is absent"))) {
    goto cleanup;
  }
  TEST_CHECK(loggate_should_emit(&gate, "service audientusbaudioks is present"));
  TEST_CHECK(!loggate_should_emit(&gate, "service audientusbaudioks is present"));

cleanup:
  loggate_release(&gate);
}

/**
 * @brief An empty message is a message
 */
static void test_empty(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(&gate, ""))) {
    goto cleanup;
  }
  TEST_CHECK(!loggate_should_emit(&gate, ""));
  TEST_CHECK(loggate_should_emit(&gate, "something"));

cleanup:
  loggate_release(&gate);
}

/**
 * @brief Releasing forgets, so the next occurrence is reported again
 */
static void test_release(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(&gate, "same"))) {
    goto cleanup;
  }
  TEST_CHECK(!loggate_should_emit(&gate, "same"));
  loggate_release(&gate); // releasing forgets, so the next occurrence is reported again
  TEST_CHECK(gate.last == NULL);
  TEST_CHECK(loggate_should_emit(&gate, "same"));

cleanup:
  loggate_release(&gate);
}

/**
 * @brief Null arguments must not crash, they report
 */
static void test_null(void) {
  struct loggate gate;
  memset(&gate, 0, sizeof(gate));
  if (!TEST_CHECK(loggate_should_emit(NULL, "x"))) {
    goto cleanup;
  }
  TEST_CHECK(loggate_should_emit(&gate, NULL));
  loggate_release(NULL);

cleanup:
  loggate_release(&gate);
}

TEST_LIST = {
    {"first_and_repeat", test_first_and_repeat},
    {"change", test_change},
    {"similarity", test_similarity},
    {"empty", test_empty},
    {"release", test_release},
    {"null", test_null},
    {NULL, NULL},
};
