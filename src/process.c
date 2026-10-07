#include "process.h"

#include <windows.h>

#include <tlhelp32.h>
#include <winternl.h>

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
 * @brief Open a process for the right this caller needs
 *
 * @param pid the process to open
 * @param access what OpenProcess should be asked for
 * @param out_handle receives the handle
 * @param err receives the refusal of the system
 * @return false when the process could not be opened
 */
static bool process_open(uint32_t const pid, DWORD const access, HANDLE *const out_handle, struct ov_error *const err) {
  HANDLE h = OpenProcess(access, FALSE, pid);
  if (h == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  *out_handle = h;
  return true;
}

/**
 * @brief The win32 path of the image a process runs
 *
 * QueryFullProcessImageNameW does not accept a NULL buffer to ask for the size,
 * so the name is read into a stack buffer first and copied out only if it fits.
 *
 * @param pid the process to ask
 * @param out_path receives the path, release with process_string_free
 * @param err receives the refusal of the system
 * @return false when the path could not be read
 */
static bool path_of(uint32_t const pid, char **const out_path, struct ov_error *const err) {
  bool success = false;
  HANDLE h = NULL;
  wchar_t buf[512];
  DWORD chars = (DWORD)sizeof(buf) / sizeof(buf[0]);

  if (!process_open(pid, PROCESS_QUERY_LIMITED_INFORMATION, &h, err)) {
    goto cleanup;
  }
  if (!QueryFullProcessImageNameW(h, 0, buf, &chars) || (chars == 0)) {
    DWORD const le = (GetLastError() != 0) ? GetLastError() : ERROR_NOT_FOUND;
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
    goto cleanup;
  }
  if (!ov_sprintf_char(out_path, err, NULL, "%ls", buf)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (h) {
    CloseHandle(h);
  }
  return success;
}

/**
 * @brief Read a Unicode string that lives inside another process
 *
 * NtQueryInformationProcess is undocumented for user mode, but winternl.h declares it.
 * It is the only way to reach the PEB of another process.
 *
 * @param process the process to read from
 * @param src the string to read, inside that process
 * @param out receives the text in UTF-8, release with process_string_free
 * @param err receives the refusal of the system
 * @return false when the string could not be read
 */
static bool read_unicode_string(HANDLE const process, UNICODE_STRING const *const src, char **const out, struct ov_error *const err) {
  bool success = false;
  wchar_t *buf = NULL;
  size_t chars = 0;
  SIZE_T read = 0;

  *out = NULL;
  if ((src->Buffer == NULL) || (src->Length == 0)) {
    success = true;
    goto cleanup;
  }
  chars = (size_t)src->Length / 2u;
  if (!OV_REALLOC(&buf, chars + 1, sizeof(buf[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!ReadProcessMemory(process, src->Buffer, buf, chars * sizeof(buf[0]), &read)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (read < chars * sizeof(buf[0])) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  buf[chars] = L'\0';
  if (!ov_sprintf_char(out, err, NULL, "%ls", buf)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  OV_FREE(&buf);
  return success;
}

/**
 * @brief The command line a process was started with
 *
 * @param pid the process to ask
 * @param err receives the refusal of the system
 * @return the command line in UTF-8, NULL when it could not be read
 * @note Release with process_string_free.
 */
char *process_command_line(uint32_t const pid, struct ov_error *const err) {
  char *result = NULL;
  HANDLE h = NULL;
  PROCESS_BASIC_INFORMATION pbi;
  PEB peb;
  RTL_USER_PROCESS_PARAMETERS params;
  SIZE_T read = 0;
  NTSTATUS st = 0;

  memset(&pbi, 0, sizeof(pbi));
  memset(&peb, 0, sizeof(peb));
  memset(&params, 0, sizeof(params));

  if (!process_open(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, &h, err)) {
    goto cleanup;
  }
  st = NtQueryInformationProcess(h, ProcessBasicInformation, &pbi, (ULONG)sizeof(pbi), NULL);
  if ((st != 0) || (pbi.PebBaseAddress == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof(peb), &read) || (read < sizeof(peb))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (peb.ProcessParameters == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  if (!ReadProcessMemory(h, peb.ProcessParameters, &params, sizeof(params), &read) || (read < sizeof(params))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!read_unicode_string(h, &params.CommandLine, &result, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

cleanup:
  if (h) {
    CloseHandle(h);
  }
  return result;
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

/**
 * @brief Append one snapshot entry when it passes the filter
 *
 * The path is best effort: protected processes answer with ACCESS_DENIED and still count
 * as running.
 *
 * @param list the list to append to
 * @param entry the entry of the snapshot
 * @param filter the image name to keep, NULL keeps every one
 * @param err receives a failure that is worth stopping the walk for
 * @return false on such a failure
 */
static bool process_list_append(struct process_entry **const list,
                                PROCESSENTRY32W const *const entry,
                                wchar_t const *const filter,
                                struct ov_error *const err) {
  struct process_entry item;
  struct ov_error path_err = {0};
  bool path_failed = false;
  bool success = false;

  if ((filter != NULL) && (_wcsicmp(entry->szExeFile, filter) != 0)) {
    return true;
  }
  memset(&item, 0, sizeof(item));
  item.pid = (uint32_t)entry->th32ProcessID;
  {
    char *path = NULL;
    if (path_of(item.pid, &path, &path_err)) {
      item.path = path;
    } else {
      path_failed = true;
    }
  }
  if (!OV_ARRAY_PUSH(list, item)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (path_failed) {
    OV_ERROR_DESTROY(&path_err);
  }
  if (!success) {
    OV_ARRAY_DESTROY(&item.path); // made by ov_sprintf_char
    item.path = NULL;
  }
  return success;
}

/**
 * @brief The processes that are running right now
 *
 * @param exe_name only entries of this image name, NULL keeps every one
 * @param err receives the failure of the walk
 * @return the list, NULL when the walk failed
 * @note Release with process_list_destroy.
 */
struct process_entry *process_list(char const *const exe_name, struct ov_error *const err) {
  struct process_entry *list = NULL;
  HANDLE snap = INVALID_HANDLE_VALUE;
  PROCESSENTRY32W entry;
  BOOL ok = FALSE;
  wchar_t filter_buf[MAX_PATH] = {0}; // the image names the snapshot reports fit in this
  wchar_t const *filter = NULL;
  bool success = false;

  if (!OV_ARRAY_GROW(&list, 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  OV_ARRAY_SET_LENGTH(list, 0);

  snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memset(&entry, 0, sizeof(entry));
  entry.dwSize = sizeof(entry);
  if (exe_name != NULL) {
    int const n = OV_SNPRINTF(filter_buf, sizeof(filter_buf) / sizeof(filter_buf[0]), NULL, L"%s", exe_name);
    if ((n < 0) || ((size_t)n >= (sizeof(filter_buf) / sizeof(filter_buf[0])))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      goto cleanup;
    }
    filter = filter_buf;
  }
  success = true;
  for (ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
    if (!process_list_append(&list, &entry, filter, err)) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
      break;
    }
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
  }

cleanup:
  if (snap != INVALID_HANDLE_VALUE) {
    CloseHandle(snap);
  }
  if (!success) {
    process_list_destroy(&list);
    return NULL;
  }
  return list;
}

/**
 * @brief Number of entries in the list
 *
 * @param list NULL is allowed
 * @return the number of entries, 0 for NULL
 */
size_t process_count(struct process_entry const *const list) {
  if (list == NULL) {
    return 0;
  }
  return OV_ARRAY_LENGTH(list);
}

/**
 * @brief Release the list and every entry in it
 *
 * @param list set to NULL afterwards
 */
void process_list_destroy(struct process_entry **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(*list);
  for (size_t i = 0; i < n; i++) {
    if ((*list)[i].path != NULL) {
      OV_ARRAY_DESTROY(&(*list)[i].path);
    }
  }
  OV_ARRAY_DESTROY(list);
}

/**
 * @brief Wait until a process is gone
 *
 * @param pid the process to wait for
 * @param timeout_ms how long to wait
 * @param out_gone receives whether the process is gone
 * @param err receives the failure of the wait
 * @return false when the wait could not be answered
 */
bool process_wait(uint32_t const pid, uint32_t const timeout_ms, bool *const out_gone, struct ov_error *const err) {
  bool success = false;
  bool gone_without_open = false;
  HANDLE h = NULL;
  DWORD wait = WAIT_FAILED;

  if (out_gone == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_gone = false;
  if (!process_open(pid, SYNCHRONIZE, &h, err)) {
    if (ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
        ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER))) {
      gone_without_open = true;
      *out_gone = true;
      success = true;
    }
    goto cleanup;
  }
  wait = WaitForSingleObject(h, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    *out_gone = true;
    success = true;
  } else if (wait == WAIT_TIMEOUT) {
    success = true;
  } else {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

cleanup:
  if (gone_without_open) {
    OV_ERROR_DESTROY(err);
  }
  if (h) {
    CloseHandle(h);
  }
  return success;
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
 * once more with SeDebugPrivilege enabled.  out_error carries the reason of the last try.
 *
 * @param pid the process to open
 * @param access what OpenProcess should be asked for
 * @param out_error receives the reason of the last refusal
 * @return the handle, INVALID_HANDLE_VALUE when the process could not be opened
 */
static HANDLE open_with_debug_privilege(uint32_t const pid, DWORD const access, DWORD *const out_error) {
  HANDLE h = NULL;
  struct ov_error err = {0};
  bool was_on = false;
  bool changed = false;

  h = OpenProcess(access, FALSE, pid);
  if (h != NULL) {
    return h;
  }
  *out_error = GetLastError();
  if (*out_error != ERROR_ACCESS_DENIED) {
    return NULL;
  }
  if (!set_debug_privilege(true, &was_on, &err)) {
    OV_ERROR_REPORT(&err, NULL);
    return NULL;
  }
  changed = !was_on;
  h = OpenProcess(access, FALSE, pid);
  if (h == NULL) {
    *out_error = GetLastError();
  }
  if (changed) {
    bool ignored = false;
    struct ov_error restore_err = {0};
    if (!set_debug_privilege(false, &ignored, &restore_err)) {
      OV_ERROR_REPORT(&restore_err, NULL);
    }
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
  DWORD first_error = ERROR_SUCCESS;
  FILETIME creation;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;

  if (out == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->handle = INVALID_HANDLE_VALUE;
  out->pid = pid;

  h = open_with_debug_privilege(pid, access, &first_error);
  if (h == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(first_error));
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

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!QueryFullProcessImageNameW((HANDLE)ref->handle, 0, buf, &chars) || (chars == 0)) {
    DWORD const le = (GetLastError() != 0) ? GetLastError() : ERROR_NOT_FOUND;
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
    return NULL;
  }
  if (!ov_sprintf_char(&result, err, NULL, "%ls", buf)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return result;
}

/**
 * @brief Terminate this instance
 *
 * @param ref the instance to close
 * @param err receives the refusal of the system
 * @return false when the process could not be terminated
 */
bool process_ref_terminate(struct process_ref const *const ref, struct ov_error *const err) {
  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!TerminateProcess((HANDLE)ref->handle, 0)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
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
  DWORD wait = WAIT_FAILED;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE) || (out_gone == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *out_gone = false;
  if (!process_ref_alive(ref)) {
    *out_gone = true;
    return true;
  }
  wait = WaitForSingleObject((HANDLE)ref->handle, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    *out_gone = true;
    return true;
  }
  if (wait == WAIT_TIMEOUT) {
    return true;
  }
  OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
  return false;
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
