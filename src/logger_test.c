/**
 * Tests for the log: the record format, the trim decisions and the real file next to the
 * executable.  Nothing here reads a file it did not write itself.
 */
#include <ovtest.h>

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "logger.h"
#include "paths.h"

/**
 * @brief The record is tab separated with the message last
 *
 * The message is the last field, so cut -f4 reaches it directly.
 */
static void test_format_line(void) {
  char line[512];
  size_t len = 0;
  char const *fields[4];
  size_t field_count = 0;

  len = logger_format_line(line, sizeof(line), 1786000138123LL, ov_error_severity_error, "repair", "the removal was vetoed");
  TEST_CHECK(len > 0);
  TEST_CHECK(len == strlen(line));

  {
    char copy[512];
    char *p = NULL;
    memcpy(copy, line, len + 1);
    for (p = strtok(copy, "\t"); p != NULL && field_count < 4; p = strtok(NULL, "\t")) {
      fields[field_count++] = p;
    }
  }
  if (!TEST_CHECK(field_count == 4)) {
    TEST_MSG("want 4 fields, got %zu", field_count);
    return;
  }
  TEST_CHECK(strlen(fields[0]) == (sizeof("YYYY-MM-DD HH:MM:SS.mmm") - 1));
  TEST_CHECK(fields[0][4] == '-');
  TEST_CHECK(fields[0][7] == '-');
  TEST_CHECK(fields[0][10] == ' ');
  TEST_CHECK(fields[0][13] == ':');
  TEST_CHECK(fields[0][16] == ':');
  TEST_CHECK(fields[0][19] == '.');
  TEST_CHECK(strcmp(fields[1], "error") == 0);
  TEST_CHECK(strcmp(fields[2], "repair") == 0);
  TEST_CHECK(strcmp(fields[3], "the removal was vetoed") == 0);

  TEST_CHECK(strcmp(logger_level_name(ov_error_severity_error), "error") == 0);
  TEST_CHECK(strcmp(logger_level_name(ov_error_severity_warn), "warn") == 0);
  TEST_CHECK(strcmp(logger_level_name(ov_error_severity_info), "info") == 0);
  TEST_CHECK(strcmp(logger_level_name(ov_error_severity_verbose), "verbose") == 0);
}

/**
 * @brief A tab or a newline in a field must not break the record layout
 */
static void test_format_line_escapes(void) {
  char line[512];
  static char const *const message = "first\tsecond\nthird";

  TEST_CHECK(logger_format_line(line, sizeof(line), 0, ov_error_severity_info, "tag\twith\ttabs", message) > 0);
  TEST_CHECK(strchr(line, '\n') == NULL);
  TEST_CHECK(strstr(line, "tag\\twith\\ttabs") != NULL);
  TEST_CHECK(strstr(line, "first\\tsecond\\nthird") != NULL);
  {
    size_t count = 0;
    for (char const *p = line; *p != '\0'; p++) {
      if (*p == '\t') {
        count++;
      }
    }
    TEST_CHECK(count == 3);
  }
}

/**
 * @brief A record that does not fit the buffer is not written half way
 */
static void test_format_line_bounds(void) {
  char small[16];
  char one[2];
  char big[4096];

  TEST_CHECK(logger_format_line(NULL, 64, 0, ov_error_severity_info, "t", "m") == 0);
  TEST_CHECK(logger_format_line(small, 0, 0, ov_error_severity_info, "t", "m") == 0);
  TEST_CHECK(logger_format_line(one, sizeof(one), 0, ov_error_severity_info, "t", "m") == 0);
  TEST_CHECK(logger_format_line(small, sizeof(small), 0, ov_error_severity_info, "t", "m") == 0);
  TEST_CHECK(logger_format_line(small, 8, 0, ov_error_severity_info, "tag", "message") == 0);
  TEST_CHECK(logger_format_line(big, sizeof(big), 0, ov_error_severity_info, NULL, "m") == 0);
  TEST_CHECK(logger_format_line(big, sizeof(big), 0, ov_error_severity_info, "t", NULL) == 0);
}

/**
 * @brief The sizes the log works with
 *
 * The cap the file may reach is what the compaction is built around: the tail a trim keeps
 * and one record both have to fit under it.  The wanted numbers are variables rather than
 * constants, because a comparison of an enum constant with a literal the compiler can bound
 * is refused as a tautology.
 */
static void test_sizes(void) {
  int want_max_bytes = 100 * 1024;
  int want_keep_bytes = want_max_bytes / 2;

  TEST_CHECK(LOGGER_MAX_BYTES == want_max_bytes);
  TEST_MSG("want %d bytes, got %d", want_max_bytes, (int)LOGGER_MAX_BYTES);

  TEST_CHECK(LOGGER_KEEP_BYTES == want_keep_bytes);
  TEST_MSG("want %d bytes, got %d", want_keep_bytes, (int)LOGGER_KEEP_BYTES);

  TEST_CHECK(LOGGER_KEEP_BYTES < LOGGER_MAX_BYTES);
  TEST_CHECK(LOGGER_LINE_BYTES <= LOGGER_MAX_BYTES);
}

/**
 * @brief The trim decision
 *
 * Everything while it fits, start over when the record alone is too big, otherwise drop
 * the oldest records.
 */
static void test_trim_size(void) {
  TEST_CHECK(logger_trim_size(0, 10) == 0);                      // nothing on disk
  TEST_CHECK(logger_trim_size(LOGGER_MAX_BYTES - 10, 10) == 0);  // exactly full
  TEST_CHECK(logger_trim_size(LOGGER_MAX_BYTES - 10, 11) == -1); // one byte too much
  TEST_CHECK(logger_trim_size(LOGGER_MAX_BYTES * 2, 10) == -1);  // already over
  TEST_CHECK(logger_trim_size(100, LOGGER_MAX_BYTES) == -1);     // the record alone is too big
  TEST_CHECK(logger_trim_size(-5, 10) == 0);                     // a size that cannot be real
}

/**
 * @brief The kept tail starts at a line boundary and never reaches further back than it
 *        has to
 */
static void test_keep_tail(void) {
  char small[32];
  char *big = NULL;
  size_t keep = 0;
  static size_t const total = LOGGER_KEEP_BYTES + 64;

  TEST_CHECK(logger_keep_tail(NULL, 10) == 0);
  TEST_CHECK(logger_keep_tail(small, 0) == 0);
  memcpy(small, "one\ntwo\n", 9);
  TEST_CHECK(logger_keep_tail(small, 9) == 5);
  TEST_CHECK(logger_keep_tail(small, 4) == 0); // no boundary inside: nothing can be kept
  TEST_CHECK(logger_keep_tail(small, 2) == 0);

  if (!TEST_CHECK(OV_REALLOC(&big, total, sizeof(big[0])))) {
    return;
  }
  memset(big, 'x', total);
  big[10] = '\n';
  big[LOGGER_KEEP_BYTES - 5] = '\n';
  big[LOGGER_KEEP_BYTES + 5] = '\n';
  big[total - 1] = '\n';
  keep = logger_keep_tail(big, total);
  TEST_CHECK(keep > 0);
  TEST_CHECK(keep <= total);
  TEST_CHECK(big[total - keep - 1] == '\n'); // the kept part starts after a boundary
  TEST_CHECK(keep <= LOGGER_KEEP_BYTES);     // and it is the shorter part of the file

  memset(big, 'y', total);
  TEST_CHECK(logger_keep_tail(big, total) == 0);
  OV_FREE(&big);
}

/**
 * @brief The log path: next to the executable, same name with the extension replaced
 */
static void test_path_for_exe(void) {
  struct ov_error err = {0};
  char *p = NULL;

  p = logger_path_for_exe("C:\\dir\\audient-device-closer.exe", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\dir\\audient-device-closer.log") == 0);
    logger_string_free(&p);
  }
  p = logger_path_for_exe("C:\\dir\\my.tool\\app", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\dir\\my.tool\\app.log") == 0);
    logger_string_free(&p);
  }
  p = logger_path_for_exe("C:\\a.b.c\\d.e.exe", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "C:\\a.b.c\\d.e.log") == 0);
    logger_string_free(&p);
  }
  p = logger_path_for_exe("relative.exe", &err);
  if (TEST_CHECK(p != NULL)) {
    TEST_CHECK(strcmp(p, "relative.log") == 0);
    logger_string_free(&p);
  }
  TEST_FAILED_WITH(logger_path_for_exe(NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
}

/**
 * @brief The record on disk is one line, and the file is written where the executable sits
 */
static void test_write_to_file(void) {
  struct ov_error err = {0};
  char *exe = NULL;
  char *path = NULL;
  wchar_t *wide = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  char *text = NULL;
  DWORD got = 0;

  if (logger_is_open()) {
    logger_close();
  }
  if (!TEST_CHECK(logger_open(&err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(logger_is_open() == true);
  if (!TEST_CHECK(logger_write("paths", "hello\tworld", &err) == true)) {
    goto cleanup;
  }
  {
    bool const second = logger_open(&err);
    TEST_FAILED_WITH(second, &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  }
  logger_close();
  TEST_CHECK(logger_is_open() == false);
  TEST_CHECK(logger_write("paths", "after close", &err) == true);

  exe = paths_executable(&err);
  if (!TEST_CHECK(exe != NULL)) {
    goto cleanup;
  }
  path = logger_path_for_exe(exe, &err);
  if (!TEST_CHECK(path != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(ov_sprintf_wchar(&wide, &err, NULL, L"%s", path) == true)) {
    goto cleanup;
  }
  h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (!TEST_CHECK(h != INVALID_HANDLE_VALUE)) {
    goto cleanup;
  }
  if (!TEST_CHECK(GetFileSizeEx(h, &size) && (size.QuadPart > 0) && (size.QuadPart < 1 << 20))) {
    goto cleanup;
  }
  if (!TEST_CHECK(OV_REALLOC(&text, (size_t)size.QuadPart + 1, sizeof(text[0])))) {
    goto cleanup;
  }
  if (!TEST_CHECK(ReadFile(h, text, (DWORD)size.QuadPart, &got, NULL) && (got == (DWORD)size.QuadPart))) {
    goto cleanup;
  }
  text[got] = '\0';
  TEST_CHECK(strstr(text, "\tpaths\thello\\tworld\n") != NULL);
  TEST_CHECK(strstr(text, "after close") == NULL); // the closed log took nothing
  TEST_CHECK(strstr(path, "test_logger.log") != NULL);

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  logger_close();
  if (text != NULL) {
    OV_FREE(&text);
  }
  if (wide != NULL) {
    OV_ARRAY_DESTROY(&wide);
  }
  paths_string_free(&exe);
  logger_string_free(&path);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief The log next to the executable, read back
 *
 * For the tests that check what a run leaves in it.
 *
 * @param out_text receives the contents of the file
 * @param err receives the failure of the read
 * @return false when the file could not be read
 * @note Release the text with OV_FREE.
 */
static bool read_log_file(char **const out_text, struct ov_error *const err) {
  bool ok = false;
  char *exe = NULL;
  char *path = NULL;
  wchar_t *wide = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  char *text = NULL;
  DWORD got = 0;

  exe = paths_executable(err);
  if (!TEST_CHECK(exe != NULL)) {
    goto cleanup;
  }
  path = logger_path_for_exe(exe, err);
  if (!TEST_CHECK(path != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(ov_sprintf_wchar(&wide, err, NULL, L"%s", path) == true)) {
    goto cleanup;
  }
  h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (!TEST_CHECK(h != INVALID_HANDLE_VALUE)) {
    goto cleanup;
  }
  if (!TEST_CHECK(GetFileSizeEx(h, &size) && (size.QuadPart > 0) && (size.QuadPart < 1 << 20))) {
    goto cleanup;
  }
  if (!TEST_CHECK(OV_REALLOC(&text, (size_t)size.QuadPart + 1, sizeof(text[0])))) {
    goto cleanup;
  }
  if (!TEST_CHECK(ReadFile(h, text, (DWORD)size.QuadPart, &got, NULL) && (got == (DWORD)size.QuadPart))) {
    goto cleanup;
  }
  text[got] = '\0';
  *out_text = text;
  text = NULL;
  ok = true;

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  if (text != NULL) {
    OV_FREE(&text);
  }
  if (wide != NULL) {
    OV_ARRAY_DESTROY(&wide);
  }
  paths_string_free(&exe);
  logger_string_free(&path);
  return ok;
}

/**
 * @brief The records that frame a run
 *
 * The one that starts it and the one that ends it say nothing but start and end, so the pair
 * of them is found at a glance whatever came of the run.  What came of it is one sentence of
 * its own before the close: the defect this test exists for was a sentence that reached the
 * log after the close of its run.
 */
static void test_run_records(void) {
  struct ov_error err = {0};
  char *text = NULL;
  static char const want_sentence[] = "\trun\tThe device could not be released.\n";
  static char const want_close[] = "\trun\t=== end ===\n";

  if (logger_is_open()) {
    logger_close();
  }
  if (!TEST_CHECK(logger_open(&err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(logger_log_run_start(&err) == true);
  TEST_CHECK(logger_log_run_end(NULL, &err) == true);
  TEST_CHECK(logger_log_run_end("No Audient device was found.", &err) == true);
  TEST_CHECK(logger_log_run_end("The device could not be released.", &err) == true);
  logger_close();

  if (!TEST_CHECK(read_log_file(&text, &err))) {
    goto cleanup;
  }
  TEST_CASE("the records that frame the run always say the same");
  TEST_CHECK(strstr(text, "\trun\t=== start ===\n") != NULL);
  TEST_CHECK(strstr(text, want_close) != NULL);

  TEST_CASE("the sentence of the run is written as it stands");
  TEST_CHECK(strstr(text, "\trun\tNo Audient device was found.\n") != NULL);
  TEST_CHECK(strstr(text, want_sentence) != NULL);

  TEST_CASE("a close with no sentence writes an empty record");
  TEST_CHECK(strstr(text, "\trun\t\n") == NULL);

  TEST_CASE("the record that ends the run is the last one");
  {
    static size_t const n = sizeof(want_close) - 1;
    size_t const len = strlen(text);
    if (TEST_CHECK(len >= n)) {
      TEST_CHECK(strcmp(text + (len - n), want_close) == 0);
      TEST_MSG("want the log to end with [%hs], got [%hs]", want_close, text + (len - n));
    }
  }

  TEST_CASE("the sentence is the record right before the close");
  {
    static size_t const n = sizeof(want_close) - 1;
    char const *after = strstr(text, want_sentence);
    size_t newlines = 0;

    if (!TEST_CHECK(after != NULL)) {
      goto cleanup;
    }
    after += sizeof(want_sentence) - 1; /* the record the sentence is followed by */
    for (char const *p = after; *p != '\0'; ++p) {
      newlines += (*p == '\n') ? 1u : 0u;
    }
    TEST_CHECK(newlines == 1u);
    TEST_MSG("want the close to be the only record after the sentence, got %u", (unsigned)newlines);
    if (TEST_CHECK(strlen(after) >= n)) {
      TEST_CHECK(strcmp(after + strlen(after) - n, want_close) == 0);
      TEST_MSG("want [%hs], got [%hs]", want_close, after + strlen(after) - n);
    }
  }
  TEST_CASE_(NULL);

cleanup:
  logger_close();
  if (text != NULL) {
    OV_FREE(&text);
  }
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief How often a text appears in the log that was read back
 *
 * @param text the contents of the log
 * @param needle the text to look for
 * @return how many times it appears
 */
static size_t count_occurrences(char const *const text, char const *const needle) {
  size_t n = 0;

  for (char const *p = text; p != NULL;) {
    char const *const hit = strstr(p, needle);
    if (hit == NULL) {
      break;
    }
    n++;
    p = hit + 1;
  }
  return n;
}

/**
 * @brief The same report reaching the log again is written once
 *
 * The device state is polled every few seconds, so a standing failure reports the same
 * block again and again; the file is for reading after something went wrong, not for
 * copies of one failure.
 */
static void test_repeated_report_is_written_once(void) {
  struct ov_error err = {0};
  char *text = NULL;

  if (logger_is_open()) {
    logger_close();
  }
  if (!TEST_CHECK(logger_open(&err) == true)) {
    goto cleanup;
  }
  logger_output(ov_error_severity_error, "[01:0x00000001] adc-repeated: the same failure");
  logger_output(ov_error_severity_error, "[01:0x00000001] adc-repeated: the same failure");
  logger_output(ov_error_severity_error, "[01:0x00000002] adc-repeated: a different failure");
  logger_close();

  if (!TEST_CHECK(read_log_file(&text, &err))) {
    goto cleanup;
  }
  TEST_CASE("the report that repeats is written once");
  if (!TEST_CHECK(count_occurrences(text, "adc-repeated: the same failure") == 1)) {
    TEST_MSG("want one record, got %zu", count_occurrences(text, "adc-repeated: the same failure"));
  }
  TEST_CASE("a report of its own is not held back");
  TEST_CHECK(count_occurrences(text, "adc-repeated: a different failure") == 1);
  TEST_CASE_(NULL);

cleanup:
  logger_close();
  if (text != NULL) {
    OV_FREE(&text);
  }
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief The file is trimmed when it grows past the limit, and it keeps its newest records
 */
static void test_trim_on_write(void) {
  struct ov_error err = {0};
  char *exe = NULL;
  char *path = NULL;
  wchar_t *wide = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  LARGE_INTEGER offset;
  char *blob = NULL;
  char *text = NULL;
  DWORD written = 0;
  DWORD got = 0;
  static size_t const blob_size = (size_t)LOGGER_MAX_BYTES + 4096;

  if (logger_is_open()) {
    logger_close();
  }
  exe = paths_executable(&err);
  path = (exe != NULL) ? logger_path_for_exe(exe, &err) : NULL;
  if (!TEST_CHECK((path != NULL) && ov_sprintf_wchar(&wide, &err, NULL, L"%s", path))) {
    goto cleanup;
  }
  h = CreateFileW(wide, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (!TEST_CHECK(h != INVALID_HANDLE_VALUE)) {
    goto cleanup;
  }
  if (!TEST_CHECK(OV_REALLOC(&blob, blob_size, sizeof(blob[0])))) {
    goto cleanup;
  }
  for (size_t i = 0; i < blob_size; i++) {
    blob[i] = ((i % 120) == 119) ? '\n' : 'a';
  }
  if (!TEST_CHECK(WriteFile(h, blob, (DWORD)blob_size, &written, NULL) && (written == (DWORD)blob_size))) {
    goto cleanup;
  }
  CloseHandle(h);
  h = INVALID_HANDLE_VALUE;

  if (!TEST_CHECK(logger_open(&err) == true)) {
    goto cleanup;
  }
  TEST_CHECK(logger_is_open() == true);
  if (!TEST_CHECK(logger_write("trim", "the newest record", &err) == true)) {
    goto cleanup;
  }
  logger_close();

  h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (!TEST_CHECK(h != INVALID_HANDLE_VALUE)) {
    goto cleanup;
  }
  if (!TEST_CHECK(GetFileSizeEx(h, &size) && (size.QuadPart < LOGGER_MAX_BYTES))) {
    TEST_MSG("want less than %d bytes, got %lld", LOGGER_MAX_BYTES, (long long)size.QuadPart);
    goto cleanup;
  }
  offset.QuadPart = (size.QuadPart > 256) ? (size.QuadPart - 256) : 0;
  if (!TEST_CHECK(SetFilePointerEx(h, offset, NULL, FILE_BEGIN))) {
    goto cleanup;
  }
  if (!TEST_CHECK(OV_REALLOC(&text, (size_t)(size.QuadPart - offset.QuadPart) + 1, sizeof(text[0])))) {
    goto cleanup;
  }
  if (!TEST_CHECK(ReadFile(h, text, (DWORD)(size.QuadPart - offset.QuadPart), &got, NULL))) {
    goto cleanup;
  }
  text[got] = '\0';
  TEST_CHECK(strstr(text, "the newest record") != NULL);

cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  logger_close();
  if (wide != NULL) {
    DeleteFileW(wide);
    OV_ARRAY_DESTROY(&wide);
  }
  OV_FREE(&blob);
  OV_FREE(&text);
  paths_string_free(&exe);
  logger_string_free(&path);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief Closing a log that was never opened is not a failure
 */
static void test_close_without_open(void) {
  logger_close();
  logger_close(); // twice must be fine as well
  logger_init();
  logger_init(); // and so must a second init
  TEST_CHECK(logger_is_open() == false);
}

/**
 * @brief The lines of a rendered error come out of the error machinery with its
 *        decorations on them
 *
 * A level marker the record carries already is dropped, and the error code stays in the
 * message because it is the identity of the failure.  The tag is the module the line
 * names, so the errors group by where they happened instead of blaming the library that
 * rendered them.
 */
static void test_error_fields(void) {
  char tag[32];
  char msg[512];

  TEST_CASE("the level marker is dropped, the error code stays");
  logger_error_fields("[01:0x00000001] the removal failed (at ui.c:617 report_note())", tag, sizeof(tag), msg, sizeof(msg));
  TEST_CHECK(strcmp(msg, "[01:0x00000001] the removal failed (at ui.c:617 report_note())") == 0);
  TEST_CHECK(strcmp(tag, "ui") == 0);

  TEST_CASE("the module the line names is the tag");
  logger_error_fields("  removal.c:15 set_configret_error() [01:0x00000001] CM_Query_And_Remove_SubTreeW failed: CONFIGRET=51",
                      tag,
                      sizeof(tag),
                      msg,
                      sizeof(msg));
  TEST_CHECK(strcmp(tag, "removal") == 0);
  TEST_CHECK(strcmp(msg, "removal.c:15 set_configret_error() [01:0x00000001] CM_Query_And_Remove_SubTreeW failed: CONFIGRET=51") == 0);

  TEST_CASE("the report position");
  logger_error_fields("         reported at ui.c:563 report_once()", tag, sizeof(tag), msg, sizeof(msg));
  TEST_CHECK(strcmp(tag, "ui") == 0);
  TEST_CHECK(strcmp(msg, "reported at ui.c:563 report_once()") == 0);

  TEST_CASE("a line that names no source");
  logger_error_fields("[WARN] something went wrong", tag, sizeof(tag), msg, sizeof(msg));
  TEST_CHECK(strcmp(tag, "error") == 0);
  TEST_CHECK(strcmp(msg, "something went wrong") == 0);

  TEST_CASE("a line with nothing left to say is empty");
  logger_error_fields("[ERROR] ", tag, sizeof(tag), msg, sizeof(msg));
  TEST_CHECK(msg[0] == '\0');

  TEST_CASE("the empty line is tolerated");
  logger_error_fields("", tag, sizeof(tag), msg, sizeof(msg));
  TEST_CHECK(msg[0] == '\0');
  TEST_CHECK(tag[0] != '\0');

  TEST_CASE_(NULL);
}

TEST_LIST = {
    {"format_line", test_format_line},
    {"format_line_escapes", test_format_line_escapes},
    {"format_line_bounds", test_format_line_bounds},
    {"error_fields", test_error_fields},
    {"sizes", test_sizes},
    {"trim_size", test_trim_size},
    {"keep_tail", test_keep_tail},
    {"path_for_exe", test_path_for_exe},
    {"write_to_file", test_write_to_file},
    {"run_records", test_run_records},
    {"repeated_report_is_written_once", test_repeated_report_is_written_once},
    {"trim_on_write", test_trim_on_write},
    {"close_without_open", test_close_without_open},
    {NULL, NULL},
};
