#include "instance.h"

#include <windows.h>

#include <ovbase.h>

/**
 * @brief The name the mutex of a running copy carries
 *
 * Windows compares mutex names case-sensitively, and installer.iss names the same string in
 * its AppMutex directive: the two have to agree exactly.
 */
static wchar_t const instance_mutex_name[] = L"AudientDeviceCloser.Mutex";

/**
 * @brief Take the mutex of this name, which marks the copy that holds it as the running one
 *
 * @param mutex_name the name of the mutex to take
 * @param err receives the reason no mutex is held
 * @return the instance of the running copy, one without a handle when err says why not
 */
static NODISCARD struct instance take_named(wchar_t const *const mutex_name, struct ov_error *const err) {
  struct instance instance = {0};
  HANDLE mutex = NULL;

  if (mutex_name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  mutex = CreateMutexW(NULL, FALSE, mutex_name);
  if (mutex == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // GetLastError names the answer of the create alone: it is read before any other call
  // can replace it, and only the existing mutex is of interest here
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    goto cleanup;
  }
  instance.handle = mutex;
  mutex = NULL;

cleanup:
  if (mutex != NULL) {
    // The create hands back a handle of the mutex of the other copy as well, and this copy
    // holds none of it.  The call only comes here with a reason already, so the close joins
    // that reason rather than taking its place.
    if (!CloseHandle(mutex)) {
      OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
    }
  }
  return instance;
}

NODISCARD struct instance instance_take(struct ov_error *const err) { return take_named(instance_mutex_name, err); }

NODISCARD bool instance_release(struct instance *const instance, struct ov_error *const err) {
  bool success = true;

  if ((instance == NULL) || (instance->handle == NULL)) {
    return true;
  }
  if (!CloseHandle((HANDLE)instance->handle)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    success = false;
  }
  instance->handle = NULL;
  return success;
}
