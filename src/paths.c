#include "paths.h"

#include <windows.h>

#include <string.h>

#include <ovarray.h>
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
 * @brief Directory of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the directory in UTF-8 without a trailing separator, NULL when it could not be read
 * @note Release with paths_string_free.
 */
char *paths_executable_dir(struct ov_error *const err) {
  char *exe = NULL;
  size_t len = 0;

  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  len = strlen(exe);
  while (len > 0) {
    char const c = exe[len - 1];
    if ((c == '\\') || (c == '/')) {
      break;
    }
    len--;
  }
  if (len == 0) {
    len = strlen(exe);
  } else {
    len--;
  }
  exe[len] = '\0';
  return exe;
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
