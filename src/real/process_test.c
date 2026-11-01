/**
 * Tests for the machine of the system that ends a process.
 *
 * The process we kill is a dummy we started ourselves, so the test works on a CI runner too
 * and never touches a real Audient process.
 */
#include <windows.h>

#include <ovtest.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include <string.h>

#include "real/process.c"

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
    OV_ARRAY_DESTROY(&cmdline);
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
 * The point of the machine: the handle keeps the id on the instance that was checked, so the
 * termination lands on that one and the wait answers once it is gone.
 */
static void test_terminate_and_wait(void) {
  struct ov_error err = {0};
  PROCESS_INFORMATION pi;
  struct process_ref ref;
  HANDLE stop = NULL;

  memset(&pi, 0, sizeof(pi));
  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  if (!spawn_dummy("cmd.exe /c ping -n 30 127.0.0.1 > nul", &pi)) {
    TEST_SKIP("cannot spawn here");
    goto cleanup;
  }
  stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(stop != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(process_ref_open(&ref, (uint32_t)pi.dwProcessId, true, &err), &err)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(process_ref_terminate(&ref, &err), &err);
  TEST_SUCCEEDED(wait_gone(&ref, 5000, stop, &err) == ov_true, &err);
  TEST_MSG("want the process gone after the termination");

cleanup:
  TEST_SUCCEEDED(process_ref_close(&ref, false, &err), &err);
  if (stop != NULL) {
    CloseHandle(stop);
  }
  if (pi.hProcess != NULL) {
    WaitForSingleObject(pi.hProcess, 5000);
  }
  close_pi(&pi);
}

/**
 * @brief A process that keeps running is not gone when the bound is up
 *
 * The wait has to answer that it did not happen rather than hang until the process leaves.
 */
static void test_wait_times_out(void) {
  struct ov_error err = {0};
  PROCESS_INFORMATION pi;
  struct process_ref ref;
  HANDLE stop = NULL;

  memset(&pi, 0, sizeof(pi));
  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  if (!spawn_dummy("cmd.exe /c ping -n 30 127.0.0.1 > nul", &pi)) {
    TEST_SKIP("cannot spawn here");
    goto cleanup;
  }
  stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(stop != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(process_ref_open(&ref, (uint32_t)pi.dwProcessId, false, &err), &err)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(wait_gone(&ref, 0, stop, &err) == ov_false, &err);
  TEST_MSG("want the process still there after a bound of nothing");

cleanup:
  TEST_SUCCEEDED(process_ref_close(&ref, false, &err), &err);
  if (stop != NULL) {
    CloseHandle(stop);
  }
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
  }
  close_pi(&pi);
}

/**
 * @brief An ask to stop ends the wait where it stands
 *
 * The close of a mixer is the longest step of a repair, so an ask that comes during it must
 * not be held up until the process leaves.  The wait then has no answer, which is not the
 * same as an answer that the process stayed.
 */
static void test_stop_ends_the_wait(void) {
  struct ov_error err = {0};
  PROCESS_INFORMATION pi;
  struct process_ref ref;
  HANDLE stop = NULL;

  memset(&pi, 0, sizeof(pi));
  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  if (!spawn_dummy("cmd.exe /c ping -n 30 127.0.0.1 > nul", &pi)) {
    TEST_SKIP("cannot spawn here");
    goto cleanup;
  }
  stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(stop != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(process_ref_open(&ref, (uint32_t)pi.dwProcessId, false, &err), &err)) {
    goto cleanup;
  }
  SetEvent(stop);
  TEST_FAILED_WITH(wait_gone(&ref, 5000, stop, &err) != ov_indeterminate, &err, ov_error_type_generic, ov_error_generic_abort);

cleanup:
  TEST_SUCCEEDED(process_ref_close(&ref, false, &err), &err);
  if (stop != NULL) {
    CloseHandle(stop);
  }
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
  }
  close_pi(&pi);
}

/**
 * @brief A process that is not there cannot be opened
 */
static void test_terminate_unknown_pid(void) {
  struct ov_error err = {0};
  struct process_ref ref;

  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  TEST_FAILED_WITH(
      process_ref_open(&ref, 0xFFFFFFFFu, true, &err), &err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER));
  TEST_SUCCEEDED(process_ref_close(&ref, false, &err), &err);
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
}

/**
 * @brief An instance that was never opened is refused rather than acted on
 */
static void test_null_arguments(void) {
  struct ov_error err = {0};
  struct process_ref ref;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  TEST_SUCCEEDED(process_ref_close(&ref, false, &err), &err); // a ref that was never opened must be tolerated
  TEST_FAILED_WITH(process_ref_open(NULL, 1, false, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(wait_gone(&ref, 10, NULL, &err) != ov_indeterminate, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_FAILED_WITH(process_ref_terminate(&ref, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

TEST_LIST = {
    {"terminate_and_wait", test_terminate_and_wait},
    {"wait_times_out", test_wait_times_out},
    {"stop_ends_the_wait", test_stop_ends_the_wait},
    {"terminate_unknown_pid", test_terminate_unknown_pid},
    {"start_matches_event_time", test_start_matches_event_time},
    {"null_arguments", test_null_arguments},
    {NULL, NULL},
};
