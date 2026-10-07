/**
 * The mixer module is the policy layer on top of process.c: which exact process instance is
 * closed, and the checks that keep it off every other one.
 *
 * Everything here runs against dummy processes this test owns, never against iD.exe.  The
 * point of the suite is the identity rule: the pid of the record decides, the name is only
 * a sanity check, and a same-named process somewhere else must stay untouched.
 *
 * The dummies sit in the temporary directory, so the copies cannot be confused with the
 * one next to this test executable.
 */
#include <ovtest.h>

#include <windows.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "mixer.h"
#include "process.h"

#define DUMMY_NAME "test_dummy.exe"
#define DUMMY_COPY_NAME "test_dummy_copy.exe"

/** @brief The separator and the quote the paths of the dummies are built with */
#define SEP_CHAR ((wchar_t)0x5C)
#define QUOTE_CHAR ((wchar_t)0x22)

/**
 * @brief Where the dummy and its copies live
 *
 * Remove the whole directory with the files in it.
 *
 * @param err receives the failure of the build
 * @return the directory, NULL when it could not be made
 * @note Release with CoTaskMemFree.
 */
static wchar_t *make_dir(struct ov_error *const err) {
  wchar_t *dir = NULL;
  wchar_t tmp[512];
  DWORD len = 0;

  len = GetTempPathW((DWORD)(sizeof(tmp) / sizeof(tmp[0])), tmp);
  if ((len == 0) || (len >= (DWORD)(sizeof(tmp) / sizeof(tmp[0])))) {
    return NULL;
  }
  if (!ov_sprintf_wchar(&dir, err, NULL, L"%lsadc-mixer-test-%lu", tmp, (unsigned long)GetCurrentProcessId())) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  if (!CreateDirectoryW(dir, NULL) && (GetLastError() != ERROR_ALREADY_EXISTS)) {
    OV_ARRAY_DESTROY(&dir); // made by ov_sprintf_wchar
    return NULL;
  }
  return dir;
}

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
 * @brief A second copy of the same image under the same name, in another directory
 *
 * Exactly what the tool must never close by accident.
 *
 * @param dir the directory to put the copy in
 * @param src the dummy to copy
 * @param name the name the copy is given
 * @param err receives the failure of the copy
 * @return the path of the copy, NULL when it could not be made
 * @note Release with CoTaskMemFree.
 */
static wchar_t *copy_dummy(wchar_t const *const dir, wchar_t const *const src, char const *const name, struct ov_error *const err) {
  wchar_t *dst = NULL;

  if (!ov_sprintf_wchar(&dst, err, NULL, L"%ls%lc%s", dir, (int)SEP_CHAR, name)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  if (!CopyFileW(src, dst, FALSE)) {
    OV_ERROR_DESTROY(err);
    OV_ARRAY_DESTROY(&dst); // made by ov_sprintf_wchar
    return NULL;
  }
  return dst;
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
  struct ov_error err = {0};
  struct process_ref ref;
  bool alive = false;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if (process_ref_open(&ref, pid, false, &err)) {
    alive = process_ref_alive(&ref);
  }
  OV_ERROR_REPORT(&err, NULL);
  process_ref_close(&ref);
  return alive;
}

/**
 * @brief The event time to hand in for a process this test started
 *
 * Its own start time, which makes it the instance the record names.
 *
 * @param pi the process the record is about
 * @param out_ms receives the moment the record carries
 * @param err receives the failure of the lookup
 * @return false when the start time could not be read
 */
static bool event_time_of(PROCESS_INFORMATION const *const pi, long long *const out_ms, struct ov_error *const err) {
  struct process_ref ref;
  bool ok = false;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if (!process_ref_open(&ref, (uint32_t)pi->dwProcessId, false, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  *out_ms = ref.start_ms_utc;
  ok = (*out_ms > 0);
  process_ref_close(&ref);
  return ok;
}

/**
 * @brief The happy path: the instance the record names is taken and closed
 *
 * The pid and the time of the record name this process, so it is taken and closed, and the
 * snapshot keeps the path for a later restart.
 */
static void test_take_and_close(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct mixer_target target;
  bool found = false;
  bool gone = false;
  long long event_time = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&target, 0, sizeof(target));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }

  if (!TEST_CHECK(event_time_of(&pi, &event_time, &err) == true)) {
    goto cleanup;
  }
  if (!TEST_CHECK(mixer_take((uint32_t)pi.dwProcessId, DUMMY_NAME, event_time, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == true);
  TEST_CHECK(target.pid == (uint32_t)pi.dwProcessId);
  TEST_CHECK(target.path != NULL && (strstr(target.path, DUMMY_NAME) != NULL));

  if (!TEST_CHECK(mixer_close(&target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(target.pid == 0);
  TEST_CHECK(process_wait((uint32_t)pi.dwProcessId, 5000, &gone, &err) == true);
  TEST_CHECK(gone == true);

  if (pi.hProcess != NULL) {
    WaitForSingleObject(pi.hProcess, 5000);
  }

  TEST_CHECK(target.path != NULL && (strstr(target.path, DUMMY_NAME) != NULL));

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  mixer_target_release(&target);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path); // made by ov_sprintf_wchar
    path = NULL;
  }
}

/**
 * @brief A process with the same image name in another directory is never touched
 *
 * The one that matters: its pid was never in the record, so nothing may happen to it, and
 * a record naming the copy must not reach it either.
 */
static void test_same_name_elsewhere_is_not_touched(void) {
  struct ov_error err = {0};
  wchar_t *dir = NULL;
  wchar_t *shipped = NULL;
  wchar_t *copy = NULL;
  PROCESS_INFORMATION victim;
  PROCESS_INFORMATION other;
  struct mixer_target target;
  bool found = false;
  long long event_time = 0;

  memset(&victim, 0, sizeof(victim));
  memset(&other, 0, sizeof(other));
  memset(&target, 0, sizeof(target));
  shipped = shipped_dummy(&err);
  dir = make_dir(&err);
  if ((shipped == NULL) || (dir == NULL) || ((copy = copy_dummy(dir, shipped, DUMMY_NAME, &err)) == NULL)) {
    TEST_SKIP("cannot prepare the copies here");
    goto cleanup;
  }
  if (!spawn(shipped, &victim) || !spawn(copy, &other)) {
    TEST_SKIP("cannot start the dummies here");
    goto cleanup;
  }
  TEST_CASE("a pid from another record does not reach the copy");
  if (!TEST_CHECK(event_time_of(&victim, &event_time, &err) == true)) {
    goto cleanup;
  }
  if (!TEST_CHECK(mixer_take((uint32_t)victim.dwProcessId, DUMMY_NAME, event_time, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == true);
  TEST_CHECK(target.pid == (uint32_t)victim.dwProcessId);
  TEST_CHECK(target.pid != (uint32_t)other.dwProcessId);
  TEST_CHECK(target.path != NULL && (strlen(target.path) > 0));

  TEST_CASE("a record that is older than the process is refused");
  found = false;
  mixer_target_release(&target);
  if (!TEST_CHECK(mixer_take((uint32_t)other.dwProcessId, DUMMY_NAME, event_time, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == false);
  TEST_CHECK(target.pid == 0);
  TEST_CHECK(is_running((uint32_t)other.dwProcessId) == true);
  TEST_CASE_(NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  mixer_target_release(&target);
  if (victim.hProcess != NULL) {
    TerminateProcess(victim.hProcess, 0);
    WaitForSingleObject(victim.hProcess, 5000);
    close_pi(&victim);
  }
  if (other.hProcess != NULL) {
    TerminateProcess(other.hProcess, 0);
    WaitForSingleObject(other.hProcess, 5000);
    close_pi(&other);
  }
  if (copy != NULL) {
    DeleteFileW(copy);
    OV_ARRAY_DESTROY(&copy);
  }
  if (dir != NULL) {
    RemoveDirectoryW(dir);
    OV_ARRAY_DESTROY(&dir);
  }
  if (shipped != NULL) {
    OV_ARRAY_DESTROY(&shipped);
  }
}

/**
 * @brief The name is the sanity check on top of the pid
 *
 * A live pid whose image is something else is not the process the record was about.
 */
static void test_wrong_name_is_refused(void) {
  struct ov_error err = {0};
  wchar_t *dir = NULL;
  wchar_t *shipped = NULL;
  wchar_t *copy = NULL;
  PROCESS_INFORMATION pi;
  struct mixer_target target;
  bool found = false;
  long long event_time = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&target, 0, sizeof(target));
  shipped = shipped_dummy(&err);
  dir = make_dir(&err);
  if ((shipped == NULL) || (dir == NULL) || ((copy = copy_dummy(dir, shipped, DUMMY_COPY_NAME, &err)) == NULL)) {
    TEST_SKIP("cannot prepare the copies here");
    goto cleanup;
  }
  if (!spawn(copy, &pi)) {
    TEST_SKIP("cannot start the dummy here");
    goto cleanup;
  }
  if (!TEST_CHECK(event_time_of(&pi, &event_time, &err) == true)) {
    goto cleanup;
  }
  if (!TEST_CHECK(mixer_take((uint32_t)pi.dwProcessId, DUMMY_NAME, event_time, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == false);
  TEST_CHECK(target.pid == 0);
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  mixer_target_release(&target);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (copy != NULL) {
    DeleteFileW(copy);
    OV_ARRAY_DESTROY(&copy);
  }
  if (dir != NULL) {
    RemoveDirectoryW(dir);
    OV_ARRAY_DESTROY(&dir);
  }
  if (shipped != NULL) {
    OV_ARRAY_DESTROY(&shipped);
  }
}

/**
 * @brief An unknown event time is not a licence to close anything
 *
 * The caller gets "not found", and the process stays.
 */
static void test_unknown_event_time_is_refused(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct mixer_target target;
  bool found = false;

  memset(&pi, 0, sizeof(pi));
  memset(&target, 0, sizeof(target));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(mixer_take((uint32_t)pi.dwProcessId, DUMMY_NAME, 0, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == false);
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  mixer_target_release(&target);
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
 * @brief A pid that is not running any more is not a blocker any more
 *
 * That is an answer, not a failure.
 */
static void test_gone_process_is_not_found(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct mixer_target target;
  bool found = false;
  long long event_time = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&target, 0, sizeof(target));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(event_time_of(&pi, &event_time, &err) == true)) {
    goto cleanup;
  }
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  close_pi(&pi);

  if (!TEST_CHECK(mixer_take((uint32_t)pi.dwProcessId, DUMMY_NAME, event_time, &found, &target, &err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(found == false);
  TEST_CHECK(target.pid == 0);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  mixer_target_release(&target);
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
 * @brief Closing what was never taken must not crash
 */
static void test_close_without_target(void) {
  struct ov_error err = {0};
  struct mixer_target target;
  bool found = false;
  bool ok = false;

  memset(&target, 0, sizeof(target));
  ok = mixer_close(NULL, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  ok = mixer_close(&target, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  ok = mixer_take(1, NULL, 1, &found, &target, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  ok = mixer_take(0, DUMMY_NAME, 1, &found, &target, &err);
  TEST_FAILED_WITH(ok, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  mixer_target_release(&target);
  mixer_target_release(NULL);
}

TEST_LIST = {
    {"take_and_close", test_take_and_close},
    {"same_name_elsewhere_is_not_touched", test_same_name_elsewhere_is_not_touched},
    {"wrong_name_is_refused", test_wrong_name_is_refused},
    {"unknown_event_time_is_refused", test_unknown_event_time_is_refused},
    {"gone_process_is_not_found", test_gone_process_is_not_found},
    {"close_without_target", test_close_without_target},
    {NULL, NULL},
};
