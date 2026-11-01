#include "process_manager.h"

#include <windows.h>

#include <tlhelp32.h>
#include <winsvc.h>

#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "logger.h"

/** @brief How long a closed process is given to leave after it was asked to */
enum { blocker_wait_max_ms = 5000 };

/** @brief The name Windows gives the service that runs the audio engine */
#define AUDIO_SERVICE_NAME L"Audiosrv"

/**
 * @brief How long the wait for a service state lasts, and how it polls
 *
 * A control that is sent comes back as accepted long before the service has done
 * anything, so the caller waits for the state to settle.  The service says how long it
 * expects to need; that hint is followed, with a bound on it so a service that never
 * moves on cannot hold the wait forever.
 */
enum {
  audio_service_wait_max_ms = 30000,
  audio_service_hint_max_ms = 1000,
  audio_service_hint_default_ms = 250,
};

/**
 * @brief Wait until a service reports the state that was asked for
 *
 * A control that is sent comes back as accepted long before the service has done anything,
 * so the caller has to wait for the state to settle.  The wait is bounded, so a service
 * that never answers is a failure the caller can report rather than a hang.
 *
 * @param service the service to watch
 * @param wanted the state to wait for
 * @param stop the event that is up once the caller asked the act to stop
 * @param err receives the failure
 * @return false when the state was not reached in time
 */
static NODISCARD bool wait_for_state(SC_HANDLE const service, DWORD const wanted, HANDLE const stop, struct ov_error *const err) {
  SERVICE_STATUS_PROCESS status;
  DWORD got = 0;
  DWORD const deadline = GetTickCount() + audio_service_wait_max_ms;
  bool success = false;

  for (;;) {
    DWORD waited = 0;

    memset(&status, 0, sizeof(status));
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &got)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (status.dwCurrentState == wanted) {
      success = true;
      goto cleanup;
    }
    if (GetTickCount() >= deadline) {
      OV_ERROR_SETF(err,
                    ov_error_type_generic,
                    ov_error_generic_fail,
                    "the audio service did not reach state %1$lu in time",
                    "the audio service did not reach state %1$lu in time",
                    (unsigned long)wanted);
      goto cleanup;
    }
    /* the service reports how long it expects to need, in milliseconds */
    waited = (status.dwWaitHint > 0) ? status.dwWaitHint : audio_service_hint_default_ms;
    if (waited > audio_service_hint_max_ms) {
      waited = audio_service_hint_max_ms;
    }
    {
      DWORD const answer = WaitForSingleObject(stop, waited);

      if (answer == WAIT_OBJECT_0) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_abort);
        goto cleanup;
      }
      if (answer == WAIT_FAILED) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      }
    }
  }

cleanup:
  return success;
}

/**
 * @brief Send one control and wait for the state it leads to
 *
 * @param service the service to control
 * @param control the control to send
 * @param wanted the state to wait for
 * @param err receives the failure
 * @return false when the control was refused or the state was not reached
 */
static NODISCARD bool
control_and_wait(SC_HANDLE const service, DWORD const control, DWORD const wanted, HANDLE const stop, struct ov_error *const err) {
  SERVICE_STATUS status;
  bool success = false;

  memset(&status, 0, sizeof(status));
  if (!ControlService(service, control, &status)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = wait_for_state(service, wanted, stop, err);
  if (!success) {
    OV_ERROR_ADD_TRACE(err);
  }

cleanup:
  return success;
}

static NODISCARD bool pause_service(HANDLE const stop, struct ov_error *const err) {
  bool success = false;
  SC_HANDLE scm = NULL;
  SC_HANDLE service = NULL;
  SERVICE_STATUS_PROCESS status;
  DWORD got = 0;

  scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  service = OpenServiceW(scm, AUDIO_SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (service == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memset(&status, 0, sizeof(status));
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &got)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (status.dwCurrentState == SERVICE_STOPPED) {
    success = true; // nothing to do, and the caller has to start it again either way
    goto cleanup;
  }
  if (!control_and_wait(service, SERVICE_CONTROL_STOP, SERVICE_STOPPED, stop, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (service != NULL) {
    if (!CloseServiceHandle(service)) {
      if (success) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      success = false;
    }
  }
  if (scm != NULL) {
    if (!CloseServiceHandle(scm)) {
      if (success) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      success = false;
    }
  }
  return success;
}

static NODISCARD bool resume_service(HANDLE const stop, struct ov_error *const err) {
  bool success = false;
  SC_HANDLE scm = NULL;
  SC_HANDLE service = NULL;
  SERVICE_STATUS_PROCESS status;
  DWORD got = 0;

  scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  service = OpenServiceW(scm, AUDIO_SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
  if (service == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memset(&status, 0, sizeof(status));
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &got)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (status.dwCurrentState == SERVICE_RUNNING) {
    success = true;
    goto cleanup;
  }
  // a service on its way down cannot take a start yet, so its state settles first: starting
  // it now is refused with a reason the caller cannot do anything about
  if ((status.dwCurrentState == SERVICE_STOP_PENDING) && !wait_for_state(service, SERVICE_STOPPED, stop, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!StartServiceW(service, 0, NULL)) {
    DWORD const le = GetLastError();
    if (le != ERROR_SERVICE_ALREADY_RUNNING) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
      goto cleanup;
    }
  }
  if (!wait_for_state(service, SERVICE_RUNNING, stop, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (service != NULL) {
    if (!CloseServiceHandle(service)) {
      if (success) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      success = false;
    }
  }
  if (scm != NULL) {
    if (!CloseServiceHandle(scm)) {
      if (success) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      success = false;
    }
  }
  return success;
}

/**
 * @brief Take a look at the processes that are running now, about the names the caller holds
 *
 * The machine is read once and let go at once, so every name in the list is answered from
 * that same moment and none of them can cost another read.
 *
 * Only the ids the caller names are looked for, so what the look costs follows what the
 * caller needs and not how much the machine happens to be running.
 *
 * @param names the processes to look for, their image is written in place; may be NULL when
 *              count is 0
 * @param count how many of them there are
 * @param err receives the failure of the look
 * @return how many of the names the machine held a process of, SIZE_MAX when the machine
 *         could not be read, with err set
 */
static NODISCARD size_t take_snapshot(struct process_name *const names, size_t const count, struct ov_error *const err) {
  HANDLE list = INVALID_HANDLE_VALUE;
  PROCESSENTRY32W entry;
  size_t found_count = 0;
  size_t result = SIZE_MAX;

  if (count > 0 && names == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  list = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (list == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memset(&entry, 0, sizeof(entry));
  entry.dwSize = sizeof(entry);
  for (size_t i = 0; i < count; i++) {
    bool found = false;

    if (!Process32FirstW(list, &entry)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    do {
      size_t const name_len = wcslen(entry.szExeFile);

      if (names[i].pid != (uint32_t)entry.th32ProcessID) {
        continue;
      }
      if ((name_len == 0) || (name_len >= PROCESS_IMAGE_MAX)) {
        break; // the machine holds it, but under a name this cannot carry
      }
      memcpy(names[i].image, entry.szExeFile, (name_len + 1) * sizeof(names[i].image[0]));
      found = true;
      break;
    } while (Process32NextW(list, &entry));
    if (!found) {
      // the walk is over at the end of the list; any other answer is a read that failed
      DWORD const last_error = GetLastError();
      if (last_error != ERROR_NO_MORE_FILES) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last_error));
        goto cleanup;
      }
      names[i].image[0] = L'\0'; // the moment held no process of that id
    } else {
      found_count++;
    }
  }
  result = found_count;

cleanup:
  if (list != INVALID_HANDLE_VALUE) {
    if (!CloseHandle(list)) {
      if (result != SIZE_MAX) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      result = SIZE_MAX;
    }
  }
  return result;
}

/**
 * @brief A handle on one process instance
 *
 * Windows reuses process ids, so a pid on its own does not identify a process: only a
 * handle stays bound to the instance it was opened on.  Everything below runs through the
 * handle, and while one handle is held the pid cannot move on to another process.
 */
struct process_ref {
  HANDLE handle; //!< the instance, INVALID_HANDLE_VALUE when there is none
};

/**
 * @brief Did the process start at or before the moment it is measured against
 *
 * A process that started after the veto cannot be the one that blocked the removal: the
 * pid was reused in between.
 *
 * @param start_ms_utc when the process started, epoch milliseconds UTC
 * @param event_time_ms when the event was written, epoch milliseconds UTC
 * @return false when one of the two is unknown or the process is younger than the event
 */
static bool process_start_matches_event_time(uint64_t const start_ms_utc, uint64_t const event_time_ms) {
  if ((start_ms_utc == 0) || (event_time_ms == 0)) {
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
static uint64_t filetime_to_ms(FILETIME const *const ft) {
  ULARGE_INTEGER u;

  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  if (u.QuadPart == 0) {
    return 0;
  }
  return (uint64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

/**
 * @brief Open a process
 *
 * @param pid the process to open
 * @param access what OpenProcess should be asked for
 * @param err receives the reason of the refusal
 * @return the handle, NULL when the process could not be opened
 */
static NODISCARD HANDLE open_process(uint32_t const pid, DWORD const access, struct ov_error *const err) {
  HANDLE const h = OpenProcess(access, FALSE, pid);
  if (h == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
  }
  return h;
}

/**
 * @brief Open one exact process instance
 *
 * @param out receives the instance, release with process_ref_close
 * @param pid the process the record named
 * @param need_terminate ask for the right to terminate the process
 * @param err receives the refusal of the system
 * @return false when the process could not be opened
 */
static NODISCARD bool
process_ref_open(struct process_ref *const out, uint32_t const pid, bool const need_terminate, struct ov_error *const err) {
  bool success = false;
  DWORD const access = (need_terminate ? PROCESS_TERMINATE : 0) | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  HANDLE h = NULL;

  if (out == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  memset(out, 0, sizeof(*out));
  out->handle = INVALID_HANDLE_VALUE;

  h = open_process(pid, access, err);
  if (h == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
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
 * @brief Did this instance start before the event it is measured against
 *
 * The creation time is read from the handle itself, so the answer is about the instance the
 * handle is bound to and no other.
 *
 * @param ref the instance to look at
 * @param event_time_ms when the event was written, epoch milliseconds UTC
 * @param err receives the failure of the read
 * @return ov_true when the instance started at or before the event, ov_false when it
 *         started after it, and ov_indeterminate when the time could not be read, with err
 *         set
 */
static NODISCARD ov_tribool process_ref_started_before_event(struct process_ref const *const ref,
                                                             uint64_t const event_time_ms,
                                                             struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  FILETIME creation;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  memset(&creation, 0, sizeof(creation));
  memset(&exit, 0, sizeof(exit));
  memset(&kernel, 0, sizeof(kernel));
  memset(&user, 0, sizeof(user));
  if (!GetProcessTimes(ref->handle, &creation, &exit, &kernel, &user)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  result = process_start_matches_event_time(filetime_to_ms(&creation), event_time_ms) ? ov_true : ov_false;

cleanup:
  return result;
}

/**
 * @brief Terminate this instance
 *
 * It does not touch any other process, even one that has taken over the pid in the
 * meantime.
 *
 * @param ref the instance to close
 * @param err receives the refusal of the system
 * @return false when the process could not be terminated
 */
static NODISCARD bool process_ref_terminate(struct process_ref const *const ref, struct ov_error *const err) {
  bool success = false;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!TerminateProcess(ref->handle, 0)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Give the handle back
 *
 * @param ref the instance to close, NULL is allowed
 * @param failed_before whether the caller already holds a reason it is here
 * @param err receives the failure of the close, NULL is allowed
 * @return false when the handle could not be given back
 */
static NODISCARD bool process_ref_close(struct process_ref *const ref, bool const failed_before, struct ov_error *const err) {
  bool success = true;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE)) {
    return true;
  }
  if (!CloseHandle(ref->handle)) {
    if (failed_before) {
      OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
    } else {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    }
    success = false;
  }
  ref->handle = INVALID_HANDLE_VALUE;
  return success;
}

/**
 * @brief Wait for the process to leave, or for the caller to ask it to stop
 *
 * @param ref the instance to wait for
 * @param timeout_ms how long to wait
 * @param stop the event that is up once the caller asked the act to stop
 * @param err receives the failure of the wait
 * @return ov_true when the process is gone, ov_false when it is still there after the
 *         timeout, ov_indeterminate when it was asked to stop, with err set
 */
static NODISCARD ov_tribool wait_gone(struct process_ref const *const ref,
                                      uint32_t const timeout_ms,
                                      HANDLE const stop,
                                      struct ov_error *const err) {
  HANDLE ends[2] = {0};
  ov_tribool result = ov_indeterminate;
  DWORD waited = WAIT_FAILED;

  if ((ref == NULL) || (ref->handle == NULL) || (ref->handle == INVALID_HANDLE_VALUE) || (stop == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  ends[0] = stop;
  ends[1] = ref->handle;
  waited = WaitForMultipleObjects(2, ends, FALSE, timeout_ms);
  if (waited == (WAIT_OBJECT_0 + 1)) {
    result = ov_true;
  } else if (waited == WAIT_TIMEOUT) {
    result = ov_false;
  } else if (waited == WAIT_OBJECT_0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_abort);
  } else {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
  }

cleanup:
  return result;
}

/**
 * @brief End the process a record names, through the handle on the very instance
 *
 * The handle is what keeps the id on the instance that is checked: from the open to the
 * termination no other process can take the id over.
 *
 * @param pid the process the record named
 * @param event_time_ms when the record was written, epoch milliseconds UTC
 * @param stop the event that is up once the caller asked the act to stop
 * @param err receives the failure; a process the id moved on from is not_found
 * @return false when the process could not be ended
 */
static NODISCARD bool end_process(uint32_t const pid, uint64_t const event_time_ms, HANDLE const stop, struct ov_error *const err) {
  struct process_ref ref;
  ov_tribool mine = ov_indeterminate;
  bool success = false;

  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  if (pid == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!process_ref_open(&ref, pid, true, err)) {
    logger_notef("close", "reject pid=%lu reason=the process could not be opened", (unsigned long)pid);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mine = process_ref_started_before_event(&ref, event_time_ms, err);
  if (mine == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (mine == ov_false) {
    logger_notef("close",
                 "reject pid=%lu event_time=%llu reason=the process started after the record, so the id was given to another one",
                 (unsigned long)pid,
                 (unsigned long long)event_time_ms);
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "the process behind pid %1$lu is no longer the one the record names",
                  "the process behind pid %1$lu is no longer the one the record names",
                  (unsigned long)pid);
    goto cleanup;
  }
  if (!process_ref_terminate(&ref, err)) {
    logger_notef("close", "reject pid=%lu reason=the termination was refused", (unsigned long)pid);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  switch (wait_gone(&ref, blocker_wait_max_ms, stop, err)) {
  case ov_true:
    break;
  case ov_false:
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  (unsigned long)pid);
    goto cleanup;
  case ov_indeterminate:
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  logger_notef("close", "closed pid=%lu", (unsigned long)pid);
  success = true;

cleanup:
  if (!process_ref_close(&ref, !success, err)) {
    if (success) {
      OV_ERROR_ADD_TRACE(err);
    }
    success = false;
  }
  return success;
}

/** @brief What an act does */
enum act_kind {
  ACT_TERMINATE, //!< end the process a record names
  ACT_PAUSE,     //!< stop the Windows Audio service
  ACT_RESUME,    //!< start the Windows Audio service again
};

/** @brief One long act on the processes, on a thread of its own */
struct process_act {
  struct async *async;    //!< the thread of the act
  HANDLE stop;            //!< up once the caller asked the act to stop
  async_notify_fn notify; //!< the caller's call, may be NULL
  void *userdata;         //!< handed to notify
  struct ov_error *err;   //!< borrowed, the caller's, written by the thread of the act
  bool ok;                //!< whether the act went through
  enum act_kind kind;     //!< what the act is
  uint32_t pid;           //!< the process to end, when the act is a terminate
  uint64_t event_time_ms; //!< when the record was written, when the act is a terminate
};

/**
 * @brief Tell the caller what came of the act
 *
 * Called on the thread that asked the act to stop, or on the thread of the act once it is
 * over, which is what stopped says.
 *
 * @param userdata the act
 * @param stopped true when the caller asked the act to stop
 */
static void act_notify(void *const userdata, bool const stopped) {
  struct process_act *const act = (struct process_act *)userdata;

  if (stopped) {
    if (!SetEvent(act->stop)) {
      OV_ERROR_SET_HRESULT(act->err, HRESULT_FROM_WIN32(GetLastError()));
      act->ok = false;
    }
    return;
  }
  if (act->notify != NULL) {
    act->notify(act->userdata, false);
  }
}

/**
 * @brief Run the act
 *
 * @param ctx the act, as this thread may reach it
 */
static void act_body(struct async_context *const ctx) {
  struct process_act *const act = (struct process_act *)ctx->userdata;

  switch (act->kind) {
  case ACT_TERMINATE:
    act->ok = end_process(act->pid, act->event_time_ms, act->stop, act->err);
    break;
  case ACT_PAUSE:
    act->ok = pause_service(act->stop, act->err);
    break;
  case ACT_RESUME:
    act->ok = resume_service(act->stop, act->err);
    break;
  }
}

static struct process_act *act_start(enum act_kind const kind,
                                     uint32_t const pid,
                                     uint64_t const event_time_ms,
                                     async_notify_fn const notify,
                                     void *const userdata,
                                     struct ov_error *const err) {
  struct process_act *act = NULL;
  bool success = false;

  if (!OV_REALLOC(&act, 1, sizeof(*act))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(act, 0, sizeof(*act));
  act->kind = kind;
  act->pid = pid;
  act->event_time_ms = event_time_ms;
  act->notify = notify;
  act->userdata = userdata;
  act->err = err;
  act->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (act->stop == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // the event a stop sets is made here, so an ask is answered from the first moment the act
  // is there rather than only once its body got that far
  act->async = async_start(act_body, act, act_notify, err);
  if (act->async == NULL) {
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    if ((act != NULL) && (act->stop != NULL)) {
      if (!CloseHandle(act->stop)) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    }
    OV_FREE(&act);
  }
  return success ? act : NULL;
}

static NODISCARD struct process_act *terminate(
    uint32_t const pid, uint64_t const event_time_ms, async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  return act_start(ACT_TERMINATE, pid, event_time_ms, notify, userdata, err);
}

static NODISCARD struct process_act *pause_audio_service(async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  return act_start(ACT_PAUSE, 0, 0, notify, userdata, err);
}

static NODISCARD struct process_act *resume_audio_service(async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  return act_start(ACT_RESUME, 0, 0, notify, userdata, err);
}

static NODISCARD bool act_release(struct process_act **const act) {
  bool ok = false;
  bool joined = false;

  if ((act == NULL) || (*act == NULL)) {
    return false;
  }
  joined = async_release(&(*act)->async, (*act)->err);
  ok = (*act)->ok;
  if (!joined) {
    OV_ERROR_ADD_TRACE((*act)->err);
    ok = false;
  }
  if (!CloseHandle((*act)->stop)) {
    if (ok) {
      OV_ERROR_SET_HRESULT((*act)->err, HRESULT_FROM_WIN32(GetLastError()));
    } else {
      OV_ERROR_PUSH((*act)->err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
    }
    ok = false;
  }
  OV_FREE(act);
  return ok;
}

static void act_cancel(struct process_act *const act) {
  if (act != NULL) {
    async_cancel(act->async);
  }
}

/** @brief The machine of the system, asked through the snapshot and the open handle */
static struct process_manager const kSystemProcessManager = {
    .take_snapshot = take_snapshot,
    .terminate = terminate,
    .pause_audio_service = pause_audio_service,
    .resume_audio_service = resume_audio_service,
    .act_release = act_release,
    .act_cancel = act_cancel,
};

struct process_manager const *process_manager_system(void) { return &kSystemProcessManager; }
