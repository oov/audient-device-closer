#include "settings.c"

#include <ovtest.h>

#include <string.h>

static char const kFull[] = "{"
                            "\"ks_service\": \"audientusbaudioks\","
                            "\"devices\": [{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}],"
                            "\"veto_log_name\": \"System\","
                            "\"veto_provider_name\": \"Microsoft-Windows-Kernel-PnP\","
                            "\"veto_event_id\": 225,"
                            "\"mixer_process_names\": [\"iD.exe\", \"EVO.exe\"],"
                            "\"task_name\": \"\\\\AudientDeviceCloser\""
                            "}";

static char const kDevices[] = // the table this file is mostly about
    "{"
    "\"ks_service\": \"audientusbaudioks\","
    "\"devices\": ["
    "{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"},"
    "{\"name\": \"iD44\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_000B&KS\"}"
    "],"
    "\"veto_log_name\": \"System\","
    "\"veto_provider_name\": \"Some-Provider\","
    "\"veto_event_id\": 1,"
    "\"mixer_process_names\": [\"foo.exe\"]"
    "}";

static char const kRequiredOnly[] = "{"
                                    "\"ks_service\": \"someks\","
                                    "\"devices\": [{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}],"
                                    "\"veto_log_name\": \"System\","
                                    "\"veto_provider_name\": \"Some-Provider\","
                                    "\"veto_event_id\": 1,"
                                    "\"mixer_process_names\": [\"foo.exe\"]"
                                    "}";

static char const kLegacyHealthyKey[] =
    "{\"ks_service\":\"someks\",\"veto_log_name\":\"System\",\"veto_provider_name\":\"Some-Provider\","
    "\"veto_event_id\":1,\"mixer_process_names\":[\"foo.exe\"],\"allow_healthy_close\":true,\"devices\":[{"
    "\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}";

static char const kLegacyRestartKeys[] = "{\"ks_service\":\"someks\",\"veto_log_name\":\"System\",\"veto_provider_name\":\"Some-Provider\","
                                         "\"veto_event_id\":1,\"mixer_process_names\":[\"foo.exe\"],"
                                         "\"mixer_fallback_path\":\"C:/Program Files/Audient/iD/iD.exe\","
                                         "\"launcher_process_name\":\"AudientAppLauncher.exe\",\"devices\":[{\"name\": \"iD14mkII\", "
                                         "\"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}";

static char const kThemeOff[] = "{"
                                "\"ks_service\": \"someks\","
                                "\"veto_log_name\": \"System\","
                                "\"veto_provider_name\": \"Some-Provider\","
                                "\"veto_event_id\": 1,"
                                "\"mixer_process_names\": [\"foo.exe\"],"
                                "\"use_theme\": false,"
                                "\"devices\": [{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]"
                                "}";

static char const kThemeOn[] = "{"
                               "\"ks_service\": \"someks\","
                               "\"veto_log_name\": \"System\","
                               "\"veto_provider_name\": \"Some-Provider\","
                               "\"veto_event_id\": 1,"
                               "\"mixer_process_names\": [\"foo.exe\"],"
                               "\"use_theme\": true,"
                               "\"devices\": [{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]"
                               "}";

static char const kConfirmWait[] = "{"
                                   "\"ks_service\": \"someks\","
                                   "\"veto_log_name\": \"System\","
                                   "\"veto_provider_name\": \"Some-Provider\","
                                   "\"veto_event_id\": 1,"
                                   "\"mixer_process_names\": [\"foo.exe\"],"
                                   "\"confirm_max_wait_ms\": 15000,"
                                   "\"devices\": [{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]"
                                   "}";

/**
 * @brief A settings file that has every key loads
 */
static void test_full(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kFull, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service != NULL && strcmp(s.ks_service, "audientusbaudioks") == 0);
  TEST_CHECK(OV_ARRAY_LENGTH(s.devices) == 1);
  if (TEST_CHECK(OV_ARRAY_LENGTH(s.devices) == 1)) {
    TEST_CHECK(s.devices[0].name != NULL && strcmp(s.devices[0].name, "iD14mkII") == 0);
    TEST_CHECK(s.devices[0].id != NULL && strcmp(s.devices[0].id, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == 0);
  }
  TEST_CHECK(s.veto_log_name != NULL && strcmp(s.veto_log_name, "System") == 0);
  TEST_CHECK(s.veto_provider_name != NULL && strcmp(s.veto_provider_name, "Microsoft-Windows-Kernel-PnP") == 0);
  TEST_CHECK(s.veto_event_id == 225);
  TEST_CHECK(OV_ARRAY_LENGTH(s.mixer_process_names) == 2);
  if (TEST_CHECK(OV_ARRAY_LENGTH(s.mixer_process_names) == 2)) {
    TEST_CHECK(s.mixer_process_names[0] != NULL && strcmp(s.mixer_process_names[0], "iD.exe") == 0);
    TEST_CHECK(s.mixer_process_names[1] != NULL && strcmp(s.mixer_process_names[1], "EVO.exe") == 0);
  }
  TEST_CHECK(s.task_name != NULL);

cleanup:
  settings_release(&s);
}

/**
 * @brief The theme is on until the settings file switches it off
 *
 * The undocumented entry points the theme works through are what a
 * future Windows may change and what an older one never had: one key
 * of the file has to be able to keep the whole theme out of the run,
 * and a file without the key has to keep the theme as it is.
 */
static void test_use_theme_key(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kRequiredOnly, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == true);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kThemeOff, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == false);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kThemeOn, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == true);

cleanup:
  settings_release(&s);
}

/**
 * @brief A theme key of the wrong type is refused
 */
static void test_use_theme_wrong_type(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"d\"],\"use_theme\":\"no\","
                                       "\"devices\":[{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_CHECK(s.ks_service == NULL); // released on failure
  settings_release(&s);
}

/**
 * @brief The window the confirmation waits in is what the settings file says
 *
 * The interval between two looks stays one second, but how long the confirmation is willing
 * to wait for the device to come back differs from machine to machine: a slow bus needs
 * more of it.  The key may be absent, and then the wait is the one of the defaults.
 */
static void test_confirm_max_wait_key(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kConfirmWait, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK_(s.confirm_max_wait_ms == 15000, "want the wait the file names, got %lu", s.confirm_max_wait_ms);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kRequiredOnly, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK_(s.confirm_max_wait_ms == adc_settings_confirm_max_wait_ms, "want the wait of the defaults, got %lu", s.confirm_max_wait_ms);

cleanup:
  settings_release(&s);
}

/**
 * @brief A wait nobody can finish is refused
 */
static void test_confirm_max_wait_out_of_range(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  {
    char const *const json = "{\"ks_service\":\"someks\",\"veto_log_name\":\"System\",\"veto_provider_name\":\"Some-Provider\","
                             "\"veto_event_id\":1,\"mixer_process_names\":[\"foo.exe\"],\"confirm_max_wait_ms\":99999999,"
                             "\"devices\":[{\"name\": \"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}";
    TEST_FAILED_WITH(settings_apply_json(json, &s, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  }

  settings_release(&s);
}

/**
 * @brief The keys that are marked optional may be absent
 */
static void test_optional_absent(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kRequiredOnly, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.task_name == NULL);
  TEST_CHECK(s.veto_event_id == 1);

cleanup:
  settings_release(&s);
}

/**
 * @brief The key an older build called healthy is still read
 */
static void test_legacy_healthy_key(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kLegacyHealthyKey, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service != NULL);

cleanup:
  settings_release(&s);
}

/**
 * @brief A file written for an older build must still load
 *
 * The two keys that named the restart of the mixer are gone from the settings: the
 * launcher brings the mixer back by itself and nothing consults the names.
 */
static void test_legacy_restart_keys(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kLegacyRestartKeys, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service != NULL);

cleanup:
  settings_release(&s);
}

/**
 * @brief A required key that is not there is refused
 */
static void test_missing_required(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(
      settings_apply_json("{\"veto_log_name\":\"System\"}", &s, &err), &err, ov_error_type_generic, ov_error_generic_not_found);
  TEST_CHECK(s.ks_service == NULL); // released on failure
  settings_release(&s);
}

/**
 * @brief The required number keys are refused when they are not there
 */
static void test_missing_required_number(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                       "\"mixer_process_names\":[\"d\"],\"devices\":[{\"name\": \"iD14mkII\", \"id\": "
                                       "\"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);
}

/**
 * @brief A key of the wrong type is refused
 */
static void test_wrong_types(void) {
  struct settings s;
  struct ov_error not_a_string = {0};
  struct ov_error not_a_number = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":1}", &s, &not_a_string),
                   &not_a_string,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                       "\"veto_event_id\":\"225\",\"mixer_process_names\":[\"d\"],\"devices\":[{\"name\": "
                                       "\"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}",
                                       &s,
                                       &not_a_number),
                   &not_a_number,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  settings_release(&s);
}

/**
 * @brief A number outside the range this program accepts is refused
 */
static void test_out_of_range(void) {
  struct settings s;
  struct ov_error too_small = {0};
  struct ov_error too_big = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                       "\"veto_event_id\":0,\"mixer_process_names\":[\"d\"],\"devices\":[{\"name\": \"iD14mkII\", "
                                       "\"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}",
                                       &s,
                                       &too_small),
                   &too_small,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                       "\"veto_event_id\":99999999,\"mixer_process_names\":[\"d\"],\"devices\":[{\"name\": "
                                       "\"iD14mkII\", \"id\": \"TUSBAUDIO_ENUM\\\\VID_2708&PID_0008&KS\"}]}",
                                       &s,
                                       &too_big),
                   &too_big,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);

  settings_release(&s);
}

/**
 * @brief Text that is no JSON is refused
 */
static void test_broken_json(void) {
  struct settings s;
  struct ov_error broken = {0};
  struct ov_error no_object = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(
      settings_apply_json("{ \"ks_service\": ", &s, &broken), &broken, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(settings_apply_json("[1,2]", &s, &no_object), &no_object, ov_error_type_generic, ov_error_generic_invalid_argument);

  settings_release(&s);
}

/**
 * @brief The arguments may be absent
 */
static void test_null_arguments(void) {
  struct settings s;
  struct ov_error no_json = {0};
  struct ov_error no_out = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json(NULL, &s, &no_json), &no_json, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(settings_apply_json("{}", NULL, &no_out), &no_out, ov_error_type_generic, ov_error_generic_invalid_argument);

  settings_release(&s);
}

/**
 * @brief Releasing twice must not crash
 */
static void test_release_is_idempotent(void) {
  struct settings s;
  struct ov_error err = {0};
  TEST_CHECK(settings_apply_json(kFull, &s, &err) == true);
  settings_release(&s);
  settings_release(&s); // double release must not touch freed memory
  settings_release(NULL);
  TEST_CHECK(s.ks_service == NULL);
}

/**
 * @brief The settings file is the executable with its extension replaced
 *
 * The same mechanism the log uses: the name is never written out here, it comes from the
 * name of the program, so a renamed program reads the settings beside itself.
 */
static void test_default_path(void) {
  struct ov_error err = {0};
  char *path = settings_default_path(&err);

  if (!TEST_SUCCEEDED(path != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(path, "test_settings.json") != NULL);
  TEST_CHECK(strstr(path, "audient-device-closer.json") == NULL);

cleanup:
  settings_string_free(&path);
}

/**
 * @brief A settings file that is not there is refused
 */
static void test_load_missing_file(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(
      settings_load("no-such-file-here.json", &s, &err), &err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  settings_release(&s);
}

/**
 * @brief The device table keeps the name and the identifier of every supported device
 */
static void test_devices_table(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_SUCCEEDED(settings_apply_json(kDevices, &s, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_LENGTH(s.devices) == 2);
  if (TEST_CHECK(OV_ARRAY_LENGTH(s.devices) == 2)) {
    TEST_CHECK(s.devices[0].name != NULL && strcmp(s.devices[0].name, "iD14mkII") == 0);
    TEST_CHECK(s.devices[0].id != NULL && strcmp(s.devices[0].id, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS") == 0);
    TEST_CHECK(s.devices[1].name != NULL && strcmp(s.devices[1].name, "iD44") == 0);
    TEST_CHECK(s.devices[1].id != NULL && strcmp(s.devices[1].id, "TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS") == 0);
  }

cleanup:
  settings_release(&s);
}

/**
 * @brief A device entry without its name or without its identifier is refused
 */
static void test_devices_missing_field(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"id\":\"b\"}],"
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\"}],"
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"\",\"id\":\"b\"}],"
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);
}

/**
 * @brief A device table with no device, or of the wrong shape, is refused
 */
static void test_devices_bad_shape(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[],"
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":\"nope\","
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":5,\"id\":\"b\"}],"
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);
}

/**
 * @brief A file that has no device table is refused
 */
static void test_devices_required(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\","
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"ks_instance_prefix\":\"TUSBAUDIO_ENUM\","
                                       "\"veto_log_name\":\"c\",\"veto_provider_name\":\"d\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"e\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);
}

/**
 * @brief A mixer list that has at least one image name and no empty one loads
 */
static void test_mixer_names(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\",\"id\":\"c\"}],"
                                       "\"veto_log_name\":\"d\",\"veto_provider_name\":\"e\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\",\"id\":\"c\"}],"
                                       "\"veto_log_name\":\"d\",\"veto_provider_name\":\"e\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":\"iD.exe\"}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\",\"id\":\"c\"}],"
                                       "\"veto_log_name\":\"d\",\"veto_provider_name\":\"e\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[7]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\",\"id\":\"c\"}],"
                                       "\"veto_log_name\":\"d\",\"veto_provider_name\":\"e\","
                                       "\"veto_event_id\":1,\"mixer_process_names\":[\"\"]}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_invalid_argument);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  TEST_FAILED_WITH(settings_apply_json("{\"ks_service\":\"a\",\"devices\":[{\"name\":\"b\",\"id\":\"c\"}],"
                                       "\"veto_log_name\":\"d\",\"veto_provider_name\":\"e\","
                                       "\"veto_event_id\":1}",
                                       &s,
                                       &err),
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_not_found);
  settings_release(&s);
}

TEST_LIST = {
    {"full", test_full},
    {"optional_absent", test_optional_absent},
    {"devices_table", test_devices_table},
    {"devices_missing_field", test_devices_missing_field},
    {"devices_bad_shape", test_devices_bad_shape},
    {"devices_required", test_devices_required},
    {"mixer_names", test_mixer_names},
    {"use_theme_key", test_use_theme_key},
    {"use_theme_wrong_type", test_use_theme_wrong_type},
    {"confirm_max_wait_key", test_confirm_max_wait_key},
    {"confirm_max_wait_out_of_range", test_confirm_max_wait_out_of_range},
    {"legacy_healthy_key", test_legacy_healthy_key},
    {"legacy_restart_keys", test_legacy_restart_keys},
    {"missing_required", test_missing_required},
    {"missing_required_number", test_missing_required_number},
    {"wrong_types", test_wrong_types},
    {"out_of_range", test_out_of_range},
    {"broken_json", test_broken_json},
    {"null_arguments", test_null_arguments},
    {"release_idempotent", test_release_is_idempotent},
    {"default_path", test_default_path},
    {"load_missing_file", test_load_missing_file},
    {NULL, NULL},
};
