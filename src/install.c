#include "install.h"

#include <windows.h>

#include <shlwapi.h>

#include <ovarray.h>
#include <ovbase.h>

#include "paths.h"

/**
 * @brief The uninstall entry Inno Setup writes
 *
 * The name is the AppId of installer.iss with the suffix Inno Setup puts behind it.  The
 * AppId never changes: an upgrade finds the entry it has to replace by that name.
 */
static wchar_t const kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                                       L"{7A2C4E61-9B35-4D0F-8A47-2C1E5B9D3F80}_is1";

/** @brief The value of the entry that names the folder the installer installed into */
static wchar_t const kInstallLocation[] = L"InstallLocation";

/** @brief The answer install_init worked out; the portable one until it runs */
static bool g_installed;

/**
 * @brief The folder the installer recorded, as the value carries it
 *
 * The entry is read from the 64-bit view: the installer writes it there because it installs
 * the 64-bit program into the 64-bit Program Files.
 *
 * @param err receives the failure of the read
 * @return the folder, NULL when the entry is not there or could not be read
 * @note Release with OV_FREE.
 */
static NODISCARD wchar_t *record_location(struct ov_error *const err) {
  DWORD size = 0;
  LSTATUS status =
      RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, kInstallLocation, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, NULL, &size);
  wchar_t *value = NULL;

  if (status != ERROR_SUCCESS) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32((DWORD)status));
    return NULL;
  }
  if (size < sizeof(value[0])) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return NULL;
  }
  if (!OV_REALLOC(&value, (size_t)size / sizeof(value[0]) + 1, sizeof(value[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  status = RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, kInstallLocation, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, value, &size);
  if (status != ERROR_SUCCESS) {
    OV_FREE(&value);
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32((DWORD)status));
    return NULL;
  }
  return value;
}

NODISCARD bool install_init(struct ov_error *const err) {
  bool success = false;
  wchar_t *exe = NULL;
  wchar_t *record = NULL;

  g_installed = false; // a lookup that does not finish leaves the safe answer
  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!PathRemoveFileSpecW(exe)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  record = record_location(err);
  if (record == NULL) {
    if (ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) {
      // the machine carries no install entry: this copy was not put here by the installer
      OV_ERROR_DESTROY(err);
      success = true;
      goto cleanup;
    }
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    ov_tribool const same = paths_same_directory(exe, record, err);

    if (same == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    g_installed = (same == ov_true);
  }
  success = true;

cleanup:
  if (record != NULL) {
    OV_FREE(&record);
  }
  if (exe != NULL) {
    OV_ARRAY_DESTROY(&exe);
  }
  return success;
}

NODISCARD bool install_is_installed(void) { return g_installed; }
