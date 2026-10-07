/**
 * Tests for the paths this program derives from the location of its executable.  The log and
 * the settings file are the name of the executable with a different extension, so the cases
 * below name both extensions: the file has to be where the executable is and carry the name of
 * the executable, whatever that name is.
 */
#include <ovtest.h>

#include <stdbool.h>
#include <string.h>

#include "paths.h"

/**
 * @brief Does the text end with the suffix
 *
 * @param text the text to look at
 * @param suffix the ending to look for
 * @return true when text ends with suffix
 */
static bool ends_with(char const *const text, char const *const suffix) {
  size_t const len = strlen(text);
  size_t const sub = strlen(suffix);

  return (len >= sub) && (memcmp(&text[len - sub], suffix, sub) == 0);
}

/**
 * @brief The extension of the executable is replaced, the one behind the last separator only
 */
static void test_replace_extension(void) {
  struct ov_error err = {0};
  char *p = NULL;

  p = paths_replace_extension("C:\\dir\\audient-device-closer.exe", ".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\dir\\audient-device-closer.json") == 0);
    OV_FREE(&p);
  }
  p = paths_replace_extension("C:\\dir\\audient-device-closer.exe", ".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\dir\\audient-device-closer.log") == 0);
    OV_FREE(&p);
  }
  p = paths_replace_extension("C:\\dir\\my.tool\\app", ".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\dir\\my.tool\\app.log") == 0); // a dot inside a directory is not an extension
    OV_FREE(&p);
  }
  p = paths_replace_extension("C:\\a.b.c\\d.e.exe", ".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\a.b.c\\d.e.json") == 0);
    OV_FREE(&p);
  }
  p = paths_replace_extension("relative.exe", ".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "relative.log") == 0);
    OV_FREE(&p);
  }
  p = paths_replace_extension("drive:folder\\app.exe", ".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "drive:folder\\app.json") == 0);
    OV_FREE(&p);
  }
  TEST_FAILED_WITH(paths_replace_extension(NULL, ".json", &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(paths_replace_extension("C:\\dir\\app.exe", NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief The file name is what sits behind the last separator
 */
static void test_file_name(void) {
  TEST_CHECK(strcmp(paths_file_name("C:\\dir\\audient-device-closer.json"), "audient-device-closer.json") == 0);
  TEST_CHECK(strcmp(paths_file_name("C:\\a.b.c\\d.e.json"), "d.e.json") == 0);
  TEST_CHECK(strcmp(paths_file_name("forward/dir/app.json"), "app.json") == 0);
  TEST_CHECK(strcmp(paths_file_name("drive:folder\\app.json"), "app.json") == 0);
  TEST_CHECK(strcmp(paths_file_name("relative.json"), "relative.json") == 0);
  TEST_CHECK(strcmp(paths_file_name("C:\\dir\\"), "") == 0);
  TEST_CHECK(paths_file_name(NULL) == NULL);
}

/**
 * @brief The log and the settings of the running program are one name with two extensions
 */
static void test_files_beside_the_running_executable(void) {
  static char const kLog[] = ".log";
  static char const kJson[] = ".json";
  struct ov_error err = {0};
  char *exe = NULL;
  char *log = NULL;
  char *json = NULL;

  exe = paths_executable(&err);
  if (!TEST_CHECK(exe != NULL)) {
    goto cleanup;
  }
  log = paths_replace_extension(exe, kLog, &err);
  if (!TEST_CHECK(log != NULL)) {
    goto cleanup;
  }
  json = paths_replace_extension(exe, kJson, &err);
  if (!TEST_CHECK(json != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(ends_with(log, kLog) && ends_with(json, kJson))) {
    goto cleanup;
  }
  // everything before the two extensions is the path of the executable itself
  TEST_CHECK(strncmp(log, json, strlen(log) - strlen(kLog)) == 0);
  TEST_CHECK(strncmp(exe, json, strlen(json) - strlen(kJson)) == 0);
  TEST_CHECK(ends_with(json, "test_paths.json"));

cleanup:
  if (exe != NULL) {
    paths_string_free(&exe);
  }
  if (log != NULL) {
    OV_FREE(&log);
  }
  if (json != NULL) {
    OV_FREE(&json);
  }
  OV_ERROR_REPORT(&err, NULL);
}

TEST_LIST = {
    {"replace_extension", test_replace_extension},
    {"file_name", test_file_name},
    {"files_beside_the_running_executable", test_files_beside_the_running_executable},
    {NULL, NULL},
};
