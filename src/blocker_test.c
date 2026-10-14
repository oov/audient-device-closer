/**
 * Tests for the layer that turns veto records into the processes this tool may close.
 *
 * Everything here runs against dummy processes this test owns, never against iD.exe.  Two
 * things are checked: that the image of the running process decides which record is worth
 * acting on at all, and that the moment of the record decides whether the process reached
 * by the id is really the one the record names.
 */
#include <ovtest.h>

#include <windows.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "blocker.h"
#include "process.h"
#include "veto_event.h"

#define DUMMY_NAME "test_dummy.exe"

/** @brief The separator and the quote the paths of the dummies are built with */
#define SEP_CHAR ((wchar_t)0x5C)
#define QUOTE_CHAR ((wchar_t)0x22)

/** @brief The names the settings are stood in for with, so the dummy is a mixer */
static char const *const kDummyNames[] = {DUMMY_NAME};

/**
 * @brief The dummy that ships with this build, next to the test executable
 *
 * @param err receives the failure of the lookup
 * @return the path of the dummy, NULL when it could not be found
 * @note Release with CoTaskMemFree.
 */
static wchar_t *shipped_dummy(struct ov_error *const err) {
  wchar_t *path = NULL;
  wchar_t self[512];
  wchar_t *slash = NULL;
  DWORD len = 0;

  len = GetModuleFileNameW(NULL, self, (DWORD)(sizeof(self) / sizeof(self[0])));
  if ((len == 0) || (len >= (DWORD)(sizeof(self) / sizeof(self[0])))) {
    return NULL;
  }
  self[len] = 0;
  slash = wcsrchr(self, SEP_CHAR);
  if (slash == NULL) {
    return NULL;
  }
  *slash = 0;
  if (!ov_sprintf_wchar(&path, err, NULL, L"%ls%lc%s", self, (int)SEP_CHAR, DUMMY_NAME)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return path;
}

/**
 * @brief Start one of the dummies
 *
 * @param path the dummy to start
 * @param pi receives the handle of the process
 * @return false when the dummy could not be started
 */
static bool spawn(wchar_t const *const path, PROCESS_INFORMATION *const pi) {
  STARTUPINFOW si;
  wchar_t *cmdline = NULL;
  BOOL ok = FALSE;
  bool success = false;

  memset(pi, 0, sizeof(*pi));
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (!ov_sprintf_wchar(&cmdline, NULL, NULL, L"%lc%ls%lc", (int)QUOTE_CHAR, path, (int)QUOTE_CHAR)) {
    goto cleanup;
  }
  ok = CreateProcessW(path, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
  success = ok ? true : false;

cleanup:
  if (cmdline != NULL) {
    OV_ARRAY_DESTROY(&cmdline); // made by ov_sprintf_wchar
    cmdline = NULL;
  }
  return success;
}

/**
 * @brief Give the handles of a dummy back
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
 * @brief Is this pid still running
 *
 * @param pid the process to look at
 * @return true while it is alive
 */
static bool is_running(uint32_t const pid) {
  struct process_ref ref;
  ov_tribool alive = ov_indeterminate;
  struct ov_error err = {0};

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if (process_ref_open(&ref, pid, false, false, &err)) {
    alive = process_ref_alive(&ref, &err);
  }
  OV_ERROR_REPORT(&err, NULL);
  process_ref_close(&ref);
  return alive == ov_true;
}

/**
 * @brief A moment this process is known to be older than
 *
 * The public surface has no way to ask for the creation time alone, so the test asks whether
 * the process started before the moment it reads now: that the process exists at all is what
 * the test needs to build a record of its own around.
 *
 * @param out_ms receives the moment
 * @return true when the moment could be read
 */
static bool start_time_of(uint64_t *const out_ms) {
  *out_ms = process_now_ms();
  return *out_ms > 0;
}

/**
 * @brief A record whose process is the one it names becomes a blocker
 *
 * The id and the moment of the record name this process, so the list carries it.
 */
static void test_record_names_the_running_process(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = start;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&events, c))) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_count(blockers) == 1, "want the one running process, got %zu", blocker_count(blockers));
  TEST_CHECK(blockers[0].pid == (uint32_t)pi.dwProcessId);
  TEST_CHECK(blockers[0].kind == BLOCKER_KIND_MIXER);
  TEST_SUCCEEDED(blocker_close(&blockers[0], false, &err), &err);
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == false);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A process whose image is neither the mixer nor the audio engine is left out
 *
 * The snapshot says what each process is, so a process this tool never closes is dropped
 * before anything is opened.
 */
static void test_a_process_this_tool_never_closes_is_left_out(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  static char const *const other_names[] = {"something_else.exe"};

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = process_now_ms();
  if (!TEST_CHECK(OV_ARRAY_PUSH(&events, c))) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, other_names, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_count(blockers) == 0, "want no blocker, got %zu", blocker_count(blockers));
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A record that is older than the process names somebody else
 *
 * The identity rule that keeps this tool off the wrong process: a process that started after
 * the record was written cannot be the one that blocked the removal, the id was given to it
 * in between.  The lookup keeps it (the image is the right one), and the close refuses it.
 */
static void test_record_older_than_the_process_is_refused(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = (start > 0) ? (start - 1000) : 1;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&events, c))) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  if (!TEST_CHECK(blocker_count(blockers) == 1)) {
    goto cleanup;
  }
  TEST_FAILED_WITH(blocker_close(&blockers[0], false, &err), &err, ov_error_type_generic, ov_error_generic_not_found);
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A record whose process is already gone names a blocker that is gone too
 *
 * That is an answer, not a failure: the device was held by somebody who is no longer there.
 */
static void test_record_of_a_gone_process_is_no_blocker(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = start;
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  close_pi(&pi);
  if (!TEST_CHECK(OV_ARRAY_PUSH(&events, c))) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_count(blockers) == 0, "want no blocker, got %zu", blocker_count(blockers));

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief The list is empty when no record names a process that is there
 */
static void test_no_records_means_no_blockers(void) {
  struct ov_error err = {0};
  struct veto_event *events = NULL;
  struct blocker *blockers = NULL;

  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(blocker_count(blockers) == 0);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  blocker_list_destroy(&blockers);
}

/**
 * @brief Closing what was never built must not crash
 */
static void test_close_without_blocker(void) {
  struct ov_error err = {0};
  bool ok = false;

  ok = blocker_close(NULL, false, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  blocker_list_destroy(NULL);
}

TEST_LIST = {
    {"record_names_the_running_process", test_record_names_the_running_process},
    {"a_process_this_tool_never_closes_is_left_out", test_a_process_this_tool_never_closes_is_left_out},
    {"record_older_than_the_process_is_refused", test_record_older_than_the_process_is_refused},
    {"record_of_a_gone_process_is_no_blocker", test_record_of_a_gone_process_is_no_blocker},
    {"no_records_means_no_blockers", test_no_records_means_no_blockers},
    {"close_without_blocker", test_close_without_blocker},
    {NULL, NULL},
};
