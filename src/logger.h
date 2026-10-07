#pragma once

#include <ovbase.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * The log lives next to the executable, under the same name with the extension replaced:
 * audient-device-closer.exe -> audient-device-closer.log.  It is the one file this program
 * writes, and it exists so that a failure that only reaches the log leaves a trace at all.
 *
 * One record is one physical line, tab separated, in this order:
 *
 *   timestamp, level, tag, message
 *
 * timestamp is local time with milliseconds, level is one of error/warn/info/verbose, and
 * the message is the last field, so `cut -f4` reaches it directly.  A tab or a newline
 * inside a field is written as the letter t or n behind a backslash: the record stays one
 * line.
 */

/**
 * @brief The sizes the log works with
 *
 * The upper bound for the file: when a write would push it past this, the oldest records
 * are dropped until nothing but the newest LOGGER_KEEP_BYTES are left; when a single
 * record is bigger than the whole file, the file starts over with that record.
 */
enum {
  LOGGER_MAX_BYTES = 100 * 1024,
  LOGGER_KEEP_BYTES = 50 * 1024,
  LOGGER_LINE_BYTES = 4 * 1024,
};

/**
 * @brief Open the log next to the executable and trim it when it is already too big
 *
 * Without this call every write is a no-op, so a caller that does not want a log simply
 * skips it.  A second call is refused: the handle is owned until logger_close.
 *
 * @param err receives the failure of the open
 * @return false when the log could not be opened
 */
bool logger_open(struct ov_error *const err);

/**
 * @brief Make the mutex that serialises the writes
 *
 * Call it once, before any other thread can report; a program without a log skips both.
 */
void logger_init(void);

/**
 * @brief Is the log open and usable
 *
 * @return true when logger_open() succeeded and the log is still usable
 */
bool logger_is_open(void);

/**
 * @brief Write one record at the info level
 *
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param message the record itself, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_write(char const *const tag, char const *const message, struct ov_error *const err);

/**
 * @brief Write one record with a level of its own
 *
 * @param level the level the record carries
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param message the record itself, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_write_level(enum ov_error_severity const level, char const *const tag, char const *const message, struct ov_error *const err);

/**
 * @brief Flush and release the log
 *
 * Safe to call without an open log.
 */
void logger_close(void);

/**
 * @brief The output hook for ov_init()
 *
 * It turns whatever ov_error reports into log records, one record per line of the
 * rendering, tagged with the module the line names.  Pass its address to
 * struct ov_init_options.output_func.
 *
 * @param severity the level the records carry
 * @param str the rendering to write
 */
void logger_output(enum ov_error_severity const severity, char const *const str);

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
                          char const *const message);

/**
 * @brief The level name as it appears in the record
 *
 * @param level the level to name
 * @return a static string
 */
char const *logger_level_name(enum ov_error_severity const level);

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
void logger_error_fields(char const *const line, char *const tag, size_t const tag_size, char *const message, size_t const message_size);

/**
 * @brief Write one record for a message that carries numbers
 *
 * The level is info and the reference is both the reference and the format, the way
 * ov_sprintf_char is called everywhere else in this program: the reference keeps a format
 * that comes from outside, a translation for one, from reading its arguments in a way the
 * caller did not intend.  Without an open log this is a no-op that reports success.  A
 * record that cannot be written goes to err the way logger_write gives it: what a missing
 * record means is the caller's to decide.
 *
 * @param tag the module the record belongs to, UTF-8, must not be NULL
 * @param err receives the failure of the write
 * @param reference the pattern of the message, and its reference
 * @param ... the values the pattern names
 * @return false when the record did not reach the file
 */
bool logger_writef(char const *const tag, struct ov_error *const err, char const *const reference, ...);

/**
 * @brief Write the record that starts a run
 *
 * The records that frame one run of the repair: "=== start ===" and "=== end ===", which
 * say nothing but themselves.  The detail of the steps and the sentence of the run hang
 * between them, so a run with nothing to do still leaves a trace, and a reader who looks for
 * the close of a run finds the same line every time.  Without an open log all of these are
 * no-ops that report success.
 *
 * @param err receives the failure of the write
 * @return false when the record did not reach the file
 */
bool logger_log_run_start(struct ov_error *const err);

/**
 * @brief Write the sentence of a run and the record that ends it
 *
 * sentence says what came of the run in the words of the step that knows it.  It goes out
 * before the fixed "=== end ===", so nothing of the run can reach the log after the record
 * that ends it, and the close of a run always reads the same.
 *
 * @param sentence the sentence of the run, may be NULL when it has nothing to say
 * @param err receives the failure of the write
 * @return false when a record did not reach the file
 */
bool logger_log_run_end(char const *const sentence, struct ov_error *const err);

/**
 * @brief What to do with the file when another record has to fit
 *
 * @param new_size the size the file has now
 * @param add_size how much the next record adds
 * @return 0 keeps everything, -1 starts over with an empty file, and any positive number
 *         is the size the file is cut down to (the kept part always begins at a line
 *         boundary)
 */
long long logger_trim_size(long long const new_size, size_t const add_size);

/**
 * @brief The length of the tail that has to be kept
 *
 * The kept part starts at a line boundary and holds at least LOGGER_KEEP_BYTES.  Works on
 * the raw bytes of the file.
 *
 * @param data the contents of the file
 * @param size how much the file holds
 * @return how many bytes to keep at the end
 */
size_t logger_keep_tail(char const *const data, size_t const size);

/**
 * @brief The path of the log for an executable
 *
 * @param exe_path the executable the log belongs to, UTF-8
 * @param err receives the failure of the build
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with logger_string_free.
 */
char *logger_path_for_exe(char const *const exe_path, struct ov_error *const err);

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void logger_string_free(char **const value);
