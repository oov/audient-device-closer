#include "device_probe.c"

#include "mock/device.h"

#include <ovtest.h>

/**
 * @brief A healthy device: no problem code and an endpoint is present
 */
static void test_evaluate_healthy(void) {
  TEST_CHECK(device_facts_state(&(struct device_facts){0, true, true}) == DEVICE_STATE_HEALTHY);
  TEST_CHECK(device_facts_state(&(struct device_facts){0, true, false}) == DEVICE_STATE_HEALTHY);
}

/**
 * @brief The failure this tool cures: no problem code, endpoints registered, none present
 */
static void test_evaluate_faulted(void) { TEST_CHECK(device_facts_state(&(struct device_facts){0, false, true}) == DEVICE_STATE_FAULTED); }

/**
 * @brief A problem code is another kind of failure, the device must NOT be removed for it
 */
static void test_evaluate_problem(void) {
  TEST_CHECK(device_facts_state(&(struct device_facts){10, false, true}) == DEVICE_STATE_PROBLEM); // CM_PROB_FAILED_START
  TEST_CHECK(device_facts_state(&(struct device_facts){22, false, true}) == DEVICE_STATE_PROBLEM); // CM_PROB_DISABLED
  TEST_CHECK(device_facts_state(&(struct device_facts){10, true, true}) == DEVICE_STATE_PROBLEM);  // present but disabled etc.
}

/**
 * @brief No endpoint registered at all is not the sleep-resume failure either
 */
static void test_evaluate_no_interface(void) {
  TEST_CHECK(device_facts_state(&(struct device_facts){0, false, false}) == DEVICE_STATE_NO_INTERFACE);
}

/**
 * @brief A devnode that could not be asked is not a problem of the device
 */
static void test_evaluate_no_report(void) {
  TEST_CHECK(device_facts_state(&(struct device_facts){.problem_code = DEVICE_PROBLEM_NO_REPORT}) == DEVICE_STATE_QUERY_FAILED);
  TEST_CHECK(device_facts_state(&(struct device_facts){.problem_code = DEVICE_PROBLEM_NO_REPORT, .endpoint_present = true}) ==
             DEVICE_STATE_QUERY_FAILED);
}

/**
 * @brief The answer the watch stops on is the healthy state and nothing else
 *
 * The watch of the machine stops when this answers true, so it has to agree with evaluate
 * about what healthy is: no problem code, and an endpoint of the service present.
 */
static void test_facts_is_healthy(void) {
  TEST_CHECK(device_facts_is_healthy(&(struct device_facts){0, true, true}) == true);
  TEST_CHECK(device_facts_is_healthy(&(struct device_facts){0, true, false}) == true);
  TEST_CHECK(device_facts_is_healthy(&(struct device_facts){0, false, true}) == false);
  TEST_CHECK(device_facts_is_healthy(&(struct device_facts){0, false, false}) == false);
  TEST_CHECK(device_facts_is_healthy(&(struct device_facts){10, true, true}) == false);
}

/**
 * @brief A device list that holds the two devices the settings file may name
 */
static struct device_entry const *make_devices(void) {
  static struct device_entry const devices[] = {{.id = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS", .name = L"iD14mkII"},
                                                {.id = L"TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS", .name = L"iD44"}};
  return devices;
}

/**
 * @brief A devnode under the instance identifier of one of the devices belongs to it
 */
static void test_matches_devices(void) {
  struct device_entry const *const devices = make_devices();
  TEST_CHECK(matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", devices, 2) == true);
  TEST_CHECK(matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS\\9&12ab34cd&0&1", devices, 2) == true);
}

/**
 * @brief A devnode of another device, or of the wrong family, is not one of the devices
 */
static void test_matches_refuses_other_devices(void) {
  struct device_entry const *const devices = make_devices();
  TEST_CHECK(matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", devices, 2) == false);
  TEST_CHECK(matches(L"USB\\VID_2708&PID_0008\\6&deadbee&0&2", devices, 2) == false);
}

/**
 * @brief The match is not case sensitive: Windows spells the devnode its own way
 */
static void test_matches_is_case_insensitive(void) {
  struct device_entry const *const devices = make_devices();
  TEST_CHECK(matches(L"tusbaudio_enum\\vid_2708&pid_0008&ks\\6&deadbee&0&2", devices, 2) == true);
}

/**
 * @brief A device list with no entries matches nothing
 */
static void test_matches_no_devices_matches_nothing(void) {
  TEST_CHECK(matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", NULL, 0) == false);
  TEST_CHECK(matches(L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2", make_devices(), 0) == false);
}

/**
 * @brief The queries refuse arguments that name no device
 */
static void test_devnode_guards(void) {
  wchar_t const *const ks_devnode = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  wchar_t const *const usb_instance = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2";
  struct device_entry const *const devices = make_devices();
  struct ov_error err = {0};

  TEST_FAILED_WITH(
      device_probe_devnode(NULL, &err) != DEVICE_STATE_QUERY_FAILED, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(
      device_probe_devnode(L"", &err) != DEVICE_STATE_QUERY_FAILED, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  TEST_FAILED_WITH(device_veto_event_names_ancestor(NULL, ks_devnode, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_veto_event_names_ancestor(L"", ks_devnode, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_veto_event_names_ancestor(usb_instance, NULL, &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_veto_event_names_ancestor(usb_instance, L"", &err) != ov_indeterminate,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  TEST_FAILED_WITH(device_probe_all(NULL, devices, 2, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief An instance Windows does not report anything for answers no
 */
static void test_veto_event_ancestor_refuses_unknown_devnode(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(device_veto_event_names_ancestor(L"USB\\VID_2708&PID_0008\\6&deadbee&0&2", L"NOTAREALBUS_NOTHING", &err) !=
                       ov_indeterminate,
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
  wchar_t const *const devnode_0008 = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  wchar_t const *const devnode_000b = L"TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS\\9&12ab34cd&0&1";
  wchar_t const *const devnode_0009 = L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS\\6&deadbee&0&2";
  struct ov_error err = {0};
  struct device_entry const *const devices = make_devices();
  size_t index = 0;

  TEST_FAILED_WITH(device_find_entry(NULL, devices, 2, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(device_find_entry(L"", devices, 2, &err) != SIZE_MAX, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

  index = device_find_entry(devnode_000b, devices, 2, &err);
  if (TEST_SUCCEEDED(index != SIZE_MAX, &err)) {
    TEST_CHECK_(index == 1, "want the second entry, got %zu", index);
  }
  index = device_find_entry(L"tusbaudio_enum\\vid_2708&pid_0008&ks\\6&deadbee&0&2", devices, 2, &err);
  if (TEST_SUCCEEDED(index != SIZE_MAX, &err)) {
    TEST_CHECK_(index == 0, "want the first entry, the case of the devnode does not matter, got %zu", index);
  }
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
  wchar_t const *const unit = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2";

  TEST_CHECK(device_is_same(unit, L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == true);
  TEST_CHECK(device_is_same(unit, L"tusbaudio_enum\\vid_2708&pid_0008&ks") == true);
  TEST_CHECK(device_is_same(unit, L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2") == true);
  TEST_CHECK(device_is_same(unit, L"tusbaudio_enum\\vid_2708&pid_0008&ks\\6&d6cb2a&0&2") == true);
  TEST_CHECK(device_is_same(unit, L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2B&0&2") == false);
  TEST_CHECK(device_is_same(unit, L"TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS") == false);
  TEST_CHECK(device_is_same(unit, L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2&3") == false);
  TEST_CHECK(device_is_same(NULL, L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == false);
  TEST_CHECK(device_is_same(unit, NULL) == false);
  TEST_CHECK(device_is_same(L"", L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == false);
  TEST_CHECK(device_is_same(unit, L"") == false);
}

/**
 * @brief A list of targets with the states named, the way probe_all hands them out
 *
 * @param states the state of each entry
 * @param count how many states the list holds
 * @return the list, release it with device_targets_release
 */
static struct device_target *make_target_states(enum device_state const *const states, size_t const count) {
  wchar_t const *const devnodes[] = {
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0001&KS\\6&deadbee&0&2",
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2",
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0003&KS\\6&deadbee&0&2",
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0004&KS\\6&deadbee&0&2",
  };
  struct device_target *targets = NULL;

  for (size_t i = 0; i < count; i++) {
    struct device_target t = {0};
    size_t const chars = wcslen(devnodes[i]) + 1;

    if (!OV_ARRAY_GROW(&t.devnode, chars)) {
      return NULL;
    }
    OV_ARRAY_SET_LENGTH(t.devnode, chars - 1);
    wcscpy(t.devnode, devnodes[i]);
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

/**
 * @brief A stand-in machine that answers the fault this tool cures
 *
 * One unit that works and one that the sleep left registered but without an endpoint.  The
 * KS devnode sits under the USB instance, which is the ancestor a removal record names.
 *
 * @return the script, valid until the test replaces it
 */
static struct device_mock_script const *make_sleep_script(void) {
  static struct device_mock_node const nodes[] = {
      {.devnode = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2",
       .parent = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2",
       .facts = {.problem_code = 0, .endpoint_present = false, .endpoint_registered = true}},
      {.devnode = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2",
       .facts = {.problem_code = 0, .endpoint_present = true, .endpoint_registered = true}},
  };
  static struct device_mock_script const script = {
      .service = L"audientusbaudioks",
      .nodes = nodes,
      .node_count = sizeof(nodes) / sizeof(nodes[0]),
  };
  return &script;
}

/**
 * @brief The stand-in answers what its script says, and only for its own service
 */
static void test_mock_speaks_for_the_machine(void) {
  struct ov_error err = {0};
  struct device_facts facts;
  wchar_t *parent = NULL;

  TEST_ASSERT(device_mock_install(make_sleep_script()));

  TEST_CASE("the devnode the script describes");
  TEST_SUCCEEDED(device_manager()->facts_of(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &facts, &err), &err);
  TEST_CHECK(facts.endpoint_registered);
  TEST_CHECK(!facts.endpoint_present);
  TEST_CHECK(device_facts_state(&facts) == DEVICE_STATE_FAULTED);

  TEST_CASE("a devnode the script does not name");
  TEST_FAILED_WITH(
      device_manager()->facts_of(L"NOT\\IN\\THE\\SCRIPT", &facts, &err), &err, ov_error_type_generic, ov_error_generic_not_found);

  TEST_CASE("the device above the KS devnode");
  TEST_SUCCEEDED(device_manager()->parent_of(L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &parent, &err) == ov_true, &err);
  if (TEST_CHECK(parent != NULL)) {
    TEST_CHECK(wcscmp(parent, L"USB\\VID_2708&PID_0008\\6&deadbee&0&2") == 0);
    TEST_MSG("want the parent of the script, got [%ls]", parent);
    OV_ARRAY_DESTROY(&parent);
  }

  TEST_CASE("a node at the root has no device above it");
  TEST_SUCCEEDED(device_manager()->parent_of(L"USB\\VID_2708&PID_0008\\6&deadbee&0&2", &parent, &err) == ov_false, &err);
  TEST_CHECK(parent == NULL);

  device_mock_reset();
}

/**
 * @brief The stand-in reaches the whole way a run reads the devices
 *
 * device_probe_all is what a run and the window call, so the stand-in has to answer through
 * it and not only through the question it is asked directly.
 */
static void test_mock_reaches_probe_all(void) {
  struct device_entry const *const devices = make_devices();
  struct device_target *targets = NULL;
  struct ov_error err = {0};

  TEST_ASSERT(device_mock_install(make_sleep_script()));
  targets = device_probe_all(L"audientusbaudioks", devices, 2, &err);
  if (TEST_CHECK(targets != NULL)) {
    TEST_CHECK_(OV_ARRAY_LENGTH(targets) == 1, "want the one unit the script names, got %zu", OV_ARRAY_LENGTH(targets));
    TEST_CHECK(targets[0].state == DEVICE_STATE_FAULTED);
    device_targets_keep_faulted(&targets);
    TEST_CHECK_(OV_ARRAY_LENGTH(targets) == 1, "want the faulted unit to be the one to repair, got %zu", OV_ARRAY_LENGTH(targets));
    device_targets_release(&targets);
  }

  TEST_CASE("the other service has no devnode in the script");
  TEST_FAILED_WITH(
      device_probe_all(L"some-other-service", devices, 2, &err) != NULL, &err, ov_error_type_generic, ov_error_generic_not_found);

  device_mock_reset();
}

/**
 * @brief The sentence of a problem is the stand-in's, and it tells the codes apart
 *
 * The stand-in has two sentences: none for a devnode the machine named no problem of, and
 * one for every other code.  A caller turns the answer into the sentence of the reader's
 * language, which is why the answer is the identifier and not the translation.
 */
static void test_mock_problem_id(void) {
  TEST_ASSERT(device_mock_install(make_sleep_script()));

  TEST_CASE("a devnode with no problem has no sentence");
  TEST_CHECK(device_manager()->problem_id(0) == NULL);

  TEST_CASE("a problem the machine named has a sentence");
  TEST_CHECK(device_manager()->problem_id(10) != NULL);

  device_mock_reset();
}
static void test_mock_ancestor_walk(void) {
  struct ov_error err = {0};

  TEST_ASSERT(device_mock_install(make_sleep_script()));
  TEST_CHECK(device_veto_event_names_ancestor(
                 L"USB\\VID_2708&PID_0008\\6&deadbee&0&2", L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &err) == ov_true);
  TEST_CHECK(device_veto_event_names_ancestor(
                 L"USB\\VID_2708&PID_9999\\6&nope&0&1", L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2", &err) == ov_false);
  device_mock_reset();
}

TEST_LIST = {
    {"evaluate_healthy", test_evaluate_healthy},
    {"evaluate_faulted", test_evaluate_faulted},
    {"evaluate_problem", test_evaluate_problem},
    {"evaluate_no_interface", test_evaluate_no_interface},
    {"evaluate_no_report", test_evaluate_no_report},
    {"facts_is_healthy", test_facts_is_healthy},
    {"matches_devices", test_matches_devices},
    {"matches_refuses_other_devices", test_matches_refuses_other_devices},
    {"matches_is_case_insensitive", test_matches_is_case_insensitive},
    {"matches_no_devices_matches_nothing", test_matches_no_devices_matches_nothing},
    {"find_entry", test_find_entry},
    {"is_same", test_is_same},
    {"keep_faulted_keeps_the_units_the_sleep_took", test_keep_faulted_keeps_the_units_the_sleep_took},
    {"keep_faulted_leaves_nothing_when_every_unit_works", test_keep_faulted_leaves_nothing_when_every_unit_works},
    {"keep_faulted_accepts_null", test_keep_faulted_accepts_null},
    {"devnode_guards", test_devnode_guards},
    {"veto_event_ancestor_unknown_devnode", test_veto_event_ancestor_refuses_unknown_devnode},
    {"targets_release_null", test_targets_release_null},
    {"mock_speaks_for_the_machine", test_mock_speaks_for_the_machine},
    {"mock_reaches_probe_all", test_mock_reaches_probe_all},
    {"mock_ancestor_walk", test_mock_ancestor_walk},
    {"mock_problem_id", test_mock_problem_id},
    {NULL, NULL},
};
