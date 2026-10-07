#include <ovtest.h>

#include <string.h>

#include "settings.h"

static char const kFull[] = "{"
                            "\"ks_service\": \"audientusbaudioks\","
                            "\"ks_instance_prefix\": \"TUSBAUDIO_ENUM\","
                            "\"endpoint_name_pattern\": \"Audient\","
                            "\"veto_log_name\": \"System\","
                            "\"veto_provider_name\": \"Microsoft-Windows-Kernel-PnP\","
                            "\"veto_event_id\": 225,"
                            "\"mixer_process_name\": \"iD.exe\","
                            "\"task_name\": \"\\\\AudientDeviceCloser\""
                            "}";

static char const kRequiredOnly[] = "{"
                                    "\"ks_service\": \"someks\","
                                    "\"veto_log_name\": \"System\","
                                    "\"veto_provider_name\": \"Some-Provider\","
                                    "\"veto_event_id\": 1,"
                                    "\"mixer_process_name\": \"foo.exe\""
                                    "}";

static char const kLegacyHealthyKey[] = "{\"ks_service\":\"someks\",\"veto_log_name\":\"System\",\"veto_provider_name\":\"Some-Provider\","
                                        "\"veto_event_id\":1,\"mixer_process_name\":\"foo.exe\",\"allow_healthy_close\":true}";

static char const kLegacyRestartKeys[] = "{\"ks_service\":\"someks\",\"veto_log_name\":\"System\",\"veto_provider_name\":\"Some-Provider\","
                                         "\"veto_event_id\":1,\"mixer_process_name\":\"foo.exe\","
                                         "\"mixer_fallback_path\":\"C:/Program Files/Audient/iD/iD.exe\","
                                         "\"launcher_process_name\":\"AudientAppLauncher.exe\"}";

static char const kThemeOff[] = "{"
                                "\"ks_service\": \"someks\","
                                "\"veto_log_name\": \"System\","
                                "\"veto_provider_name\": \"Some-Provider\","
                                "\"veto_event_id\": 1,"
                                "\"mixer_process_name\": \"foo.exe\","
                                "\"use_theme\": false"
                                "}";

static char const kThemeOn[] = "{"
                               "\"ks_service\": \"someks\","
                               "\"veto_log_name\": \"System\","
                               "\"veto_provider_name\": \"Some-Provider\","
                               "\"veto_event_id\": 1,"
                               "\"mixer_process_name\": \"foo.exe\","
                               "\"use_theme\": true"
                               "}";

/**
 * @brief A settings file that has every key loads
 */
static void test_full(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json(kFull, &s, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service != NULL && strcmp(s.ks_service, "audientusbaudioks") == 0);
  TEST_CHECK(s.ks_instance_prefix != NULL && strcmp(s.ks_instance_prefix, "TUSBAUDIO_ENUM") == 0);
  TEST_CHECK(s.endpoint_name_pattern != NULL && strcmp(s.endpoint_name_pattern, "Audient") == 0);
  TEST_CHECK(s.veto_log_name != NULL && strcmp(s.veto_log_name, "System") == 0);
  TEST_CHECK(s.veto_provider_name != NULL && strcmp(s.veto_provider_name, "Microsoft-Windows-Kernel-PnP") == 0);
  TEST_CHECK(s.veto_event_id == 225);
  TEST_CHECK(s.mixer_process_name != NULL && strcmp(s.mixer_process_name, "iD.exe") == 0);
  TEST_CHECK(s.task_name != NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
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
  if (!TEST_CHECK(settings_apply_json(kRequiredOnly, &s, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == true);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json(kThemeOff, &s, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == false);
  settings_release(&s);

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json(kThemeOn, &s, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(s.use_theme == true);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

/**
 * @brief A theme key of the wrong type is refused
 */
static void test_use_theme_wrong_type(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\","
                                      "\"veto_event_id\":1,\"mixer_process_name\":\"d\",\"use_theme\":\"no\"}",
                                      &s,
                                      &err) == false)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service == NULL); // released on failure

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

/**
 * @brief The keys that are marked optional may be absent
 */
static void test_optional_absent(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json(kRequiredOnly, &s, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_instance_prefix == NULL);
  TEST_CHECK(s.endpoint_name_pattern == NULL);
  TEST_CHECK(s.task_name == NULL);
  TEST_CHECK(s.veto_event_id == 1);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

/**
 * @brief The key an older build called healthy is still read
 */
static void test_legacy_healthy_key(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json(kLegacyHealthyKey, &s, &err) == true)) {
    goto cleanup;
  }

cleanup:
  OV_ERROR_REPORT(&err, NULL);
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
  if (!TEST_CHECK(settings_apply_json(kLegacyRestartKeys, &s, &err) == true)) {
    goto cleanup;
  }

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

/**
 * @brief A required key that is not there is refused
 */
static void test_missing_required(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json("{\"veto_log_name\":\"System\"}", &s, &err) == false)) {
    goto cleanup;
  }
  TEST_CHECK(s.ks_service == NULL); // released on failure

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

/**
 * @brief The report of a bad key names the read that raised it and the read that asked for it
 *
 * The readers of the settings file raise their errors where they noticed the problem, and the
 * caller adds its own position to the stack, so the report shows both: the helper that found
 * the key missing and the line of the file that wanted the key.
 */
static void test_error_names_reader_and_caller(void) {
  struct settings s;
  struct ov_error err = {0};
  char *text = NULL;

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_apply_json("{\"veto_log_name\":\"System\"}", &s, &err) == false)) {
    OV_ERROR_REPORT(&err, NULL);
    goto cleanup;
  }
  if (!TEST_CHECK(ov_error_to_string(&err, &text, true, NULL) == true)) {
    OV_ERROR_REPORT(&err, NULL);
    goto cleanup;
  }
  TEST_CHECK(strstr(text, "the settings file has no ks_service") != NULL);
  TEST_MSG("want the key of the file in the report, got %s", text);
  TEST_CHECK(strstr(text, "get_str") != NULL);
  TEST_MSG("want the reader that raised the error in the report, got %s", text);
  TEST_CHECK(strstr(text, "settings_apply_json") != NULL);
  TEST_MSG("want the reader that asked for the key in the report, got %s", text);
  OV_ERROR_DESTROY(&err);

cleanup:
  if (text != NULL) {
    OV_ARRAY_DESTROY(&text);
  }
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
  if (!TEST_CHECK(settings_apply_json("{\"ks_service\":1}", &s, &not_a_string) == false)) {
    goto cleanup;
  }
  TEST_CHECK(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\",\"veto_event_id\":\"225\","
                                 "\"mixer_process_name\":\"d\"}",
                                 &s,
                                 &not_a_number) == false);

cleanup:
  OV_ERROR_REPORT(&not_a_string, NULL);
  OV_ERROR_REPORT(&not_a_number, NULL);
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
  if (!TEST_CHECK(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\",\"veto_event_id\":0,"
                                      "\"mixer_process_name\":\"d\"}",
                                      &s,
                                      &too_small) == false)) {
    goto cleanup;
  }
  TEST_CHECK(settings_apply_json("{\"ks_service\":\"a\",\"veto_log_name\":\"b\",\"veto_provider_name\":\"c\",\"veto_event_id\":99999999,"
                                 "\"mixer_process_name\":\"d\"}",
                                 &s,
                                 &too_big) == false);

cleanup:
  OV_ERROR_REPORT(&too_small, NULL);
  OV_ERROR_REPORT(&too_big, NULL);
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
  if (!TEST_CHECK(settings_apply_json("{ \"ks_service\": ", &s, &broken) == false)) {
    goto cleanup;
  }
  TEST_CHECK(settings_apply_json("[1,2]", &s, &no_object) == false);

cleanup:
  OV_ERROR_REPORT(&broken, NULL);
  OV_ERROR_REPORT(&no_object, NULL);
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
  if (!TEST_CHECK(settings_apply_json(NULL, &s, &no_json) == false)) {
    goto cleanup;
  }
  TEST_CHECK(settings_apply_json("{}", NULL, &no_out) == false);

cleanup:
  OV_ERROR_REPORT(&no_json, NULL);
  OV_ERROR_REPORT(&no_out, NULL);
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

  if (!TEST_CHECK(path != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(path, "test_settings.json") != NULL);
  TEST_CHECK(strstr(path, "audient-device-closer.json") == NULL);

cleanup:
  settings_string_free(&path);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief A settings file that is not there is refused
 */
static void test_load_missing_file(void) {
  struct settings s;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  if (!TEST_CHECK(settings_load("no-such-file-here.json", &s, &err) == false)) {
    goto cleanup;
  }

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  settings_release(&s);
}

TEST_LIST = {
    {"full", test_full},
    {"optional_absent", test_optional_absent},
    {"use_theme_key", test_use_theme_key},
    {"use_theme_wrong_type", test_use_theme_wrong_type},
    {"legacy_healthy_key", test_legacy_healthy_key},
    {"legacy_restart_keys", test_legacy_restart_keys},
    {"missing_required", test_missing_required},
    {"error_names_reader_and_caller", test_error_names_reader_and_caller},
    {"wrong_types", test_wrong_types},
    {"out_of_range", test_out_of_range},
    {"broken_json", test_broken_json},
    {"null_arguments", test_null_arguments},
    {"release_idempotent", test_release_is_idempotent},
    {"default_path", test_default_path},
    {"load_missing_file", test_load_missing_file},
    {NULL, NULL},
};
