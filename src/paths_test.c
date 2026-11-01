/**
 * Tests for the paths this program derives from the location of its executable.  The log and
 * the settings file are the name of the executable with a different extension, so the cases
 * below name both extensions: the file has to be where the executable is and carry the name of
 * the executable, whatever that name is.
 */
#include <ovtest.h>

#include <windows.h>

#include <shlobj.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "paths.h"

/**
 * @brief Does the text end with the suffix
 *
 * @param text the text to look at
 * @param suffix the ending to look for
 * @return true when text ends with suffix
 */
static bool ends_with(wchar_t const *const text, wchar_t const *const suffix) {
  size_t const len = wcslen(text);
  size_t const sub = wcslen(suffix);

  return (len >= sub) && (memcmp(&text[len - sub], suffix, sub * sizeof(text[0])) == 0);
}

/**
 * @brief The extension of the executable is replaced, the one behind the last separator only
 */
static void test_replace_extension(void) {
  struct ov_error err = {0};
  wchar_t *p = NULL;

  p = paths_replace_extension(L"C:\\dir\\audient-device-closer.exe", L".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"C:\\dir\\audient-device-closer.json") == 0);
    OV_ARRAY_DESTROY(&p);
  }
  p = paths_replace_extension(L"C:\\dir\\audient-device-closer.exe", L".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"C:\\dir\\audient-device-closer.log") == 0);
    OV_ARRAY_DESTROY(&p);
  }
  p = paths_replace_extension(L"C:\\dir\\my.tool\\app", L".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"C:\\dir\\my.tool\\app.log") == 0); // a dot inside a directory is not an extension
    OV_ARRAY_DESTROY(&p);
  }
  p = paths_replace_extension(L"C:\\a.b.c\\d.e.exe", L".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"C:\\a.b.c\\d.e.json") == 0);
    OV_ARRAY_DESTROY(&p);
  }
  p = paths_replace_extension(L"relative.exe", L".log", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"relative.log") == 0);
    OV_ARRAY_DESTROY(&p);
  }
  p = paths_replace_extension(L"drive:folder\\app.exe", L".json", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(wcscmp(p, L"drive:folder\\app.json") == 0);
    OV_ARRAY_DESTROY(&p);
  }
  TEST_FAILED_WITH(paths_replace_extension(NULL, L".json", &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(
      paths_replace_extension(L"C:\\dir\\app.exe", NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief The file name is what sits behind the last separator
 */
static void test_file_name(void) {
  TEST_CHECK(wcscmp(paths_file_name(L"C:\\dir\\audient-device-closer.json"), L"audient-device-closer.json") == 0);
  TEST_CHECK(wcscmp(paths_file_name(L"C:\\a.b.c\\d.e.json"), L"d.e.json") == 0);
  TEST_CHECK(wcscmp(paths_file_name(L"forward/dir/app.json"), L"app.json") == 0);
  TEST_CHECK(wcscmp(paths_file_name(L"drive:folder\\app.json"), L"app.json") == 0);
  TEST_CHECK(wcscmp(paths_file_name(L"relative.json"), L"relative.json") == 0);
  TEST_CHECK(wcscmp(paths_file_name(L"C:\\dir\\"), L"") == 0);
  TEST_CHECK(paths_file_name(NULL) == NULL);
}

/**
 * @brief A bare tail match is not enough for the name at the end of a path
 *
 * The name at the end of the path is what decides whether a record is about this tool's
 * business, so "foo.exe" must not be taken for "foobar.exe".
 */
static void test_file_name_matches(void) {
  TEST_CHECK(paths_file_name_matches(L"\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe", L"audiodg.exe") == true);
  TEST_CHECK(paths_file_name_matches(L"C:\\WINDOWS\\system32\\AUDIODG.EXE", L"audiodg.exe") == true);
  TEST_CHECK(paths_file_name_matches(L"C:/x/y/foo.exe", L"foo.exe") == true);
  TEST_CHECK(paths_file_name_matches(L"C:\\dir\\foobar.exe", L"foo.exe") == false);
  TEST_CHECK(paths_file_name_matches(L"C:\\dir\\bar", L"foo.exe") == false);
  TEST_CHECK(paths_file_name_matches(L"short", L"muchlongername.exe") == false);
  TEST_CHECK(paths_file_name_matches(NULL, L"iD.exe") == false);
  TEST_CHECK(paths_file_name_matches(L"C:\\x\\iD.exe", NULL) == false);
  TEST_CHECK(paths_file_name_matches(L"C:\\x\\iD.exe", L"") == false);
  TEST_CHECK(paths_file_name_matches(L"iD.exe", L"iD.exe") == true);
}

/**
 * @brief The log and the settings of the running program are one name with two extensions
 */
static void test_files_beside_the_running_executable(void) {
  static wchar_t const kLog[] = L".log";
  static wchar_t const kJson[] = L".json";
  struct ov_error err = {0};
  wchar_t *exe = NULL;
  wchar_t *log = NULL;
  wchar_t *json = NULL;

  exe = paths_executable(&err);
  if (!TEST_SUCCEEDED(exe != NULL, &err)) {
    goto cleanup;
  }
  log = paths_replace_extension(exe, kLog, &err);
  if (!TEST_SUCCEEDED(log != NULL, &err)) {
    goto cleanup;
  }
  json = paths_replace_extension(exe, kJson, &err);
  if (!TEST_SUCCEEDED(json != NULL, &err)) {
    goto cleanup;
  }
  if (!TEST_CHECK(ends_with(log, kLog) && ends_with(json, kJson))) {
    goto cleanup;
  }
  // everything before the two extensions is the path of the executable itself
  TEST_CHECK(wcsncmp(log, json, wcslen(log) - wcslen(kLog)) == 0);
  TEST_CHECK(wcsncmp(exe, json, wcslen(json) - wcslen(kJson)) == 0);
  TEST_CHECK(ends_with(json, L"test_paths.json"));

cleanup:
  if (exe != NULL) {
    OV_ARRAY_DESTROY(&exe);
  }
  if (log != NULL) {
    OV_ARRAY_DESTROY(&log);
  }
  if (json != NULL) {
    OV_ARRAY_DESTROY(&json);
  }
}

/**
 * @brief The log folder belongs to the machine, not to the person who happens to be signed in
 *
 * The task scheduler starts a repair as the system account, and the application data of that
 * account is a folder nobody at the screen will ever open.  The log of the run that fixed the
 * device while the person was away has to be where the person can read it, so both kinds of run
 * write into the machine-wide application data folder.
 */
static void test_log_folder_is_the_machines(void) {
  struct ov_error err = {0};
  wchar_t *folder = NULL;
  wchar_t const *shared = NULL;
  wchar_t const *personal = NULL;

  shared = _wgetenv(L"ProgramData");
  personal = _wgetenv(L"LOCALAPPDATA");
  if ((shared == NULL) || (personal == NULL)) {
    TEST_SKIP("want both application data folders in the environment");
    return;
  }
  folder = paths_log_folder(&err);
  if (!TEST_SUCCEEDED(folder != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsncmp(folder, shared, wcslen(shared)) == 0);
  TEST_MSG("want the log folder inside %ls, got %ls", shared, folder);
  TEST_CHECK(ends_with(folder, L"\\AudientDeviceCloser"));
  TEST_MSG("want this program's own folder under the machine-wide application data: %ls", folder);
  TEST_CHECK(wcsncmp(folder, personal, wcslen(personal)) != 0);
  TEST_MSG("the log folder must not be the personal one: %ls", folder);

cleanup:
  if (folder != NULL) {
    OV_ARRAY_DESTROY(&folder);
  }
}

/**
 * @brief The folder is made when it is not there, and an existing one is no failure
 *
 * The log of an installed copy opens inside the machine-wide folder, which is not there before the
 * first run.  The next run finds the folder it made, and that is the answer the caller wanted.
 */
static void test_create_folder(void) {
  struct ov_error err = {0};
  wchar_t tmp[512];
  wchar_t *folder = NULL;
  DWORD len = 0;

  len = GetTempPathW((DWORD)(sizeof(tmp) / sizeof(tmp[0])), tmp);
  if ((len == 0) || (len >= (DWORD)(sizeof(tmp) / sizeof(tmp[0])))) {
    TEST_SKIP("want a temporary folder to build in");
    return;
  }
  if (!TEST_SUCCEEDED(ov_sprintf_wchar(&folder, &err, NULL, L"%ls\\adc-paths-test-%lu", tmp, (unsigned long)GetCurrentProcessId()), &err)) {
    goto cleanup;
  }
  RemoveDirectoryW(folder); // a run that left the folder behind must not decide the first case

  TEST_CASE("the folder is made when nothing is there");
  TEST_SUCCEEDED(paths_create_folder(folder, &err), &err);
  TEST_CHECK(GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES);
  TEST_MSG("want %ls to exist after the create", folder);

  TEST_CASE("a folder that is already there is the answer as well");
  TEST_SUCCEEDED(paths_create_folder(folder, &err), &err);

  TEST_CASE("no name is refused");
  TEST_FAILED_WITH(paths_create_folder(NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);

cleanup:
  if (folder != NULL) {
    RemoveDirectoryW(folder);
    OV_ARRAY_DESTROY(&folder);
  }
}

/**
 * @brief The folder of a Program Files is what the system says it is
 *
 * The folder name is not guessed: a machine can put its programs somewhere else, and the
 * text of a path says nothing about where the file sits.  A file under the folder the
 * system names is in it, a file that sits elsewhere is not, and a path that merely spells
 * the name of the folder is not one of its files.
 */
static void test_is_in_program_files(void) {
  struct ov_error err = {0};
  wchar_t *program_files = NULL;
  wchar_t *inside = NULL;
  wchar_t *prefix = NULL;
  wchar_t *lookalike = NULL;
  wchar_t *elsewhere = NULL;
  HRESULT const hr = SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &program_files);

  if (FAILED(hr) || (program_files == NULL)) {
    TEST_SKIP("want the Program Files folder of this machine");
    return;
  }
  if (!TEST_SUCCEEDED(ov_sprintf_wchar(&inside, &err, NULL, L"%ls\\AudientDeviceCloser\\app.exe", program_files), &err)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(ov_sprintf_wchar(&prefix, &err, NULL, L"%lsExtra\\app.exe", program_files), &err)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(ov_sprintf_wchar(&lookalike, &err, NULL, L"C:\\somewhere\\Program Files\\app.exe"), &err)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(ov_sprintf_wchar(&elsewhere, &err, NULL, L"C:\\somewhere\\AudientDeviceCloser\\app.exe"), &err)) {
    goto cleanup;
  }

  TEST_CASE("a file under the folder the system names is in it");
  TEST_SUCCEEDED(paths_is_in_program_files(inside, &err) == ov_true, &err);

  TEST_CASE("a sibling folder that spells the same name in front is not it");
  TEST_SUCCEEDED(paths_is_in_program_files(prefix, &err) == ov_false, &err);

  TEST_CASE("a path elsewhere that spells the name of the folder is not one of its files");
  TEST_SUCCEEDED(paths_is_in_program_files(lookalike, &err) == ov_false, &err);
  TEST_SUCCEEDED(paths_is_in_program_files(elsewhere, &err) == ov_false, &err);

  TEST_CASE("a path too long for the comparison is refused");
  {
    wchar_t too_long[MAX_PATH + 2];

    for (size_t i = 0; i < (sizeof(too_long) / sizeof(too_long[0])) - 1; i++) {
      too_long[i] = L'a';
    }
    too_long[(sizeof(too_long) / sizeof(too_long[0])) - 1] = L'\0';
    TEST_FAILED_WITH(
        paths_is_in_program_files(too_long, &err) != ov_indeterminate, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  }

  TEST_CASE("no path is refused");
  TEST_FAILED_WITH(
      paths_is_in_program_files(NULL, &err) != ov_indeterminate, &err, ov_error_type_generic, ov_error_generic_invalid_argument);

cleanup:
  CoTaskMemFree(program_files);
  if (inside != NULL) {
    OV_ARRAY_DESTROY(&inside);
  }
  if (prefix != NULL) {
    OV_ARRAY_DESTROY(&prefix);
  }
  if (lookalike != NULL) {
    OV_ARRAY_DESTROY(&lookalike);
  }
  if (elsewhere != NULL) {
    OV_ARRAY_DESTROY(&elsewhere);
  }
}

TEST_LIST = {
    {"replace_extension", test_replace_extension},
    {"file_name", test_file_name},
    {"file_name_matches", test_file_name_matches},
    {"files_beside_the_running_executable", test_files_beside_the_running_executable},
    {"is_in_program_files", test_is_in_program_files},
    {"log_folder_is_the_machines", test_log_folder_is_the_machines},
    {"create_folder", test_create_folder},
    {NULL, NULL},
};
