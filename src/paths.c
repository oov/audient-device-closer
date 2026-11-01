#include "paths.h"

#include <windows.h>

#include <shlobj.h>
#include <shlwapi.h>
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
  // PathRelativePathToW, which paths_is_in_program_files compares through, refuses a path
  // of MAX_PATH or more, so a longer one could not be followed through the program anyway.
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
 * @brief Is a path inside a folder
 *
 * The comparison is the one of the system, not of the strings.
 *
 * @param folder the folder to look inside
 * @param path the path to look at
 * @param err receives the failure of the comparison
 * @return ov_true when path names a file under folder, ov_false when it does not,
 *         ov_indeterminate when the two could not be compared, with err set
 */
static NODISCARD ov_tribool is_inside_folder(wchar_t const *const folder, wchar_t const *const path, struct ov_error *const err) {
  wchar_t relative[MAX_PATH];

  // PathRelativePathToW refuses both paths beyond MAX_PATH
  if ((wcslen(folder) >= MAX_PATH) || (wcslen(path) >= MAX_PATH)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, gettext("The path is too long to compare"));
    return ov_indeterminate;
  }
  if (!PathRelativePathToW(relative, folder, FILE_ATTRIBUTE_DIRECTORY, path, 0)) {
    return ov_false;
  }
  return ((relative[0] == L'.') && (relative[1] == L'.')) ? ov_false : ov_true;
}

/**
 * @brief Look up one known folder and ask whether the path is inside it
 *
 * @param id the known folder to look up
 * @param path the path to look at
 * @param err receives the failure of the lookup
 * @return ov_true when path is inside the folder, ov_false when it is not,
 *         ov_indeterminate when the answer could not be worked out, with err set
 */
static NODISCARD ov_tribool folder_holds(KNOWNFOLDERID const *const id, wchar_t const *const path, struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  wchar_t *folder = NULL;
  HRESULT const hr = SHGetKnownFolderPath(id, 0, NULL, &folder);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  if (folder == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  result = is_inside_folder(folder, path, err);
  if (result == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

cleanup:
  if (folder != NULL) {
    CoTaskMemFree(folder);
  }
  return result;
}

NODISCARD ov_tribool paths_is_in_program_files(wchar_t const *const exe_path, struct ov_error *const err) {
  if (exe_path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return ov_indeterminate;
  }

  ov_tribool ret = ov_indeterminate;

  // is_inside_folder compares with PathRelativePathToW, which refuses MAX_PATH and beyond
  if (wcslen(exe_path) >= MAX_PATH) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  static KNOWNFOLDERID const *const folders[] = {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86};
  for (size_t i = 0; i < (sizeof(folders) / sizeof(folders[0])); i++) {
    ov_tribool const holds = folder_holds(folders[i], exe_path, err);

    if (holds == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (holds == ov_true) {
      ret = ov_true;
      goto cleanup;
    }
  }
  ret = ov_false;
cleanup:
  return ret;
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
