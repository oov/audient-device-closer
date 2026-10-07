#include "logger.h"

#include <windows.h>

#include <string.h>
#include <time.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>
#include <ovthreads.h>

#include "loggate.h"
#include "paths.h"

static HANDLE g_file = INVALID_HANDLE_VALUE;
static mtx_t g_lock;
static bool g_lock_ready = false;
static struct loggate g_gate;

/**
 * @brief One line of a rendered error, shaped into the two record fields
 *
 * The decorations of the renderer are dropped: the level marker (the record has a level
 * field) and the error code (the message names the failure).  The tag is the module the
 * line names -- "(at removal.c:15 ...)" names "removal" -- so the errors group by where
 * they happened; "error" is the tag of a line that names no source.  line and message must
 * not overlap.
 *
 * @param line one line of the rendering
 * @param tag receives the module the line names, UTF-8
 * @param tag_size room of tag
 * @param message receives the line without its decorations, UTF-8
 * @param message_size room of message
 */
void logger_error_fields(char const *const line, char *const tag, size_t const tag_size, char *const message, size_t const message_size) {
  static char const *const markers[] = {"[ERROR] ", "[WARN] ", "[INFO] ", "[VERBOSE] "};
  char const *p = (line != NULL) ? line : "";
  size_t o = 0;

  if ((tag == NULL) || (tag_size == 0) || (message == NULL) || (message_size == 0)) {
    return;
  }
  strncpy(tag, "error", tag_size - 1);
  tag[tag_size - 1] = '\0';
  message[0] = '\0';
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  for (size_t i = 0; i < (sizeof(markers) / sizeof(markers[0])); i++) {
    size_t const n = strlen(markers[i]);
    if (strncmp(p, markers[i], n) == 0) {
      p += n;
      break;
    }
  }
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  while ((*p != '\0') && ((o + 1) < message_size)) {
    message[o++] = *p++;
  }
  while ((o > 0) && ((message[o - 1] == ' ') || (message[o - 1] == '\t') || (message[o - 1] == '\r'))) {
    o--;
  }
  message[o] = '\0';
  if (line != NULL) {
    char const *const dot = strstr(line, ".c:");
    if (dot != NULL) {
      char const *begin = dot;
      while ((begin > line) && (((begin[-1] >= 'a') && (begin[-1] <= 'z')) || ((begin[-1] >= 'A') && (begin[-1] <= 'Z')) ||
                                ((begin[-1] >= '0') && (begin[-1] <= '9')) || (begin[-1] == '_'))) {
        begin--;
      }
      if (((size_t)(dot - begin) > 0) && (((size_t)(dot - begin) + 1) < tag_size)) {
        memcpy(tag, begin, (size_t)(dot - begin));
        tag[dot - begin] = '\0';
      }
    }
  }
}

/**
 * @brief The level name as it appears in the record
 *
 * @param level the level to name
 * @return a static string
 */
char const *logger_level_name(enum ov_error_severity const level) {
  switch (level) {
  case ov_error_severity_error:
    return "error";
  case ov_error_severity_warn:
    return "warn";
  case ov_error_severity_info:
    return "info";
  case ov_error_severity_verbose:
    return "verbose";
  }
  return "info";
}

/**
 * @brief Append one field to a record, terminated
 *
 * The two characters that would break the record layout are escaped here.
 *
 * @param out the record to append to
 * @param pos how much of out is written already
 * @param size room of out
 * @param text the field to append
 * @return the new position in out
 */
static size_t append_field(char *const out, size_t const pos, size_t const size, char const *const text) {
  size_t p = pos;

  for (char const *s = text; *s != '\0'; s++) {
    if ((p + 2) >= size) {
      break;
    }
    if (*s == '\t') {
      out[p++] = '\\';
      out[p++] = 't';
    } else if ((*s == '\r') || (*s == '\n')) {
      out[p++] = '\\';
      out[p++] = 'n';
    } else {
      out[p++] = *s;
    }
  }
  return p;
}

/**
 * @brief Format one record into a buffer
 *
 * @param out receives the record, size bytes
 * @param size room of out
 * @param time_ms_utc the moment of the record, in epoch milliseconds UTC
 * @param level the level the record carries
 * @param tag the module the record belongs to
 * @param message the record itself
 * @return how many bytes were written without the terminating NUL, or 0 when the line does
 *         not fit into size at all
 */
size_t logger_format_line(char *const out,
                          size_t const size,
                          long long const time_ms_utc,
                          enum ov_error_severity const level,
                          char const *const tag,
                          char const *const message) {
  char stamp[32];
  struct tm local;
  time_t seconds = 0;
  int head = 0;
  size_t p = 0;

  if ((out == NULL) || (size < 2) || (tag == NULL) || (message == NULL)) {
    return 0;
  }
  out[0] = '\0';
  seconds = (time_t)(time_ms_utc / 1000);
  memset(&local, 0, sizeof(local));
  if (localtime_s(&local, &seconds) != 0) {
    memset(&local, 0, sizeof(local));
  }
  if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local) == 0) {
    memcpy(stamp, "0000-00-00 00:00:00", sizeof("0000-00-00 00:00:00"));
  }
  head = OV_SNPRINTF(out, size, NULL, "%s.%03d", stamp, (int)(time_ms_utc % 1000));
  if ((head < 0) || ((size_t)head >= size)) {
    return 0;
  }
  p = (size_t)head;
  if ((p + 1) >= size) {
    return 0;
  }
  out[p++] = '\t';
  p = append_field(out, p, size, logger_level_name(level));
  if ((p + 1) >= size) {
    return 0;
  }
  out[p++] = '\t';
  p = append_field(out, p, size, tag);
  if ((p + 1) >= size) {
    return 0;
  }
  out[p++] = '\t';
  p = append_field(out, p, size, message);
  out[p] = '\0';
  return p;
}

/**
 * @brief How much of the file has to go so that the new record fits
 *
 * @param new_size the size the file has now
 * @param add_size how much the next record adds
 * @return how many bytes to drop from the front, 0 when nothing has to go, -1 when the
 *         record alone is bigger than the file may become
 */
long long logger_trim_size(long long const new_size, size_t const add_size) {
  if (new_size <= 0) {
    return 0; // nothing on disk yet, nothing to trim
  }
  if ((long long)add_size >= LOGGER_MAX_BYTES) {
    return -1; // the record alone is bigger than the file may become
  }
  if ((new_size + (long long)add_size) <= LOGGER_MAX_BYTES) {
    return 0; // it still fits
  }
  return -1; // it does not fit any more, so only the newest records are kept
}

/**
 * @brief Where the kept records of a read file start
 *
 * @param data the contents of the file
 * @param size how much the file holds
 * @return the offset of the first record to keep, the tail alone when nothing fits
 */
size_t logger_keep_tail(char const *const data, size_t const size) {
  static size_t const keep_hint = LOGGER_KEEP_BYTES;
  size_t const begin = (size > keep_hint) ? (size - keep_hint) : 0;
  size_t start = begin;

  if ((data == NULL) || (size == 0)) {
    return 0;
  }
  while ((start < size) && (data[start] != '\n')) {
    start++;
  }
  if (start >= size) {
    return 0;
  }
  return size - (start + 1);
}

/**
 * @brief The moment now, in epoch milliseconds UTC
 *
 * @return the moment in epoch milliseconds UTC
 */
static long long now_ms(void) {
  FILETIME ft;
  ULARGE_INTEGER u;

  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void logger_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_FREE(value); // made by OV_REALLOC inside paths_replace_extension
  }
}

/**
 * @brief The log an executable writes next to itself
 *
 * The name is the name of the executable with its extension replaced, which is the same
 * computation the settings file goes through: see paths_replace_extension.
 *
 * @param exe_path the executable the log belongs to, UTF-8
 * @param err receives the failure of the build
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with logger_string_free.
 */
char *logger_path_for_exe(char const *const exe_path, struct ov_error *const err) { return paths_replace_extension(exe_path, ".log", err); }

/**
 * @brief Read a file from its current position
 *
 * An empty file is an empty buffer, not an error.
 *
 * @param h the file to read
 * @param out receives the contents
 * @param out_size receives how much was read
 * @param err receives the failure of the read
 * @return false when the file could not be read
 */
static bool read_all(HANDLE const h, char **const out, size_t *const out_size, struct ov_error *const err) {
  bool success = false;
  LARGE_INTEGER size;
  DWORD got = 0;
  char *buf = NULL;

  *out = NULL;
  *out_size = 0;
  if (!GetFileSizeEx(h, &size)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if ((size.QuadPart <= 0) || (size.QuadPart > LOGGER_MAX_BYTES)) {
    success = true; // empty, or bigger than anything this program writes: start over
    goto cleanup;
  }
  if (!OV_REALLOC(&buf, (size_t)size.QuadPart, sizeof(buf[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || (got != (DWORD)size.QuadPart)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  *out = buf;
  *out_size = (size_t)got;
  buf = NULL;
  success = true;

cleanup:
  OV_FREE(&buf);
  return success;
}

/**
 * @brief Drop the oldest records until only the newest ones are left
 *
 * The file is rewritten in place, so no rename and no second file appear next to the log.
 *
 * @param h the file to trim
 * @param err receives the failure of the rewrite
 * @return false when the file could not be trimmed
 */
static bool trim(HANDLE const h, struct ov_error *const err) {
  bool success = false;
  char *data = NULL;
  size_t size = 0;
  size_t keep = 0;
  LARGE_INTEGER zero;
  DWORD written = 0;

  if (!read_all(h, &data, &size, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (size == 0) {
    success = true;
    goto cleanup;
  }
  keep = logger_keep_tail(data, size);
  zero.QuadPart = 0;
  if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (keep > 0) {
    if (!WriteFile(h, &data[size - keep], (DWORD)keep, &written, NULL) || (written != (DWORD)keep)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  if (!SetEndOfFile(h)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  OV_FREE(&data);
  if (success) {
    SetFilePointer(h, 0, NULL, FILE_END);
  }
  return success;
}

/**
 * @brief Write one record to the file
 *
 * @param h the file to append to
 * @param line the record, written with its newline
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
static bool write_line(HANDLE const h, char const *const line, struct ov_error *const err) {
  DWORD written = 0;
  size_t const len = strlen(line);

  if (!WriteFile(h, line, (DWORD)len, &written, NULL) || (written != (DWORD)len)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (!WriteFile(h, "\n", 1, &written, NULL) || (written != 1)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  FlushFileBuffers(h); // this file is for reading after something went wrong
  return true;
}

/**
 * @brief Write the note that tells a reader of the file that its beginning was dropped
 *
 * A failure here is not worth an error of its own: the log is already short of records.
 *
 * @param h the file to append to
 */
static void write_trim_note(HANDLE const h) {
  char note[128];
  struct ov_error err = {0};

  if (OV_SNPRINTF(note, sizeof(note), NULL, "the log exceeded %d bytes, the oldest records were dropped", LOGGER_MAX_BYTES) > 0) {
    if (!write_line(h, note, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  }
}

/**
 * @brief Open the log next to the executable and trim it when it is already too big
 *
 * Without this call every write is a no-op, so a caller that does not want a log simply
 * skips it.  A second call is refused: the handle is owned until logger_close.
 *
 * @param err receives the failure of the open
 * @return false when the log could not be opened
 */
bool logger_open(struct ov_error *const err) {
  bool success = false;
  char *exe = NULL;
  char *path = NULL;
  wchar_t wide[PATHS_MAX] = {0};
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  LARGE_INTEGER zero;

  if (logger_is_open()) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  path = logger_path_for_exe(exe, err);
  if (path == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    int const n = OV_SNPRINTF(wide, sizeof(wide) / sizeof(wide[0]), NULL, L"%s", path);
    if ((n < 0) || ((size_t)n >= (sizeof(wide) / sizeof(wide[0])))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      goto cleanup;
    }
  }
  h = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!GetFileSizeEx(h, &size)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (size.QuadPart > LOGGER_MAX_BYTES) {
    if (!trim(h, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    write_trim_note(h);
  }
  zero.QuadPart = 0;
  if (!SetFilePointerEx(h, zero, NULL, FILE_END)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  g_file = h;
  h = INVALID_HANDLE_VALUE;
  success = true;

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  if (exe != NULL) {
    OV_ARRAY_DESTROY(&exe); // made by ov_sprintf_char
  }
  logger_string_free(&path);
  return success;
}

/**
 * @brief Is the log open and usable
 *
 * @return true when logger_open() succeeded and the log is still usable
 */
bool logger_is_open(void) { return g_file != INVALID_HANDLE_VALUE; }

/**
 * @brief Write one record with a level of its own
 *
 * @param level the level the record carries
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param message the record itself, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_write_level(enum ov_error_severity const level, char const *const tag, char const *const message, struct ov_error *const err) {
  bool success = false;
  char line[LOGGER_LINE_BYTES];
  LARGE_INTEGER size;
  long long trim_to = 0;

  if ((tag == NULL) || (message == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!logger_is_open()) {
    return true; // no log wanted: reporting is not a failure
  }
  if (logger_format_line(line, sizeof(line), now_ms(), level, tag, message) == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return false;
  }
  if (g_lock_ready) {
    mtx_lock(&g_lock);
  }
  if (!GetFileSizeEx(g_file, &size)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup_unlock;
  }
  trim_to = logger_trim_size(size.QuadPart, strlen(line) + 1);
  if (trim_to < 0) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(g_file, zero, NULL, FILE_BEGIN) || !SetEndOfFile(g_file)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup_unlock;
    }
  } else if (trim_to > 0) {
    if (!trim(g_file, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup_unlock;
    }
  }
  if (!write_line(g_file, line, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup_unlock;
  }
  success = true;

cleanup_unlock:
  if (g_lock_ready) {
    mtx_unlock(&g_lock);
  }
  return success;
}

/**
 * @brief Write one record at the info level
 *
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param message the record itself, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_write(char const *const tag, char const *const message, struct ov_error *const err) {
  return logger_write_level(ov_error_severity_info, tag, message, err);
}

/**
 * @brief Write one record for a message that carries numbers
 *
 * Without an open log this is a no-op that reports success.  A record that cannot be
 * written goes to err the way logger_write gives it: what a missing record means is the
 * caller's to decide.
 *
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @param reference the pattern of the message, and the reference that keeps a format of
 *                  its own from reading its arguments the wrong way
 * @param ... the values the pattern names
 * @return false when the record did not reach the file
 */
bool logger_writef(char const *const tag, struct ov_error *const err, char const *const reference, ...) {
  char message[LOGGER_LINE_BYTES];
  va_list ap;
  int n = -1;

  if (!logger_is_open()) {
    return true; // no log wanted: reporting is not a failure
  }
  va_start(ap, reference);
  n = OV_VSNPRINTF(message, sizeof(message), reference, reference, ap);
  va_end(ap);
  if (n < 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return logger_write(tag, message, err);
}

/**
 * @brief Write the record that starts a run
 *
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_log_run_start(struct ov_error *const err) { return logger_write("run", "=== start ===", err); }

/**
 * @brief Write the sentence of a run and the record that ends it
 *
 * The sentence comes first and the fixed close follows it, so a run that has nothing to say
 * still ends where the reader looks for it, and nothing of the run can reach the log after
 * the record that ends it.
 *
 * @param sentence the sentence of the run, may be NULL when it has nothing to say
 * @param err receives the failure of the write
 * @return false when a record did not reach the file
 */
bool logger_log_run_end(char const *const sentence, struct ov_error *const err) {
  if ((sentence != NULL) && (sentence[0] != '\0')) {
    if (!logger_write("run", sentence, err)) {
      return false;
    }
  }
  return logger_write("run", "=== end ===", err);
}

/**
 * @brief Flush and release the log
 *
 * Safe to call without an open log.
 */
void logger_close(void) {
  if (g_file != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(g_file);
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  loggate_release(&g_gate); // the next open is a new reader of the file
  if (g_lock_ready) {
    mtx_destroy(&g_lock);
    g_lock_ready = false;
  }
}

/**
 * @brief The output hook of ovbase: one record per line of a rendered error
 *
 * A rendering of an ov_error arrives as several lines: the header, the position and the
 * message.  Each one becomes a record of its own, which keeps every line parseable.
 * Whether the same report is worth writing again is decided here, before the rendering is
 * written out: it arrives as several records, so a check per record would not recognise a
 * block that repeats.
 */
void logger_output(enum ov_error_severity const severity, char const *const str) {
  char const *start = NULL;
  struct ov_error err = {0};
  char one[LOGGER_LINE_BYTES];
  char tag[32];
  char message[LOGGER_LINE_BYTES];

  if (str == NULL) {
    return;
  }
  if (!loggate_should_emit(&g_gate, str)) {
    return; // the same report as the one before: a standing failure repeats itself
  }
  start = str;
  for (char const *p = str;; p++) {
    if ((*p == '\n') || (*p == '\0')) {
      size_t const len = (size_t)(p - start);
      size_t const n = (len < (sizeof(one) - 1)) ? len : (sizeof(one) - 1);
      if (n > 0) {
        memcpy(one, start, n);
        one[n] = '\0';
        logger_error_fields(one, tag, sizeof(tag), message, sizeof(message));
        if (message[0] != '\0') {
          if (!logger_write_level(severity, tag, message, &err)) {
            OV_ERROR_REPORT(&err, NULL);
          }
        }
      }
      if (*p == '\0') {
        break;
      }
      start = p + 1;
    }
  }
}

/**
 * @brief Make the mutex that serialises the writes
 *
 * The thread that runs the repair and the window thread both report here, so the mutex is
 * made while the program still has one thread.  logger_open() is called from main before
 * the window exists, and a failure to make the mutex only costs the serialisation.
 */
void logger_init(void) {
  if (!g_lock_ready) {
    if (mtx_init(&g_lock, mtx_plain) == thrd_success) {
      g_lock_ready = true;
    }
  }
}
