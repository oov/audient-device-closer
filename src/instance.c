#include "instance.h"

#include <windows.h>

#include <ovbase.h>

HANDLE instance_take(wchar_t const *const mutex_name, bool *const out_other_running, struct ov_error *const err) {
  HANDLE mutex = NULL;

  if ((mutex_name == NULL) || (out_other_running == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_other_running = false;
  mutex = CreateMutexW(NULL, FALSE, mutex_name);
  if (mutex == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // GetLastError names the answer of the create alone: it is read before any other call
  // can replace it, and only the existing mutex is of interest here
  *out_other_running = (GetLastError() == ERROR_ALREADY_EXISTS);

cleanup:
  return mutex;
}

void instance_release(HANDLE *const mutex) {
  if ((mutex != NULL) && (*mutex != NULL)) {
    CloseHandle(*mutex);
    *mutex = NULL;
  }
}
