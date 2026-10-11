#include "process.h"

#include <windows.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "logger.h"

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#  define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/**
 * @brief Does an image path end with the given name
 *
 * @param path the path to look at
 * @param name the image name to look for
 * @return true when the last component of path is name
 */
bool process_path_matches_name(char const *const path, char const *const name) {
  size_t plen = 0;
  size_t nlen = 0;
  size_t i = 0;

  if ((path == NULL) || (name == NULL) || (name[0] == '\0')) {
    return false;
  }
  plen = strlen(path);
  nlen = strlen(name);
  if (plen < nlen) {
    return false;
  }
  i = plen - nlen;
  if (i > 0) {
    char const prev = path[i - 1];
    if ((prev != (char)0x5C) && (prev != '/') && (prev != ':')) {
      return false;
    }
  }
  return (_strnicmp(&path[i], name, nlen) == 0);
}

/**
 * @brief Did the process start before the event it is measured against
 *
 * A process that started after the veto cannot be the one that blocked the removal: the
 * pid was reused in between.
 *
 * @param start_ms_utc when the process started, epoch milliseconds UTC
 * @param event_time_ms when the event was written, epoch milliseconds UTC
 * @return false when one of the two is unknown or the process is younger than the event
 */
bool process_start_matches_event_time(long long const start_ms_utc, long long const event_time_ms) {
  if ((start_ms_utc <= 0) || (event_time_ms <= 0)) {
    return false; // without both times there is nothing to compare
  }
  return start_ms_utc <= event_time_ms;
}

/**
 * @brief FILETIME (100ns since 1601) to epoch milliseconds UTC
 *
 * @param ft the time to convert
 * @return the moment in epoch milliseconds UTC
 */
static long long filetime_to_ms(FILETIME const *const ft) {
  ULARGE_INTEGER u;

  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  if (u.QuadPart == 0) {
    return 0;
  }
  return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

/**
 * @brief The moment now, in epoch milliseconds UTC
 *
 * @return the moment in epoch milliseconds UTC
 */
long long process_now_ms(void) {
  FILETIME ft;

  GetSystemTimeAsFileTime(&ft);
  return filetime_to_ms(&ft);
}

/**
 * @brief Turn SeDebugPrivilege on or off for this process
 *
 * Enables or disables SeDebugPrivilege on our own token.  The privilege is present in every
 * administrative token but stays off until it is asked for; OpenProcess only honours it
 * while it is on.  out_was_on reports whether it was already on, so the caller can put the
 * token back the way it was.  A token without the privilege is not an error: the caller
 * reports whatever the operation itself answered.
 *
 * @param enable the state the token should have
 * @param out_was_on receives whether the token had it before
 * @param err receives the failure of the adjustment
 * @return false when the token could not be adjusted
 */
static bool set_debug_privilege(bool const enable, bool *const out_was_on, struct ov_error *const err) {
  bool success = false;
  HANDLE token = NULL;
  TOKEN_PRIVILEGES tp;
  TOKEN_PRIVILEGES previous;
  DWORD previous_len = sizeof(previous);
  LUID luid;

  *out_was_on = false;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memset(&tp, 0, sizeof(tp));
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
  SetLastError(ERROR_SUCCESS);
  if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(previous), &previous, &previous_len)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
    success = true;
    goto cleanup;
  }
  if ((previous_len >= sizeof(TOKEN_PRIVILEGES)) && (previous.PrivilegeCount > 0)) {
    *out_was_on = (previous.Privileges[0].Attributes & SE_PRIVILEGE_ENABLED) != 0;
  }
  success = true;

cleanup:
  if (token != NULL) {
    CloseHandle(token);
  }
  return success;
}

/**
 * @brief Open a process, with SeDebugPrivilege when a plain open is refused
 *
 * Opens the process, and when the refusal is a plain access denial, tries the same open
 * once more with SeDebugPrivilege enabled.
 *
 * @param pid the process to open
 * @param access what OpenProcess should be asked for
 * @param err receives the reason of the last refusal
 * @return the handle, NULL when the process could not be opened
 */
static HANDLE open_with_debug_privilege(uint32_t const pid, DWORD const access, struct ov_error *const err) {
  HANDLE h = NULL;
  bool was_on = false;
  bool changed = false;
  DWORD last_error = ERROR_SUCCESS;

  h = OpenProcess(access, FALSE, pid);
  if (h != NULL) {
    return h;
  }
  last_error = GetLastError();
  if (last_error != ERROR_ACCESS_DENIED) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last_error));
    return NULL;
  }
  if (!set_debug_privilege(true, &was_on, err)) {
    // the privilege is what the retry needs, so its failure is the answer of the open
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  changed = !was_on;
  h = OpenProcess(access, FALSE, pid);
  if (h == NULL) {
    last_error = GetLastError();
  }
  if (changed) {
    bool ignored = false;
    struct ov_error restore_err = {0};
    // the token is out of the picture either way: failing to put it back is worth a
    // report and nothing more
    if (!set_debug_privilege(false, &ignored, &restore_err)) {
      OV_ERROR_REPORT(&restore_err, NULL);
    }
  }
  if (h == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last_error));
  }
  return h;
}

/**
 * @brief Open one exact process instance
 *
 * The handle is opened once and every check runs through it, so the instance that is acted
 * on is the one that was checked.
 *
 * @param out receives the instance, release with process_ref_close
 * @param pid the process the record named
 * @param need_terminate ask for the right to terminate the process
 * @param err receives the refusal of the system
 * @return false when the process could not be opened
 */
bool process_ref_open(struct process_ref *const out, uint32_t const pid, bool const need_terminate, struct ov_error *const err) {
  bool success = false;
  DWORD const access = (need_terminate ? PROCESS_TERMINATE : 0) | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  HANDLE h = NULL;
  FILETIME creation;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;

  if (out == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  memset(out, 0, sizeof(*out));
  out->handle = INVALID_HANDLE_VALUE;
  out->pid = pid;

  h = open_with_debug_privilege(pid, access, err);
  if (h == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(&creation, 0, sizeof(creation));
  memset(&exit, 0, sizeof(exit));
  memset(&kernel, 0, sizeof(kernel));
  memset(&user, 0, sizeof(user));
  if (!GetProcessTimes(h, &creation, &exit, &kernel, &user)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  out->start_ms_utc = filetime_to_ms(&creation);
  out->handle = h;
  h = NULL;
  success = true;

cleanup:
  if (h != NULL) {
    CloseHandle(h);
  }
  return success;
}

/**
 * @brief Is the instance still running
 *
 * @param ref the instance to look at
 * @return true while the process is alive
 */
bool process_ref_alive(struct process_ref const *const ref) {
  DWORD code = 0;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    return false;
  }
  if (!GetExitCodeProcess((HANDLE)ref->handle, &code)) {
    return false; // an unreadable state is not evidence that it is still there
  }
  return code == STILL_ACTIVE;
}

/**
 * @brief The win32 path of the image this instance runs
 *
 * @param ref the instance to look at
 * @param err receives the refusal of the system
 * @return the path in UTF-8, NULL when it could not be read
 * @note Release with process_string_free.
 */
char *process_ref_image_path(struct process_ref const *const ref, struct ov_error *const err) {
  char *result = NULL;
  wchar_t buf[512];
  DWORD chars = (DWORD)(sizeof(buf) / sizeof(buf[0]));
  bool success = false;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!QueryFullProcessImageNameW((HANDLE)ref->handle, 0, buf, &chars) || (chars == 0)) {
    DWORD const le = (GetLastError() != 0) ? GetLastError() : ERROR_NOT_FOUND;
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
    goto cleanup;
  }
  if (!ov_sprintf_char(&result, err, NULL, "%ls", buf)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    if (result != NULL) {
      OV_ARRAY_DESTROY(&result);
    }
  }
  return success ? result : NULL;
}

/**
 * @brief Terminate this instance
 *
 * @param ref the instance to close
 * @param err receives the refusal of the system
 * @return false when the process could not be terminated
 */
bool process_ref_terminate(struct process_ref const *const ref, struct ov_error *const err) {
  bool success = false;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!TerminateProcess((HANDLE)ref->handle, 0)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Wait until this instance is gone
 *
 * @param ref the instance to wait for
 * @param timeout_ms how long to wait
 * @param out_gone receives whether the process is gone
 * @param err receives the failure of the wait
 * @return false when the wait could not be answered
 */
bool process_ref_wait(struct process_ref const *const ref, uint32_t const timeout_ms, bool *const out_gone, struct ov_error *const err) {
  bool success = false;
  DWORD wait = WAIT_FAILED;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE) || (out_gone == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_gone = false;
  if (!process_ref_alive(ref)) {
    *out_gone = true;
    success = true;
    goto cleanup;
  }
  wait = WaitForSingleObject((HANDLE)ref->handle, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    *out_gone = true;
    success = true;
    goto cleanup;
  }
  if (wait == WAIT_TIMEOUT) {
    success = true;
    goto cleanup;
  }
  OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));

cleanup:
  return success;
}

/**
 * @brief Give the handle back
 *
 * @param ref the instance to close, NULL is allowed
 */
void process_ref_close(struct process_ref *const ref) {
  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    return;
  }
  CloseHandle((HANDLE)ref->handle);
  ref->handle = INVALID_HANDLE_VALUE;
  ref->pid = 0;
  ref->start_ms_utc = 0;
}

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void process_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_ARRAY_DESTROY(value); // made by ov_sprintf_char
  }
}
