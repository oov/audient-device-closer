#include "settings.h"

#include <windows.h>

#include <string.h>

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  if __has_warning("-Wdocumentation-unknown-command")
#    pragma GCC diagnostic ignored "-Wdocumentation-unknown-command"
#  endif
#endif // __GNUC__
#include <yyjson.h>
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "paths.h"

/**
 * @brief The delay a run nobody is watching waits before it looks at the devices
 *
 * The settings file may say another value; this is the one the
 * settings fall back to when the file does not say.
 */
enum { adc_settings_auto_run_delay_ms = 5000 };

/**
 * @brief How long the confirmation waits for a released device to come back
 *
 * The settings file may say another value; this is the one the settings fall back to when
 * the file does not say.
 */
enum { adc_settings_confirm_max_wait_ms = 5000 };

/**
 * @brief The allocator yyjson works through
 *
 * yyjson must go through the ovbase allocator, otherwise its blocks are invisible to the
 * leak detector and the Debug build cannot tell who leaked what.
 *
 * @param ctx unused
 * @param size how many bytes to make
 * @return the block, NULL when there is none
 */
static void *ov_alloc(void *const ctx, size_t const size) {
  void *p = NULL;
  (void)ctx;
  if (!OV_REALLOC(&p, size, 1)) {
    return NULL;
  }
  return p;
}

/**
 * @brief The allocator yyjson works through, for a block that grows
 *
 * @param ctx unused
 * @param ptr the block to replace, NULL makes a new one
 * @param old_size how big the block is now
 * @param size how big it should be
 * @return the block, NULL when there is none
 */
static void *ov_realloc(void *const ctx, void *const ptr, size_t const old_size, size_t const size) {
  void *p = ptr;
  (void)ctx;
  (void)old_size;
  if (p == NULL) {
    return ov_alloc(ctx, size);
  }
  if (!OV_REALLOC(&p, size, 1)) {
    return NULL;
  }
  return p;
}

/**
 * @brief The allocator yyjson works through, for a block that goes
 *
 * @param ctx unused
 * @param ptr the block to release, NULL is allowed
 */
static void ov_release(void *const ctx, void *const ptr) {
  void *p = ptr;
  (void)ctx;
  OV_FREE(&p);
}

static struct yyjson_alc const kAlc = {
    .malloc = ov_alloc,
    .realloc = ov_realloc,
    .free = ov_release,
    .ctx = NULL,
};

/**
 * @brief A copy of a string of the settings file
 *
 * MEM_FILEPOS_PARAMS hands the position of the caller down: the allocator records where a
 * block was made, and without it every copy in this file would be blamed on this helper in
 * a leak report.
 *
 * @param src the string to copy, UTF-8
 * @return the copy, NULL when there is no memory for it
 * @note Release with OV_FREE.
 */
static char *dup_text(char const *const src MEM_FILEPOS_PARAMS) {
  char *dst = NULL;
  size_t const len = strlen(src) + 1;
  if (!ov_mem_realloc(&dst, len, sizeof(dst[0]) MEM_FILEPOS_VALUES_PASSTHRU)) {
    return NULL;
  }
  memcpy(dst, src, len);
  return dst;
}

/**
 * @brief A copy of a string of the settings file, made at the position of the call
 *
 * The caller should not have to write MEM_FILEPOS_VALUES itself: this appends the position
 * of the call, which is what dup_text hands down to the allocator.
 *
 * @param src the string to copy, UTF-8
 * @return the copy, NULL when there is no memory for it
 * @note Release with OV_FREE.
 */
#define DUP_TEXT(src) dup_text((src)MEM_FILEPOS_VALUES)

/**
 * @brief Read one string of the settings file
 *
 * The error is raised where the problem was found and the caller adds its own position to the
 * stack with OV_ERROR_ADD_TRACE, so the report names the read that found the problem and the
 * read that wanted the key.
 *
 * @param root the object that holds the key
 * @param key the key to read
 * @param required when true, a key that is not there is an error
 * @param out receives the value, release with settings_string_free
 * @param err receives the problem of the key
 * @return false when the key is missing, of the wrong type or out of memory
 */
static bool get_str(yyjson_val *const root, char const *const key, bool const required, char **const out, struct ov_error *const err) {
  yyjson_val *v = NULL;

  *out = NULL;
  v = yyjson_obj_get(root, key);
  if (v == NULL) {
    if (required) {
      OV_ERROR_SETF(
          err, ov_error_type_generic, ov_error_generic_not_found, "the settings file has no %1$hs", "the settings file has no %1$hs", key);
      return false;
    }
    return true;
  }
  if (!yyjson_is_str(v)) {
    OV_ERROR_SETF(err, ov_error_type_generic, ov_error_generic_invalid_argument, "%1$hs must be a string", "%1$hs must be a string", key);
    return false;
  }
  *out = DUP_TEXT(yyjson_get_str(v));
  if (*out == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return false;
  }
  return true;
}

/**
 * @brief Read one number of the settings file
 *
 * The error is raised where the problem was found and the caller adds its own position to the
 * stack with OV_ERROR_ADD_TRACE, so the report names the read that found the problem and the
 * read that wanted the key.
 *
 * @param root the object that holds the key
 * @param key the key to read
 * @param required when true, a key that is not there is an error; when false, def is used
 * @param min the smallest value this program accepts
 * @param max the largest value this program accepts
 * @param def the value the settings run with when the key is not there and not required
 * @param out receives the value
 * @param err receives the problem of the key
 * @return false when the key is missing although it was required, not a number, out of
 *         range or out of memory
 */
static bool get_ulong_full(yyjson_val *const root,
                           char const *const key,
                           bool const required,
                           unsigned long const min,
                           unsigned long const max,
                           unsigned long const def,
                           unsigned long *const out,
                           struct ov_error *const err) {
  yyjson_val *v = yyjson_obj_get(root, key);

  *out = def;
  if (v == NULL) {
    if (required) {
      OV_ERROR_SETF(
          err, ov_error_type_generic, ov_error_generic_not_found, "the settings file has no %1$hs", "the settings file has no %1$hs", key);
      return false;
    }
    return true;
  }
  if (!yyjson_is_uint(v)) {
    OV_ERROR_SETF(err, ov_error_type_generic, ov_error_generic_invalid_argument, "%1$hs must be a number", "%1$hs must be a number", key);
    return false;
  }
  unsigned long const got = (unsigned long)yyjson_get_uint(v);
  if ((got < min) || (got > max)) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_invalid_argument,
                  "%1$hs is out of range: %2$lu",
                  "%1$hs is out of range: %2$lu",
                  key,
                  got);
    return false;
  }
  *out = got;
  return true;
}

/**
 * @brief Read one switch of the settings file
 *
 * The error is raised where the problem was found and the caller adds its own position to the
 * stack with OV_ERROR_ADD_TRACE, so the report names the read that found the problem and the
 * read that wanted the key.
 *
 * @param root the object that holds the key
 * @param key the key to read
 * @param def the value the settings run with when the key is not there
 * @param out receives the value
 * @param err receives the problem of the key
 * @return false when the key is of the wrong type
 */
static bool get_bool_optional(yyjson_val *const root, char const *const key, bool const def, bool *const out, struct ov_error *const err) {
  yyjson_val *v = yyjson_obj_get(root, key);

  *out = def;
  if (v == NULL) {
    return true;
  }
  if (!yyjson_is_bool(v)) {
    OV_ERROR_SETF(
        err, ov_error_type_generic, ov_error_generic_invalid_argument, "%1$hs must be true or false", "%1$hs must be true or false", key);
    return false;
  }
  *out = yyjson_get_bool(v);
  return true;
}

/**
 * @brief Add one image name of the mixer list to the growing list
 *
 * @param out the list to grow
 * @param item one entry of the list
 * @param err receives the problem of the entry
 * @return false when the entry is refused or there is no memory for it
 */
static bool mixer_name_push(char ***const out, yyjson_val *const item, struct ov_error *const err) {
  bool success = false;
  char *name = NULL;

  if (!yyjson_is_str(item)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "every mixer image name must be a string");
    goto cleanup;
  }
  name = DUP_TEXT(yyjson_get_str(item));
  if (name == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (name[0] == '\0') {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "a mixer image name must not be empty");
    goto cleanup;
  }
  if (!OV_ARRAY_PUSH(out, name)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  name = NULL;
  success = true;

cleanup:
  if (name != NULL) {
    OV_FREE(&name);
  }
  return success;
}

/**
 * @brief Read one list of image names of the settings file
 *
 * The list holds the image names of the processes this program may close.
 *
 * @param root the object that holds the key
 * @param key the key of the list
 * @param required when true, a key that is not there is an error
 * @param out receives the growing list, owned by the caller afterwards
 * @param err receives the problem of the list
 * @return false when the list is missing, of the wrong shape or out of memory
 */
static bool
get_str_list(yyjson_val *const root, char const *const key, bool const required, char ***const out, struct ov_error *const err) {
  yyjson_val *v = yyjson_obj_get(root, key);
  size_t n = 0;
  bool pushed_all = true;

  if (v == NULL) {
    if (required) {
      OV_ERROR_SETF(
          err, ov_error_type_generic, ov_error_generic_not_found, "the settings file has no %1$hs", "the settings file has no %1$hs", key);
      return false;
    }
    return true;
  }
  if (!yyjson_is_arr(v)) {
    OV_ERROR_SETF(err, ov_error_type_generic, ov_error_generic_invalid_argument, "%1$hs must be an array", "%1$hs must be an array", key);
    return false;
  }
  n = yyjson_arr_size(v);
  if (n == 0) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_invalid_argument,
                  "%1$hs must name at least one image",
                  "%1$hs must name at least one image",
                  key);
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!mixer_name_push(out, yyjson_arr_get(v, i), err)) {
      pushed_all = false;
      break;
    }
  }
  if (!pushed_all) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}

/**
 * @brief Read one device of the device table and hand over its two strings
 *
 * The table is the name and the identifier of every device this program supports.  Both
 * are required and neither may be empty.
 *
 * @param item one entry of the table
 * @param out receives the entry, owned by the caller afterwards
 * @param err receives the problem of the entry
 * @return false when the entry is not a device this program can run with
 */
static bool device_entry_load(yyjson_val *const item, struct device_entry *const out, struct ov_error *const err) {
  bool success = false;
  char *name = NULL;
  char *id = NULL;

  if (!yyjson_is_obj(item)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "every device must be an object");
    goto cleanup;
  }
  if (!get_str(item, "name", true, &name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (name[0] == '\0') {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "a device name must not be empty");
    goto cleanup;
  }
  if (!get_str(item, "id", true, &id, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (id[0] == '\0') {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "a device identifier must not be empty");
    goto cleanup;
  }
  out->name = name;
  name = NULL;
  out->id = id;
  id = NULL;
  success = true;

cleanup:
  if (name) {
    OV_FREE(&name);
  }
  if (id) {
    OV_FREE(&id);
  }
  return success;
}

/**
 * @brief Add one device of the table to the growing list
 *
 * @param out the list to grow
 * @param item one entry of the table
 * @param err receives the problem of the entry
 * @return false when the entry is refused or there is no memory for it
 */
static bool device_push(struct device_entry **const out, yyjson_val *const item, struct ov_error *const err) {
  bool success = false;
  struct device_entry e;

  memset(&e, 0, sizeof(e));
  if (!device_entry_load(item, &e, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!OV_ARRAY_PUSH(out, e)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    OV_FREE(&e.id);
    OV_FREE(&e.name);
  }
  return success;
}

/**
 * @brief Read the device table of the settings file
 *
 * The key is optional: a file that an older build wrote named the devices another way.
 *
 * @param root the object that holds the key
 * @param out receives the growing list, owned by the caller afterwards
 * @param err receives the problem of the table
 * @return false when the table is not one this program can run with
 */
static bool get_devices(yyjson_val *const root, struct device_entry **const out, struct ov_error *const err) {
  bool success = false;
  yyjson_val *v = yyjson_obj_get(root, "devices");
  size_t n = 0;
  bool pushed_all = true;

  if (v == NULL) {
    success = true;
    goto cleanup;
  }
  if (!yyjson_is_arr(v)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "devices must be an array");
    goto cleanup;
  }
  n = yyjson_arr_size(v);
  if (n == 0) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "devices must name at least one device");
    goto cleanup;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!device_push(out, yyjson_arr_get(v, i), err)) {
      pushed_all = false;
      break;
    }
  }
  if (!pushed_all) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Read the device table this program looks at
 *
 * @param root the object that holds the keys
 * @param out filled in and owned by the caller afterwards
 * @param err receives the problem of the devices
 * @return false when the table is missing or not one this program can run with
 */
static bool load_devices(yyjson_val *const root, struct settings *const out, struct ov_error *const err) {
  if (!get_devices(root, &out->devices, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (OV_ARRAY_LENGTH(out->devices) == 0) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "the settings file has no %1$hs",
                  "the settings file has no %1$hs",
                  "devices");
    return false;
  }
  return true;
}

/**
 * @brief Fill the settings from the text of the file
 *
 * Nothing is read from disk here.  Required keys must be strings, veto_event_id must
 * be a number in 1..65535.
 *
 * @param json_utf8 the contents of the settings file, UTF-8
 * @param out filled in and owned by the caller afterwards; release with settings_release
 * @param err receives the first problem of the file
 * @return false when the text does not describe settings this program can run with
 */
static bool settings_apply_json(char const *const json_utf8, struct settings *const out, struct ov_error *const err) {
  bool success = false;
  yyjson_doc *doc = NULL;
  yyjson_val *root = NULL;
  yyjson_read_err parse_err;

  if ((json_utf8 == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(out, 0, sizeof(*out));

  doc = yyjson_read_opts((char *)ov_deconster_(json_utf8), strlen(json_utf8), 0, &kAlc, &parse_err);
  if (doc == NULL) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_invalid_argument,
                  "the settings file is not valid JSON (offset %1$lu)",
                  "the settings file is not valid JSON (offset %1$lu)",
                  (unsigned long)parse_err.pos);
    goto cleanup;
  }
  root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "the settings file must contain one object");
    goto cleanup;
  }

  if (!get_str(root, "ks_service", true, &out->ks_service, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!load_devices(root, out, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_str(root, "veto_log_name", true, &out->veto_log_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_str(root, "veto_provider_name", true, &out->veto_provider_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_ulong_full(root, "veto_event_id", true, 1, 65535, 0, &out->veto_event_id, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_str_list(root, "mixer_process_names", true, &out->mixer_process_names, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_str(root, "task_name", false, &out->task_name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_ulong_full(root, "auto_run_delay_ms", false, 0, 600000, adc_settings_auto_run_delay_ms, &out->auto_run_delay_ms, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_ulong_full(root, "confirm_max_wait_ms", false, 0, 600000, adc_settings_confirm_max_wait_ms, &out->confirm_max_wait_ms, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!get_bool_optional(root, "use_theme", true, &out->use_theme, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    settings_release(out);
  }
  if (doc != NULL) {
    yyjson_doc_free(doc);
  }
  return success;
}

/**
 * @brief Read a whole file into memory
 *
 * @param path_utf8 the file to read, UTF-8
 * @param out_text receives the contents, release with OV_FREE
 * @param err receives the failure of the read
 * @return false when the file could not be read
 */
static bool read_whole_file(char const *const path_utf8, char **const out_text, struct ov_error *const err) {
  bool success = false;
  wchar_t *path = NULL;
  char *text = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  DWORD got = 0;

  if (!ov_sprintf_wchar(&path, err, NULL, L"%s", path_utf8)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    OV_ERROR_SETF(err, ov_error_type_hresult, (int)HRESULT_FROM_WIN32(GetLastError()), "cannot open %1$hs", "cannot open %1$hs", path_utf8);
    goto cleanup;
  }
  if (!GetFileSizeEx(h, &size) || (size.QuadPart < 0) || (size.QuadPart > (LONGLONG)0x400000)) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_invalid_argument, "the settings file has an unusable size");
    goto cleanup;
  }
  if (!OV_REALLOC(&text, (size_t)size.QuadPart + 1, sizeof(text[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!ReadFile(h, text, (DWORD)size.QuadPart, &got, NULL) || (got != (DWORD)size.QuadPart)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  text[got] = '\0';
  if ((got >= 3) && ((unsigned char)text[0] == 0xEF) && ((unsigned char)text[1] == 0xBB) && ((unsigned char)text[2] == 0xBF)) {
    memmove(text, &text[3], (size_t)got - 3);
    text[got - 3] = '\0';
  }
  *out_text = text;
  text = NULL;
  success = true;

cleanup:
  if (path) {
    OV_ARRAY_DESTROY(&path);
  }
  if (text) {
    OV_FREE(&text);
  }
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  return success;
}

/**
 * @brief Read the settings file and fill the settings from it
 *
 * @param path_utf8 the file to read, UTF-8
 * @param out filled in and owned by the caller afterwards; release with settings_release
 * @param err receives the failure of the read or of the contents
 * @return false when the file could not be read or understood
 */
bool settings_load(char const *const path_utf8, struct settings *const out, struct ov_error *const err) {
  bool success = false;
  char *text = NULL;

  if (!read_whole_file(path_utf8, &text, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!settings_apply_json(text, out, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (text) {
    OV_FREE(&text);
  }
  return success;
}

/**
 * @brief The settings file this program reads: the executable with its extension replaced
 *        by ".json", next to the executable
 *
 * The log gets its name the same way, so the two files always carry the name of the program
 * that reads them.
 *
 * @param err receives the failure of the lookup
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with settings_string_free.
 */
char *settings_default_path(struct ov_error *const err) {
  char *exe = NULL;
  char *result = NULL;

  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = paths_replace_extension(exe, ".json", err);
  if (result == NULL) {
    OV_ERROR_ADD_TRACE(err);
  }

cleanup:
  if (exe != NULL) {
    paths_string_free(&exe);
  }
  return result;
}

/**
 * @brief Release the strings the settings hold
 *
 * @param s NULL is allowed
 */
void settings_release(struct settings *const s) {
  if (s == NULL) {
    return;
  }
  OV_FREE(&s->ks_service);
  if (s->devices != NULL) {
    size_t const n = OV_ARRAY_LENGTH(s->devices);
    for (size_t i = 0; i < n; ++i) {
      OV_FREE(&s->devices[i].id);
      OV_FREE(&s->devices[i].name);
    }
    OV_ARRAY_DESTROY(&s->devices);
  }
  OV_FREE(&s->veto_log_name);
  OV_FREE(&s->veto_provider_name);
  if (s->mixer_process_names != NULL) {
    size_t const n = OV_ARRAY_LENGTH(s->mixer_process_names);
    for (size_t i = 0; i < n; ++i) {
      OV_FREE(&s->mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s->mixer_process_names);
  }
  OV_FREE(&s->task_name);
  s->veto_event_id = 0;
}

/**
 * @brief The mixer image names the settings carry, read only
 *
 * @param s the settings to read from, may be NULL
 * @return the names as a flat read-only array, NULL when there is none
 */
char const *const *settings_mixer_names(struct settings const *const s) {
  if ((s == NULL) || (s->mixer_process_names == NULL)) {
    return NULL;
  }
  return (char const *const *)(void const *)s->mixer_process_names;
}

/**
 * @brief How many mixer image names the settings carry
 *
 * @param s the settings to read from, may be NULL
 * @return the count, 0 when there is none
 */
size_t settings_mixer_name_count(struct settings const *const s) {
  if ((s == NULL) || (s->mixer_process_names == NULL)) {
    return 0;
  }
  return OV_ARRAY_LENGTH(s->mixer_process_names);
}

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void settings_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_FREE(value); // made by OV_REALLOC inside paths_replace_extension
  }
}
