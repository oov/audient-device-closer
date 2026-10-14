#include "device.c"

#include <ovtest.h>

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
 * @brief No endpoint registered at all is not the sleep-resume failure either
 */
static void test_evaluate_no_interface(void) { TEST_CHECK(device_evaluate(0, 0, 0, 0, 0) == DEVICE_STATE_NO_INTERFACE); }

/**
 * @brief Freeing a string that is not there must not crash
 */
static void test_string_free_null(void) {
  char *p = NULL;
  device_string_free(&p);
  TEST_CHECK(p == NULL);
  device_string_free(NULL);
}

/**
 * @brief A device list that holds the two devices the settings file may name
 */
static struct device_entry const *make_devices(void) {
  static struct device_entry const devices[] = {{.id = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS", .name = "iD14mkII"},
                                                {.id = "TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS", .name = "iD44"}};
  return devices;
}

/**
 * @brief A devnode under the instance identifier of one of the devices belongs to it
 */
static void test_matches_devices(void) {
  bool matched = false;
  struct ov_error err = {0};

  struct device_entry const *const devices = make_devices();
  TEST_CHECK(device_matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", devices, 2, &matched, &err) == true);
  TEST_CHECK(matched == true);
  TEST_CHECK(device_matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS\\9&12ab34cd&0&1", devices, 2, &matched, &err) == true);
  TEST_CHECK(matched == true);
}

/**
 * @brief A devnode of another device, or of the wrong family, is not one of the devices
 */
static void test_matches_refuses_other_devices(void) {
  bool matched = true;
  struct ov_error err = {0};

  struct device_entry const *const devices = make_devices();
  TEST_CHECK(device_matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", devices, 2, &matched, &err) == true);
  TEST_CHECK(matched == false);
  TEST_CHECK(device_matches(L"USB\\VID_2708&PID_0008\\6&deadbee&0&2", devices, 2, &matched, &err) == true);
  TEST_CHECK(matched == false);
}

/**
 * @brief The match is not case sensitive: Windows spells the devnode its own way
 */
static void test_matches_is_case_insensitive(void) {
  bool matched = false;
  struct ov_error err = {0};

  struct device_entry const *const devices = make_devices();
  TEST_CHECK(device_matches(L"tusbaudio_enum\\vid_2708&pid_0008&ks\\6&deadbee&0&2", devices, 2, &matched, &err) == true);
  TEST_CHECK(matched == true);
}

/**
 * @brief A device list with no entries keeps every devnode of the service in play
 */
static void test_matches_no_devices_is_no_filter(void) {
  bool matched = false;
  struct ov_error err = {0};

  TEST_CHECK(device_matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", NULL, 0, &matched, &err) == true);
  TEST_CHECK(matched == true);

  TEST_CHECK(device_matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", make_devices(), 0, &matched, &err) == true);
  TEST_CHECK(matched == true);
}

/**
 * @brief The queries refuse arguments that name no device
 */
static void test_devnode_guards(void) {
  char const *const ks_devnode = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  char const *const usb_devnode = "USB\\VID_2708&PID_0008\\6&deadbee&0&2";
  struct device_entry const *const devices = make_devices();
  struct ov_error err = {0};

  TEST_FAILED_WITH(
      device_probe_devnode(NULL, &err) != DEVICE_STATE_QUERY_FAILED, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(
      device_probe_devnode("", &err) != DEVICE_STATE_QUERY_FAILED, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  TEST_FAILED_WITH(device_culprit_names_ancestor(NULL, ks_devnode, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_culprit_names_ancestor("", ks_devnode, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_culprit_names_ancestor(usb_devnode, NULL, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_culprit_names_ancestor(usb_devnode, "", &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  TEST_FAILED_WITH(device_probe_all(NULL, devices, 2, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief An instance Windows does not report anything for answers no
 */
static void test_culprit_ancestor_refuses_unknown_devnode(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(device_culprit_names_ancestor("USB\\VID_2708&PID_0008\\6&deadbee&0&2", "NOTAREALBUS_NOTHING", &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_fail);
}

/**
 * @brief The entry of the list a devnode belongs to
 *
 * The picker of a device names the device the reader picked, so a
 * devnode has to say which entry of the list it is.  A devnode no
 * entry names is one the list does not cover.
 */
static void test_find_entry(void) {
  char const *const devnode_0008 = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  char const *const devnode_000b = "TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS\\9&12ab34cd&0&1";
  char const *const devnode_0009 = "TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2";
  struct ov_error err = {0};
  struct device_entry const *const devices = make_devices();

  TEST_FAILED_WITH(device_find_entry(NULL, devices, 2, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_find_entry("", devices, 2, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  TEST_CHECK_(device_find_entry(devnode_000b, devices, 2, &err) == 1, "want the second entry");
  TEST_CHECK_(device_find_entry("tusbaudio_enum\\vid_2708&pid_0008&ks\\6&deadbee&0&2", devices, 2, &err) == 0,
              "want the first entry, the case of the devnode does not matter");
  TEST_FAILED_WITH(device_find_entry(devnode_0009, devices, 2, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_not_found);
  TEST_FAILED_WITH(device_find_entry(devnode_0008, NULL, 0, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_not_found);
}

/**
 * @brief Releasing what no probe handed out must not crash
 */
static void test_targets_release_null(void) {
  struct device_target *targets = NULL;

  device_targets_release(&targets);
  TEST_CHECK(targets == NULL);
  device_targets_release(NULL);
}

/**
 * @brief An identifier of a device names every unit of it, an
 * identifier of a unit names that unit alone
 */
static void test_is_same(void) {
  char const *const unit = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2";

  TEST_CHECK(device_is_same(unit, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == true);
  TEST_CHECK(device_is_same(unit, "tusbaudio_enum\\vid_2708&pid_0008&ks") == true);
  TEST_CHECK(device_is_same(unit, unit) == true);
  TEST_CHECK(device_is_same(unit, "tusbaudio_enum\\vid_2708&pid_0008&ks\\6&d6cb2a&0&2") == true);
  TEST_CHECK(device_is_same(unit, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2B&0&2") == false);
  TEST_CHECK(device_is_same(unit, "TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS") == false);
  TEST_CHECK(device_is_same(unit, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2&3") == false);
  TEST_CHECK(device_is_same(NULL, unit) == false);
  TEST_CHECK(device_is_same(unit, NULL) == false);
  TEST_CHECK(device_is_same("", unit) == false);
  TEST_CHECK(device_is_same(unit, "") == false);
}

/**
 * @brief A list of targets with the states named, the way probe_all hands them out
 *
 * @param states the state of each entry
 * @param count how many states the list holds
 * @return the list, release it with device_targets_release
 */
static struct device_target *make_target_states(enum device_state const *const states, size_t const count) {
  char const *const devnodes[] = {
      "TUSBAUDIO_ENUM\\VID_2708&PID_0001&KS\\6&deadbee&0&2",
      "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2",
      "TUSBAUDIO_ENUM\\VID_2708&PID_0003&KS\\6&deadbee&0&2",
      "TUSBAUDIO_ENUM\\VID_2708&PID_0004&KS\\6&deadbee&0&2",
  };
  struct device_target *targets = NULL;

  for (size_t i = 0; i < count; i++) {
    struct device_target t = {0};
    if (!ov_sprintf_char(&t.devnode, NULL, NULL, "%s", devnodes[i])) {
      return NULL;
    }
    t.state = states[i];
    if (!OV_ARRAY_PUSH(&targets, t)) {
      OV_ARRAY_DESTROY(&t.devnode);
      return NULL;
    }
  }
  return targets;
}

/**
 * @brief Only the faulted devices are the ones the repair is for
 */
static void test_keep_faulted_keeps_the_units_the_sleep_took(void) {
  enum device_state const states[] = {DEVICE_STATE_HEALTHY, DEVICE_STATE_FAULTED, DEVICE_STATE_ABSENT};
  struct device_target *targets = make_target_states(states, 3);

  if (TEST_CHECK(targets != NULL)) {
    device_targets_keep_faulted(&targets);
    TEST_CHECK_(OV_ARRAY_LENGTH(targets) == 1, "want only the faulted unit, got %zu", OV_ARRAY_LENGTH(targets));
    TEST_CHECK(targets[0].state == DEVICE_STATE_FAULTED);
    device_targets_release(&targets);
  }
}

/**
 * @brief A list of working devices keeps nothing to repair
 */
static void test_keep_faulted_leaves_nothing_when_every_unit_works(void) {
  enum device_state const states[] = {DEVICE_STATE_HEALTHY, DEVICE_STATE_ABSENT};
  struct device_target *targets = make_target_states(states, 2);

  if (TEST_CHECK(targets != NULL)) {
    device_targets_keep_faulted(&targets);
    TEST_CHECK_(OV_ARRAY_LENGTH(targets) == 0, "want no unit, got %zu", OV_ARRAY_LENGTH(targets));
    device_targets_release(&targets);
  }
}

/**
 * @brief Releasing what no probe handed out must not crash
 */
static void test_keep_faulted_accepts_null(void) {
  device_targets_keep_faulted(NULL);
  TEST_CHECK(true);
}

TEST_LIST = {
    {"evaluate_healthy", test_evaluate_healthy},
    {"evaluate_faulted", test_evaluate_faulted},
    {"evaluate_problem", test_evaluate_problem},
    {"evaluate_no_interface", test_evaluate_no_interface},
    {"matches_devices", test_matches_devices},
    {"matches_refuses_other_devices", test_matches_refuses_other_devices},
    {"matches_is_case_insensitive", test_matches_is_case_insensitive},
    {"matches_no_devices_is_no_filter", test_matches_no_devices_is_no_filter},
    {"find_entry", test_find_entry},
    {"is_same", test_is_same},
    {"keep_faulted_keeps_the_units_the_sleep_took", test_keep_faulted_keeps_the_units_the_sleep_took},
    {"keep_faulted_leaves_nothing_when_every_unit_works", test_keep_faulted_leaves_nothing_when_every_unit_works},
    {"keep_faulted_accepts_null", test_keep_faulted_accepts_null},
    {"string_free_null", test_string_free_null},
    {"devnode_guards", test_devnode_guards},
    {"culprit_ancestor_unknown_devnode", test_culprit_ancestor_refuses_unknown_devnode},
    {"targets_release_null", test_targets_release_null},
    {NULL, NULL},
};
