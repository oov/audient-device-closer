#include "paths.h"

#include <windows.h>

#include <shlobj.h>
#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief Full path of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the path, NULL when it could not be read
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_executable(struct ov_error *const err) {
  wchar_t *result = NULL;
  // GetModuleFileNameW fills a buffer of this size; a longer path leaves the buffer full
  // to the end, and the check below refuses that answer.
  wchar_t self[MAX_PATH];
  DWORD len = 0;

  len = GetModuleFileNameW(NULL, self, (DWORD)(sizeof(self) / sizeof(self[0])));
  if ((len == 0) || (len >= (DWORD)(sizeof(self) / sizeof(self[0])))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return NULL;
  }
  self[len] = L'\0';
  if (!ov_sprintf_wchar(&result, err, NULL, L"%ls", self)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return result;
}

/**
 * @brief The file name of a path, the part behind the last separator
 *
 * @param path the path to look at, NULL is allowed
 * @return the file name inside path, empty when the path ends with a separator, NULL for
 *         NULL
 * @note The returned string lives inside path, there is nothing to release.
 */
wchar_t const *paths_file_name(wchar_t const *const path) {
  wchar_t const *name = NULL;

  if (path == NULL) {
    return NULL;
  }
  name = path;
  for (wchar_t const *p = path; *p != L'\0'; p++) {
    if ((*p == L'\\') || (*p == L'/') || (*p == L':')) {
      name = p + 1;
    }
  }
  return name;
}

/**
 * @brief Is the file name of a path the name the caller gives
 *
 * @param path the path to look at, NULL is not a match
 * @param name the file name to look for, NULL and the empty name are not a match
 * @return true when the last component of path is name
 */
bool paths_file_name_matches(wchar_t const *const path, wchar_t const *const name) {
  wchar_t const *const leaf = paths_file_name(path);

  if ((leaf == NULL) || (name == NULL) || (name[0] == L'\0')) {
    return false;
  }
  return CompareStringOrdinal(leaf, -1, name, -1, TRUE) == CSTR_EQUAL;
}

/**
 * @brief Read the identity the file system gives a folder
 *
 * The identity is the one the system compares, so two spellings of one folder give the same
 * answer and a copy of the folder elsewhere does not.
 *
 * @param path the folder to open
 * @param out receives the identity
 * @param err receives the failure of the open or of the read
 * @return false when the identity could not be read
 */
static NODISCARD bool folder_identity(wchar_t const *const path, FILE_ID_INFO *const out, struct ov_error *const err) {
  HANDLE const h = CreateFileW(path,
                               FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL,
                               OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS,
                               NULL);
  bool success = false;

  if (h == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  success = GetFileInformationByHandleEx(h, FileIdInfo, out, sizeof(*out)) != 0;
  if (!success) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
  }
  CloseHandle(h);
  return success;
}

NODISCARD ov_tribool paths_same_directory(wchar_t const *const a, wchar_t const *const b, struct ov_error *const err) {
  FILE_ID_INFO ida;
  FILE_ID_INFO idb;

  if ((a == NULL) || (b == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return ov_indeterminate;
  }
  if (!folder_identity(a, &ida, err)) {
    if (ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
        ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))) {
      OV_ERROR_DESTROY(err); // nothing of that name is not the same folder
      return ov_false;
    }
    OV_ERROR_ADD_TRACE(err);
    return ov_indeterminate;
  }
  if (!folder_identity(b, &idb, err)) {
    if (ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
        ov_error_is(err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))) {
      OV_ERROR_DESTROY(err);
      return ov_false;
    }
    OV_ERROR_ADD_TRACE(err);
    return ov_indeterminate;
  }
  return (ida.VolumeSerialNumber == idb.VolumeSerialNumber &&
          memcmp(ida.FileId.Identifier, idb.FileId.Identifier, sizeof(ida.FileId.Identifier)) == 0)
             ? ov_true
             : ov_false;
}

NODISCARD wchar_t *paths_final_path(wchar_t const *const path, struct ov_error *const err) {
  wchar_t *result = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  DWORD needed = 0;
  bool success = false;

  if (path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  needed = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (needed == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!OV_ARRAY_GROW(&result, (size_t)needed + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  needed = GetFinalPathNameByHandleW(h, result, needed + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (needed == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  OV_ARRAY_SET_LENGTH(result, needed);
  // the prefix the API puts in front of the path is no use to the command line it is written
  // into, and a share comes as \\?\UNC\server\share
  if (wcsncmp(result, L"\\\\?\\UNC\\", 8) == 0) {
    memmove(result + 2, result + 8, (wcslen(result + 8) + 1) * sizeof(result[0]));
  } else if (wcsncmp(result, L"\\\\?\\", 4) == 0) {
    memmove(result, result + 4, (wcslen(result + 4) + 1) * sizeof(result[0]));
  }
  OV_ARRAY_SET_LENGTH(result, wcslen(result));
  success = true;

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  if (!success && (result != NULL)) {
    OV_ARRAY_DESTROY(&result);
  }
  return success ? result : NULL;
}

NODISCARD wchar_t *paths_log_folder(struct ov_error *const err) {
  wchar_t *shared = NULL;
  wchar_t *result = NULL;
  wchar_t *joined = NULL;
  bool success = false;
  HRESULT const hr = SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &shared);

  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&result, err, NULL, L"%ls", shared)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  joined = paths_join(result, L"AudientDeviceCloser", err);
  if (joined == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (shared != NULL) {
    CoTaskMemFree(shared);
  }
  if (result != NULL) {
    OV_ARRAY_DESTROY(&result);
  }
  if (!success) {
    if (joined != NULL) {
      OV_ARRAY_DESTROY(&joined);
    }
    return NULL;
  }
  return joined;
}

NODISCARD bool paths_create_folder(wchar_t const *const folder, struct ov_error *const err) {
  if (folder == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!CreateDirectoryW(folder, NULL)) {
    DWORD const last_error = GetLastError();
    if (last_error != ERROR_ALREADY_EXISTS) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last_error));
      return false;
    }
  }
  return true;
}

/**
 * @brief Join a directory and a file name with one backslash
 *
 * @param directory the directory, without a trailing separator
 * @param name the file name
 * @param err receives a failure of the join
 * @return the path, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_join(wchar_t const *const directory, wchar_t const *const name, struct ov_error *const err) {
  wchar_t *result = NULL;
  bool success = false;

  if ((directory == NULL) || (name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&result, err, NULL, L"%ls\\%ls", directory, name)) {
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
 * @brief The path of a file that sits next to an executable under the same name
 *
 * @param exe_path the executable the file belongs to
 * @param extension the extension the file carries, with its dot
 * @param err receives the failure of the build
 * @return the path, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_replace_extension(wchar_t const *const exe_path, wchar_t const *const extension, struct ov_error *const err) {
  wchar_t *result = NULL;
  size_t len = 0;
  size_t dot = 0;
  size_t sep = 0;
  size_t ext_len = 0;
  bool success = false;

  if ((exe_path == NULL) || (extension == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  len = wcslen(exe_path);
  ext_len = wcslen(extension);
  for (size_t i = 0; i < len; i++) {
    if ((exe_path[i] == L'\\') || (exe_path[i] == L'/') || (exe_path[i] == L':')) {
      sep = i + 1;
    }
    if (exe_path[i] == L'.') {
      dot = i;
    }
  }
  if (dot < sep) {
    dot = len; // a dot inside a directory name is not the extension
  }
  if (!OV_ARRAY_GROW(&result, dot + ext_len + 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  OV_ARRAY_SET_LENGTH(result, dot + ext_len);
  memcpy(result, exe_path, dot * sizeof(result[0]));
  memcpy(&result[dot], extension, (ext_len + 1) * sizeof(result[0])); // the terminator comes along with the extension
  success = true;

cleanup:
  if (!success) {
    if (result != NULL) {
      OV_ARRAY_DESTROY(&result);
    }
  }
  return success ? result : NULL;
}
