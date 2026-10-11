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
#include "process.h"

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
static void
logger_error_fields(char const *const line, char *const tag, size_t const tag_size, char *const message, size_t const message_size) {
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
static char const *logger_level_name(enum ov_error_severity const level) {
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
static size_t logger_format_line(char *const out,
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
static long long logger_trim_size(long long const new_size, size_t const add_size) {
  if (new_size <= 0) {
    return 0; // nothing on disk yet, nothing to trim
  }
  if ((long long)add_size >= LOGGER_MAX_BYTES) {
    return -1; // the record alone is bigger than the file may become
  }
  if ((new_size + (long long)add_size) <= LOGGER_MAX_BYTES) {
    return 0; // it still fits
  }
  return LOGGER_KEEP_BYTES; // it does not fit any more: cut down to the newest records
}

/**
 * @brief Is the log open and usable
 *
 * @return true when logger_open() succeeded and the log is still usable
 */
static bool logger_is_open(void) { return g_file != INVALID_HANDLE_VALUE; }

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
static void logger_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_FREE(value); // made by OV_REALLOC inside paths_replace_extension
  }
}

/**
 * @brief Make the folder an installed copy writes its log into
 *
 * The name of the log is built without touching the disk, so the machine-wide folder is made
 * here, at the moment the log is opened.  A portable copy needs nothing: its folder is the one
 * the executable already sits in.  Without the folder the open cannot succeed either, so a
 * failure here is the failure of the open.
 *
 * @param exe_path the executable the log belongs to, UTF-8
 * @param err receives the failure of the folder
 * @return false when the folder could not be made
 */
static bool logger_make_folder(char const *const exe_path, struct ov_error *const err) {
  char *folder = NULL;
  bool success = false;

  if (!paths_is_in_program_files(exe_path)) {
    success = true; // a portable copy writes next to itself, its folder exists already
    goto cleanup;
  }
  folder = paths_log_folder(err);
  if (folder == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!paths_create_folder(folder, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  paths_string_free(&folder);
  return success;
}

/**
 * @brief The log an executable writes next to itself
 *
 * The name is the name of the executable with its extension replaced, which is the same
 * computation the settings file goes through: see paths_replace_extension.  An installed copy
 * keeps its log under its own name in the folder paths_log_folder gives, because that copy is
 * run both by the person at the screen and by the task scheduler.
 *
 * @param exe_path the executable the log belongs to, UTF-8
 * @param err receives the failure of the build
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with logger_string_free.
 */
static char *logger_path_for_exe(char const *const exe_path, struct ov_error *const err) {
  char *folder = NULL;
  char *joined = NULL;
  char *result = NULL;
  size_t len = 0;
  bool success = false;

  if (exe_path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!paths_is_in_program_files(exe_path)) {
    result = paths_replace_extension(exe_path, ".log", err); // a portable copy keeps its log next to itself
    if (result == NULL) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    success = true;
    goto cleanup;
  }
  folder = paths_log_folder(err);
  if (folder == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  joined = paths_join(folder, "audient-device-closer.log", err);
  if (joined == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  // the caller releases the result with logger_string_free, which frees a block of
  // OV_REALLOC, so the joined string of ov_sprintf_char is copied into one
  len = strlen(joined) + 1;
  if (!OV_REALLOC(&result, len, sizeof(result[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memcpy(result, joined, len);
  success = true;

cleanup:
  paths_string_free(&folder);
  if (joined != NULL) {
    OV_ARRAY_DESTROY(&joined); // made by paths_join
  }
  if (!success) {
    if (result != NULL) {
      OV_FREE(&result);
    }
  }
  return success ? result : NULL;
}

/**
 * @brief Cut the file down to the given size
 *
 * The file is rewritten in place, so no rename and no second file appear next to the log.
 * The newest records survive: the tail the size names is read, cut to the first line
 * boundary it holds and written over the front of the file.
 *
 * @param h the file to trim
 * @param keep_size the size the file is cut down to
 * @param err receives the failure of the rewrite
 * @return false when the file could not be trimmed
 */
static bool trim(HANDLE const h, long long const keep_size, struct ov_error *const err) {
  bool success = false;
  char *buf = NULL;
  LARGE_INTEGER size;
  LARGE_INTEGER start;
  DWORD got = 0;
  size_t keep = 0;
  DWORD written = 0;

  if (!GetFileSizeEx(h, &size)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if ((size.QuadPart <= 0) || (keep_size <= 0)) {
    success = true;
    goto cleanup;
  }
  start.QuadPart = (size.QuadPart > keep_size) ? (size.QuadPart - keep_size) : 0;
  if (!SetFilePointerEx(h, start, NULL, FILE_BEGIN)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!OV_REALLOC(&buf, (size_t)(size.QuadPart - start.QuadPart), sizeof(buf[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!ReadFile(h, buf, (DWORD)(size.QuadPart - start.QuadPart), &got, NULL) || ((long long)got != (size.QuadPart - start.QuadPart))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // the kept part begins after the first newline of the tail: the record the tail was cut
  // out of is incomplete, and an incomplete record would read as another one
  keep = 0;
  while ((keep < (size_t)got) && (buf[keep] != '\n')) {
    keep++;
  }
  if (keep >= (size_t)got) {
    success = true; // no line boundary in the tail: nothing readable is left to keep
    goto cleanup;
  }
  keep++; // the newline itself belongs to the record in front of the kept part
  {
    LARGE_INTEGER front;
    front.QuadPart = 0;
    if (!SetFilePointerEx(h, front, NULL, FILE_BEGIN)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  if (!WriteFile(h, &buf[keep], (DWORD)(got - keep), &written, NULL) || (written != (got - keep))) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!SetEndOfFile(h)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  OV_FREE(&buf);
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
 * @param h the file to append to
 * @param err receives the failure of the write
 * @return false when the note did not reach the file
 */
static bool write_trim_note(HANDLE const h, struct ov_error *const err) {
  char note[128];

  if (OV_SNPRINTF(note, sizeof(note), NULL, "the log exceeded %d bytes, the oldest records were dropped", LOGGER_MAX_BYTES) > 0) {
    if (!write_line(h, note, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
  }
  return true;
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
  if (!logger_make_folder(exe, err)) {
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
    if (!trim(h, LOGGER_KEEP_BYTES, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!write_trim_note(h, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
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
 * @brief Write one record with a level of its own
 *
 * @param level the level the record carries
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param message the record itself, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
static bool
logger_write_level(enum ov_error_severity const level, char const *const tag, char const *const message, struct ov_error *const err) {
  bool success = false;
  bool locked = false;
  char line[LOGGER_LINE_BYTES];
  LARGE_INTEGER size;
  long long trim_to = 0;

  if ((tag == NULL) || (message == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!logger_is_open()) {
    success = true; // no log wanted: reporting is not a failure
    goto cleanup;
  }
  if (logger_format_line(line, sizeof(line), process_now_ms(), level, tag, message) == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }
  if (g_lock_ready) {
    mtx_lock(&g_lock);
    locked = true;
  }
  if (!GetFileSizeEx(g_file, &size)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  trim_to = logger_trim_size(size.QuadPart, strlen(line) + 1);
  if (trim_to < 0) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(g_file, zero, NULL, FILE_BEGIN) || !SetEndOfFile(g_file)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  } else if (trim_to > 0) {
    if (!trim(g_file, trim_to, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  if (!write_line(g_file, line, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (locked) {
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
 * @brief Write one record for a message that carries numbers, reporting a failure itself
 *
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param reference the pattern of the message, and its reference
 * @param ... the values the pattern names
 */
void logger_notef(char const *const tag, char const *const reference, ...) {
  char message[LOGGER_LINE_BYTES];
  struct ov_error err = {0};
  va_list ap;
  int n = -1;

  if (!logger_is_open()) {
    return; // no log wanted: reporting is not a failure
  }
  va_start(ap, reference);
  n = OV_VSNPRINTF(message, sizeof(message), reference, reference, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if (!logger_write(tag, message, &err)) {
    OV_ERROR_REPORT(&err, NULL);
  }
}

/**
 * @brief Write the record that starts a run
 *
 * The records that frame a run are read by a person who looks the run up afterwards, so a
 * failure to write one is reported where it stands and nowhere else: a caller that has to
 * react to a missing record uses logger_write.
 */
void logger_log_run_start(void) { logger_notef("run", "=== start ==="); }

/**
 * @brief Write the sentence of a run and the record that ends it
 *
 * The sentence comes first and the fixed close follows it, so a run that has nothing to say
 * still ends where the reader looks for it, and nothing of the run can reach the log after
 * the record that ends it.
 *
 * @param sentence the sentence of the run, may be NULL when it has nothing to say
 */
void logger_log_run_end(char const *const sentence) {
  if ((sentence != NULL) && (sentence[0] != '\0')) {
    logger_notef("run", "%hs", sentence);
  }
  logger_notef("run", "=== end ===");
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
