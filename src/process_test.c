/**
 * Tests for the generic process helpers.
 *
 * The process we kill is a dummy we started ourselves, so the test works on a CI runner
 * too and never touches a real Audient process.
 */
#include <ovtest.h>

#include <windows.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include <string.h>

#include "process.h"

/**
 * @brief Start the dummy process the checks close again
 *
 * @param cmdline_ascii the command line to start it with, UTF-8
 * @param pi receives the handle of the process
 * @return false when the dummy could not be started
 */
static bool spawn_dummy(char const *const cmdline_ascii, PROCESS_INFORMATION *const pi) {
  STARTUPINFOW si;
  wchar_t *cmdline = NULL;
  BOOL ok = FALSE;
  bool success = false;

  memset(pi, 0, sizeof(*pi));
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (!ov_sprintf_wchar(&cmdline, NULL, NULL, L"%s", cmdline_ascii)) {
    goto cleanup;
  }
  ok = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
  success = ok ? true : false;

cleanup:
  if (cmdline != NULL) {
    OV_ARRAY_DESTROY(&cmdline); // made by ov_sprintf_wchar, not by OV_REALLOC
    cmdline = NULL;
  }
  return success;
}

/**
 * @brief Give the handles of the dummy back
 *
 * @param pi the process to close
 */
static void close_pi(PROCESS_INFORMATION *const pi) {
  if (pi->hThread) {
    CloseHandle(pi->hThread);
    pi->hThread = NULL;
  }
  if (pi->hProcess) {
    CloseHandle(pi->hProcess);
    pi->hProcess = NULL;
  }
}

/**
 * @brief Terminate a process we do not own, then wait for it
 *
 * The point of the module.
 */
static void test_terminate_and_wait(void) {
  struct ov_error err = {0};
  PROCESS_INFORMATION pi;
  struct process_ref ref;
  char *image = NULL;
  bool gone = false;

  memset(&pi, 0, sizeof(pi));
  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if (!spawn_dummy("cmd.exe /c ping -n 30 127.0.0.1 > nul", &pi)) {
    TEST_SKIP("cannot spawn here");
    goto cleanup;
  }
  if (!TEST_CHECK(process_ref_open(&ref, (uint32_t)pi.dwProcessId, true, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(process_ref_alive(&ref) == true);
  TEST_CHECK(ref.start_ms_utc > 0);
  image = process_ref_image_path(&ref, &err);
  if (TEST_CHECK(image != NULL)) {
    TEST_CHECK(process_path_matches_name(image, "cmd.exe") == true);
  }
  TEST_CHECK(process_ref_terminate(&ref, &err) == true);
  TEST_CHECK(process_ref_wait(&ref, 5000, &gone, &err) == true);
  TEST_CHECK(gone == true);
  TEST_CHECK(process_ref_alive(&ref) == false);

  if (pi.hProcess != NULL) {
    WaitForSingleObject(pi.hProcess, 5000);
  }

cleanup:
  process_string_free(&image);
  process_ref_close(&ref);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief Opening a pid that never existed fails, and the failure is an HRESULT
 */
static void test_terminate_unknown_pid(void) {
  struct ov_error err = {0};
  struct process_ref ref;
  int code = 0;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if (!TEST_CHECK(process_ref_open(&ref, 0xFFFFFFFFu, true, &err) == false)) {
    goto cleanup;
  }
  TEST_CHECK(ov_error_get_code(&err, ov_error_type_hresult, &code));

cleanup:
  process_ref_close(&ref);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief The arguments may be absent
 */
static void test_null_arguments(void) {
  char *text = NULL;
  struct process_ref ref;
  struct ov_error err = {0};
  bool gone = false;
  bool ok = false;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  process_string_free(&text); // NULL string must be tolerated
  process_ref_close(&ref);    // a ref that was never opened must be tolerated
  TEST_CHECK(text == NULL);
  TEST_CHECK(process_ref_alive(&ref) == false);
  ok = process_ref_open(NULL, 1, false, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  ok = process_ref_open(&ref, 1, false, NULL);
  TEST_CHECK(ok == false);
  ok = process_ref_wait(&ref, 10, &gone, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  ok = process_ref_terminate(&ref, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief A process that started after the event cannot be the one the event named
 *
 * The identity rule that keeps this tool off the wrong process: a pid is reused only
 * after its previous owner is gone, so a process that started after the event cannot be
 * the one the event named.  Without both times there is nothing to compare and the answer
 * is no.
 */
static void test_start_matches_event_time(void) {
  TEST_CHECK(process_start_matches_event_time(1000, 2000) == true);  // started before the event
  TEST_CHECK(process_start_matches_event_time(2000, 2000) == true);  // started in the same millisecond
  TEST_CHECK(process_start_matches_event_time(2001, 2000) == false); // started after the event: another instance
  TEST_CHECK(process_start_matches_event_time(0, 2000) == false);    // the start time is unknown
  TEST_CHECK(process_start_matches_event_time(1000, 0) == false);    // the event time is unknown
  TEST_CHECK(process_start_matches_event_time(-1, 2000) == false);
  TEST_CHECK(process_start_matches_event_time(1000, -1) == false);
}

/**
 * @brief A bare tail match is not enough for the name at the end of a path
 *
 * The name at the end of the path is what decides whether a record is about this tool's
 * business.
 */
static void test_path_matches_name(void) {
  TEST_CHECK(process_path_matches_name("\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe", "audiodg.exe") == true);
  TEST_CHECK(process_path_matches_name("C:\\WINDOWS\\system32\\AUDIODG.EXE", "audiodg.exe") == true);
  TEST_CHECK(process_path_matches_name("C:/x/y/foo.exe", "foo.exe") == true);
  TEST_CHECK(process_path_matches_name("C:\\dir\\foobar.exe", "foo.exe") == false);
  TEST_CHECK(process_path_matches_name("C:\\dir\\bar", "foo.exe") == false);
  TEST_CHECK(process_path_matches_name("short", "muchlongername.exe") == false);
  TEST_CHECK(process_path_matches_name(NULL, "iD.exe") == false);
  TEST_CHECK(process_path_matches_name("C:\\x\\iD.exe", NULL) == false);
  TEST_CHECK(process_path_matches_name("C:\\x\\iD.exe", "") == false);
  TEST_CHECK(process_path_matches_name("iD.exe", "iD.exe") == true);
}

TEST_LIST = {
    {"terminate_and_wait", test_terminate_and_wait},
    {"terminate_unknown_pid", test_terminate_unknown_pid},
    {"start_matches_event_time", test_start_matches_event_time},
    {"path_matches_name", test_path_matches_name},
    {"null_arguments", test_null_arguments},
    {NULL, NULL},
};
