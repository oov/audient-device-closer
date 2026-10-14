#include "audio_service.h"

#include <windows.h>

#include <winsvc.h>

/** @brief The name Windows gives the service that runs the audio engine */
#define AUDIO_SERVICE_NAME L"Audiosrv"

/**
 * @brief Wait until a service reports the state that was asked for
 *
 * A control that is sent comes back as accepted long before the service has done anything,
 * so the caller has to wait for the state to settle.  The wait is bounded, so a service
 * that never answers is a failure the caller can report rather than a hang.
 *
 * @param service the service to watch
 * @param wanted the state to wait for
 * @param err receives the failure
 * @return false when the state was not reached in time
 */
static bool wait_for_state(SC_HANDLE const service, DWORD const wanted, struct ov_error *const err) {
  SERVICE_STATUS_PROCESS status;
  DWORD got = 0;
  DWORD const deadline = GetTickCount() + 30000;
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
    waited = (status.dwWaitHint > 0) ? status.dwWaitHint : 250;
    if (waited > 1000) {
      waited = 1000;
    }
    Sleep(waited);
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
static bool control_and_wait(SC_HANDLE const service, DWORD const control, DWORD const wanted, struct ov_error *const err) {
  SERVICE_STATUS status;
  bool success = false;

  memset(&status, 0, sizeof(status));
  if (!ControlService(service, control, &status)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = wait_for_state(service, wanted, err);
  if (!success) {
    OV_ERROR_ADD_TRACE(err);
  }

cleanup:
  return success;
}

bool audio_service_pause(struct ov_error *const err) {
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
  if (!control_and_wait(service, SERVICE_CONTROL_STOP, SERVICE_STOPPED, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (service != NULL) {
    CloseServiceHandle(service);
  }
  if (scm != NULL) {
    CloseServiceHandle(scm);
  }
  return success;
}

bool audio_service_resume(struct ov_error *const err) {
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
  if (!StartServiceW(service, 0, NULL)) {
    DWORD const le = GetLastError();
    if (le != ERROR_SERVICE_ALREADY_RUNNING) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
      goto cleanup;
    }
  }
  if (!wait_for_state(service, SERVICE_RUNNING, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (service != NULL) {
    CloseServiceHandle(service);
  }
  if (scm != NULL) {
    CloseServiceHandle(scm);
  }
  return success;
}
