#include "paths.h"

#include <windows.h>

#include <ctype.h>
#include <shlobj.h>
#include <string.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void paths_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_ARRAY_DESTROY(value); // made by ov_sprintf_char
  }
}

/**
 * @brief Full path of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the path in UTF-8, NULL when it could not be read
 * @note Release with paths_string_free.
 */
char *paths_executable(struct ov_error *const err) {
  char *result = NULL;
  wchar_t self[PATHS_MAX];
  DWORD len = 0;

  if (err == NULL) {
    return NULL;
  }
  len = GetModuleFileNameW(NULL, self, (DWORD)(sizeof(self) / sizeof(self[0])));
  if ((len == 0) || (len >= (DWORD)(sizeof(self) / sizeof(self[0])))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return NULL;
  }
  self[len] = L'\0';
  if (!ov_sprintf_char(&result, err, NULL, "%ls", self)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return result;
}

/**
 * @brief The file name of a path, the part behind the last separator
 *
 * @param path_utf8 the path to look at, UTF-8, NULL is allowed
 * @return the file name inside path_utf8, empty when the path ends with a separator, NULL for
 *         NULL
 * @note The returned string lives inside path_utf8, there is nothing to release.
 */
char const *paths_file_name(char const *const path_utf8) {
  char const *name = NULL;

  if (path_utf8 == NULL) {
    return NULL;
  }
  name = path_utf8;
  for (char const *p = path_utf8; *p != '\0'; p++) {
    if ((*p == '\\') || (*p == '/') || (*p == ':')) {
      name = p + 1;
    }
  }
  return name;
}

bool paths_is_in_program_files(char const *const exe_path) {
  static char const *const folders[] = {"\\program files\\", "\\program files (x86)\\"};

  if (exe_path == NULL) {
    return false;
  }
  for (size_t i = 0; i < (sizeof(folders) / sizeof(folders[0])); i++) {
    size_t const n = strlen(folders[i]);
    for (char const *p = exe_path; *p != '\0'; p++) {
      size_t j = 0;
      while ((j < n) && (p[j] != '\0') && (tolower((unsigned char)p[j]) == folders[i][j])) {
        j++;
      }
      if (j == n) {
        return true;
      }
    }
  }
  return false;
}

char *paths_log_folder(struct ov_error *const err) {
  wchar_t *shared = NULL;
  char *result = NULL;
  char *joined = NULL;
  bool success = false;
  HRESULT const hr = SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &shared);

  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  if (!ov_sprintf_char(&result, err, NULL, "%ls", shared)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  joined = paths_join(result, "AudientDeviceCloser", err);
  if (joined == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (shared != NULL) {
    CoTaskMemFree(shared);
  }
  paths_string_free(&result);
  if (!success) {
    if (joined != NULL) {
      OV_ARRAY_DESTROY(&joined); // made by paths_join
    }
    return NULL;
  }
  return joined;
}

bool paths_create_folder(char const *const folder_utf8, struct ov_error *const err) {
  bool success = false;
  wchar_t *wide = NULL;

  if (folder_utf8 == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!ov_sprintf_wchar(&wide, err, NULL, L"%s", folder_utf8)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!CreateDirectoryW(wide, NULL)) {
    DWORD const last_error = GetLastError();
    if (last_error != ERROR_ALREADY_EXISTS) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last_error));
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (wide != NULL) {
    OV_ARRAY_DESTROY(&wide); // made by ov_sprintf_wchar
  }
  return success;
}

/**
 * @brief Join a directory and a file name with one backslash
 *
 * @param directory the directory, without a trailing separator
 * @param name the file name
 * @param err receives a failure of the join
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with paths_string_free.
 */
char *paths_join(char const *const directory, char const *const name, struct ov_error *const err) {
  char *result = NULL;

  if ((directory == NULL) || (name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!ov_sprintf_char(&result, err, NULL, "%hs\\%hs", directory, name)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return result;
}

/**
 * @brief The path of a file that sits next to an executable under the same name
 *
 * @param exe_path the executable the file belongs to, UTF-8
 * @param extension the extension the file carries, with its dot
 * @param err receives the failure of the build
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with OV_FREE.
 */
char *paths_replace_extension(char const *const exe_path, char const *const extension, struct ov_error *const err) {
  char *result = NULL;
  size_t len = 0;
  size_t dot = 0;
  size_t sep = 0;
  size_t ext_len = 0;

  if ((exe_path == NULL) || (extension == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  len = strlen(exe_path);
  ext_len = strlen(extension);
  for (size_t i = 0; i < len; i++) {
    if ((exe_path[i] == '\\') || (exe_path[i] == '/') || (exe_path[i] == ':')) {
      sep = i + 1;
    }
    if (exe_path[i] == '.') {
      dot = i;
    }
  }
  if (dot < sep) {
    dot = len; // a dot inside a directory name is not the extension
  }
  if (!OV_REALLOC(&result, dot + ext_len + 1, sizeof(result[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  memcpy(result, exe_path, dot);
  memcpy(&result[dot], extension, ext_len + 1); // the terminator comes along with the extension
  return result;
}
