/**
 * Tests for the layer that turns veto records into the processes this tool may close.
 *
 * Everything here runs against dummy processes this test owns, never against iD.exe.  Two
 * things are checked: that the image of the running process decides which record is worth
 * acting on at all, and that the moment of the record decides whether the process reached
 * by the id is really the one the record names.
 */
#include <ovtest.h>

#include <windows.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include <time.h>

#include "blocker.h"
#include "clock_manager.h"
#include "mock/process.h"

#include "process_manager.h"
#include "veto_event.h"

/** @brief Room for one event the way the log renders it */
enum { kEventXmlLen = 512 };

/**
 * @brief Write one record as the log renders it
 *
 * The values are written as they are: the reader resolves entities.
 *
 * @param xml room for the event
 * @param size room of xml, in wchar_t
 * @param record what the event states
 */
static void render_event(wchar_t *const xml, size_t const size, struct veto_event const *const record) {
  time_t const seconds = (time_t)(record->event_time_ms / 1000ULL);
  struct tm tmv;

  memset(&tmv, 0, sizeof(tmv));
  gmtime_s(&tmv, &seconds);
  swprintf(xml,
           size,
           L"<Event><System><TimeCreated SystemTime='%04d-%02d-%02dT%02d:%02d:%02d.%03dZ'/></System>"
           L"<EventData><Data Name='ProcessId'>%lu</Data></EventData></Event>",
           tmv.tm_year + 1900,
           tmv.tm_mon + 1,
           tmv.tm_mday,
           tmv.tm_hour,
           tmv.tm_min,
           tmv.tm_sec,
           (int)(record->event_time_ms % 1000ULL),
           (unsigned long)record->pid);
}

/** @brief The records a list is built from */
struct event_script {
  struct veto_event const *records; //!< the records, in the order the log wrote them
  size_t count;                     //!< how many there are
};

/**
 * @brief Hand the records of the script to the list
 *
 * @param ctx where the events go
 * @param userdata the script
 * @param err receives the failure
 * @return false when the list could not take one of them
 */
static NODISCARD bool emit_script(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err) {
  struct event_script const *const script = (struct event_script const *)userdata;
  wchar_t xml[kEventXmlLen];

  for (size_t i = 0; i < script->count; i++) {
    render_event(xml, sizeof(xml) / sizeof(xml[0]), &script->records[i]);
    if (!ctx->emit(ctx, xml, err)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The list of the records the caller states
 *
 * @param records what the log wrote, in order
 * @param count how many records that is
 * @param err receives the failure
 * @return the list, NULL with err set when it could not be built
 */
static NODISCARD struct veto_event *build_list(struct veto_event const *const records, size_t const count, struct ov_error *const err) {
  struct event_script script = {.records = records, .count = count};

  return veto_event_list_build(emit_script, &script, err);
}

/**
 * @brief Close one process of the list, waited for on this thread
 *
 * A run closes its blockers through the process manager itself and not by waiting; this
 * keeps the tests of the list talking about one process the way the list does.
 *
 * @param it one entry of the list
 * @param err receives the failure
 * @return false when the process could not be closed
 */
static NODISCARD bool blocker_close(struct blocker_iter const *const it, struct ov_error *const err) {
  struct process_act *act = NULL;

  if ((it == NULL) || (it->pid == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  act = process_manager()->terminate(it->pid, it->event_time_ms, NULL, NULL, err);
  return (act != NULL) && process_manager()->act_release(&act);
}

/**
 * @brief How many entries the walk of the list hands out
 *
 * @param list the list to walk, may be NULL
 * @return the number of entries
 */
static size_t blocker_walked_count(struct blocker const *const list) {
  struct blocker_iter it = {0};
  size_t count = 0;

  while (blocker_next(list, &it)) {
    count++;
  }
  return count;
}

#define DUMMY_NAME L"test_dummy.exe"

/** @brief The separator and the quote the paths of the dummies are built with */
#define SEP_CHAR ((wchar_t)0x5C)
#define QUOTE_CHAR ((wchar_t)0x22)

/** @brief The names the settings are stood in for with, so the dummy is a mixer */
static wchar_t const *const kDummyNames[] = {DUMMY_NAME};

/**
 * @brief The dummy that ships with this build, next to the test executable
 *
 * @param err receives the failure of the lookup
 * @return the path of the dummy, NULL when it could not be found
 * @note Release with CoTaskMemFree.
 */
static NODISCARD wchar_t *shipped_dummy(struct ov_error *const err) {
  wchar_t *path = NULL;
  wchar_t self[512];
  wchar_t *slash = NULL;
  DWORD len = 0;

  len = GetModuleFileNameW(NULL, self, (DWORD)(sizeof(self) / sizeof(self[0])));
  if ((len == 0) || (len >= (DWORD)(sizeof(self) / sizeof(self[0])))) {
    return NULL;
  }
  self[len] = 0;
  slash = wcsrchr(self, SEP_CHAR);
  if (slash == NULL) {
    return NULL;
  }
  *slash = 0;
  if (!ov_sprintf_wchar(&path, err, NULL, L"%ls%lc%ls", self, (int)SEP_CHAR, DUMMY_NAME)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return path;
}

/**
 * @brief Start one of the dummies
 *
 * @param path the dummy to start
 * @param pi receives the handle of the process
 * @return false when the dummy could not be started
 */
static bool spawn(wchar_t const *const path, PROCESS_INFORMATION *const pi) {
  STARTUPINFOW si;
  wchar_t *cmdline = NULL;
  BOOL ok = FALSE;
  bool success = false;

  memset(pi, 0, sizeof(*pi));
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (!ov_sprintf_wchar(&cmdline, NULL, NULL, L"%lc%ls%lc", (int)QUOTE_CHAR, path, (int)QUOTE_CHAR)) {
    goto cleanup;
  }
  ok = CreateProcessW(path, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
  success = ok ? true : false;

cleanup:
  if (cmdline != NULL) {
    OV_ARRAY_DESTROY(&cmdline);
    cmdline = NULL;
  }
  return success;
}

/**
 * @brief Give the handles of a dummy back
 *
 * @param pi the process to close
 */
static void close_pi(PROCESS_INFORMATION *const pi) {
  if (pi->hThread) {
    CloseHandle(pi->hThread);
    pi->hThread = NULL;
  }
  if (pi->hProcess) {
    CloseHandle(pi->hProcess);
    pi->hProcess = NULL;
  }
}

/**
 * @brief Is this pid still running
 *
 * The question is asked of the same manager the list asks, so a process the list cannot see
 * is a process that is not running, and no private of the process layer is reached for.
 *
 * @param pid the process to look at
 * @return true while it is alive
 */
static bool is_running(uint32_t const pid) {
  struct ov_error err = {0};
  struct process_name name = {.pid = pid};

  TEST_SUCCEEDED(process_manager()->take_snapshot(&name, 1, &err) != SIZE_MAX, &err);
  return name.image[0] != L'\0';
}

/**
 * @brief A moment this process is known to be older than
 *
 * The public surface has no way to ask for the creation time alone, so the test asks whether
 * the process started before the moment it reads now: that the process exists at all is what
 * the test needs to build a record of its own around.
 *
 * @param out_ms receives the moment
 * @return true when the moment could be read
 */
static bool start_time_of(uint64_t *const out_ms) {
  *out_ms = clock_now_ms();
  return *out_ms > 0;
}

/**
 * @brief A record whose process is the one it names becomes a blocker
 *
 * The id and the moment of the record name this process, so the list carries it.
 */
static void test_record_names_the_running_process(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = start;
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_walked_count(blockers) == 1, "want the one running process, got %zu", blocker_walked_count(blockers));
  {
    struct blocker_iter it = {0};

    if (!TEST_CHECK(blocker_next(blockers, &it))) {
      goto cleanup;
    }
    TEST_CHECK(it.pid == (uint32_t)pi.dwProcessId);
    TEST_CHECK(it.mixer_name_index != SIZE_MAX);
    TEST_SUCCEEDED(blocker_close(&it, &err), &err);
  }
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == false);

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A process whose image is neither the mixer nor the audio engine is left out
 *
 * The snapshot says what each process is, so a process this tool never closes is dropped
 * before anything is opened.
 */
static void test_a_process_this_tool_never_closes_is_left_out(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  static wchar_t const *const other_names[] = {L"something_else.exe"};

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = clock_now_ms();
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, other_names, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_walked_count(blockers) == 0, "want no blocker, got %zu", blocker_walked_count(blockers));
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A record that is older than the process names somebody else
 *
 * The identity rule that keeps this tool off the wrong process: a process that started after
 * the record was written cannot be the one that blocked the removal, the id was given to it
 * in between.  The lookup keeps it (the image is the right one), and the close refuses it.
 */
static void test_record_older_than_the_process_is_refused(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = (start > 0) ? (start - 1000) : 1;
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  if (!TEST_CHECK(blocker_walked_count(blockers) == 1)) {
    goto cleanup;
  }
  {
    struct blocker_iter it = {0};

    if (!TEST_CHECK(blocker_next(blockers, &it))) {
      goto cleanup;
    }
    TEST_FAILED_WITH(blocker_close(&it, &err), &err, ov_error_type_generic, ov_error_generic_not_found);
  }
  TEST_CHECK(is_running((uint32_t)pi.dwProcessId) == true);

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief A record whose process is already gone names a blocker that is gone too
 *
 * That is an answer, not a failure: the device was held by somebody who is no longer there.
 */
static void test_record_of_a_gone_process_is_no_blocker(void) {
  struct ov_error err = {0};
  wchar_t *path = NULL;
  PROCESS_INFORMATION pi;
  struct veto_event *events = NULL;
  struct veto_event c;
  struct blocker *blockers = NULL;
  uint64_t start = 0;

  memset(&pi, 0, sizeof(pi));
  memset(&c, 0, sizeof(c));
  path = shipped_dummy(&err);
  if ((path == NULL) || !spawn(path, &pi)) {
    TEST_SKIP("cannot start the dummy process here");
    goto cleanup;
  }
  if (!TEST_CHECK(start_time_of(&start))) {
    goto cleanup;
  }
  c.pid = (uint32_t)pi.dwProcessId;
  c.event_time_ms = start;
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  close_pi(&pi);
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_walked_count(blockers) == 0, "want no blocker, got %zu", blocker_walked_count(blockers));

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  if (pi.hProcess != NULL) {
    close_pi(&pi);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
}

/**
 * @brief The list is empty when no record names a process that is there
 */
static void test_no_records_means_no_blockers(void) {
  struct ov_error err = {0};
  struct veto_event *events = NULL;
  struct blocker *blockers = NULL;

  blockers = blocker_list_build(events, kDummyNames, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(blocker_walked_count(blockers) == 0);

cleanup:
  blocker_list_destroy(&blockers);
}

/**
 * @brief Closing what was never built must not crash
 */
static void test_close_without_blocker(void) {
  struct ov_error err = {0};

  TEST_FAILED_WITH(blocker_close(NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);
  blocker_list_destroy(NULL);
}

/**
 * @brief The stand-in answers what its script says, without any real process
 *
 * A record names a process of the script; the lookup keeps it, the close ends it, and the
 * id moves on to a younger process refused.  No dummy has to be started for any of it.
 */
static void test_mock_answers_the_processes(void) {
  struct process_mock_entry entries[] = {
      {.pid = 100, .image = L"iD.exe", .start_ms = 1000, .ends = true},
      {.pid = 200, .image = L"audiodg.exe", .start_ms = 1000, .ends = false},
      {.pid = 300, .image = L"the_mixer_is_younger.exe", .start_ms = 9000, .ends = true},
  };
  struct process_mock_script const script = {
      .entries = entries,
      .entry_count = sizeof(entries) / sizeof(entries[0]),
  };
  struct veto_event *events = NULL;
  struct blocker *blockers = NULL;
  struct ov_error err = {0};
  struct veto_event c;

  TEST_ASSERT(process_mock_install(&script));

  TEST_CASE("the record names a process that is there");
  memset(&c, 0, sizeof(c));
  c.pid = 100;
  c.event_time_ms = 1000;
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, (wchar_t const *[]){L"iD.exe"}, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_walked_count(blockers) == 1, "want the one process of the script, got %zu", blocker_walked_count(blockers));
  {
    struct blocker_iter it = {0};

    if (!TEST_CHECK(blocker_next(blockers, &it))) {
      goto cleanup;
    }
    TEST_CHECK(it.mixer_name_index != SIZE_MAX);
    TEST_SUCCEEDED(blocker_close(&it, &err), &err);
  }
  TEST_CHECK(process_mock_ended(100));
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);

  TEST_CASE("the audio engine is kept and is not the mixer");
  memset(&c, 0, sizeof(c));
  c.pid = 200;
  c.event_time_ms = 1000;
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, (wchar_t const *[]){L"iD.exe"}, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(blocker_walked_count(blockers) == 1);
  {
    struct blocker_iter it = {0};

    if (!TEST_CHECK(blocker_next(blockers, &it))) {
      goto cleanup;
    }
    TEST_CHECK(it.mixer_name_index == SIZE_MAX);
    TEST_FAILED_WITH(blocker_close(&it, &err), &err, ov_error_type_generic, ov_error_generic_fail); // the script says it will not go
  }
  TEST_CHECK(!process_mock_ended(200));
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);

  TEST_CASE("the id moved on to a younger process: the close refuses it");
  memset(&c, 0, sizeof(c));
  c.pid = 300;
  c.event_time_ms = 1000; // the record is older than the process of the script
  events = build_list(&c, 1, &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }
  blockers = blocker_list_build(events, (wchar_t const *[]){L"the_mixer_is_younger.exe"}, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(blocker_walked_count(blockers) == 1, "want the lookup to keep it, got %zu", blocker_walked_count(blockers));
  {
    struct blocker_iter it = {0};

    if (!TEST_CHECK(blocker_next(blockers, &it))) {
      goto cleanup;
    }
    TEST_FAILED_WITH(blocker_close(&it, &err), &err, ov_error_type_generic, ov_error_generic_not_found);
  }
  TEST_CHECK(!process_mock_ended(300));

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  process_mock_reset();
}

/**
 * @brief The walk hands out every entry once, in the order the list keeps them
 *
 * The iterator is the only way to read a list, so its two promises are what the callers
 * lean on: each entry comes once, and the walk ends after the last one.
 */
static void test_walk_names_every_entry(void) {
  struct process_mock_entry entries[] = {
      {.pid = 100, .image = L"iD.exe", .start_ms = 1000, .ends = true},
      {.pid = 200, .image = L"audiodg.exe", .start_ms = 1000, .ends = true},
      {.pid = 300, .image = L"iD.exe", .start_ms = 1000, .ends = true},
  };
  struct process_mock_script const script = {
      .entries = entries,
      .entry_count = sizeof(entries) / sizeof(entries[0]),
  };
  struct veto_event *events = NULL;
  struct blocker *blockers = NULL;
  struct ov_error err = {0};
  struct veto_event const three[] = {
      {.pid = 100, .event_time_ms = 1000},
      {.pid = 200, .event_time_ms = 1000},
      {.pid = 300, .event_time_ms = 1000},
  };
  struct blocker_iter it = {0};
  uint32_t seen[3];
  size_t seen_count = 0;

  TEST_ASSERT(process_mock_install(&script));
  events = build_list(three, sizeof(three) / sizeof(three[0]), &err);
  if (!TEST_SUCCEEDED(events != NULL, &err)) {
    goto cleanup;
  }

  blockers = blocker_list_build(events, (wchar_t const *[]){L"iD.exe"}, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }

  TEST_CASE("every entry comes once, in the order of the list");
  while (blocker_next(blockers, &it)) {
    if (!TEST_CHECK(seen_count < 3)) {
      goto cleanup;
    }
    seen[seen_count++] = it.pid;
  }
  if (!TEST_CHECK(seen_count == 3)) {
    TEST_MSG("want 3 entries, got %zu", seen_count);
    goto cleanup;
  }
  TEST_CHECK(seen[0] == 100);
  TEST_CHECK(seen[1] == 200);
  TEST_CHECK(seen[2] == 300);

  TEST_CASE("the walk stays over after the last entry");
  TEST_CHECK(blocker_next(blockers, &it) == false);
  TEST_CHECK(blocker_next(blockers, &it) == false);

  TEST_CASE("an entry says which of the mixer names it is");
  it = (struct blocker_iter){0};
  TEST_CHECK(blocker_next(blockers, &it));
  TEST_CHECK(it.mixer_name_index == 0); // iD.exe is the first of the names
  TEST_CHECK(blocker_next(blockers, &it));
  TEST_CHECK(it.mixer_name_index == SIZE_MAX); // the audio engine is no mixer

  TEST_CASE_(NULL);

cleanup:
  blocker_list_destroy(&blockers);
  veto_event_destroy_list(&events);
  process_mock_reset();
}

/**
 * @brief A walk of nothing hands out nothing, and a null iterator is refused
 */
static void test_walk_of_nothing(void) {
  struct blocker_iter it = {0};
  struct blocker *blockers = NULL;
  struct ov_error err = {0};

  TEST_CHECK(blocker_next(NULL, &it) == false);
  TEST_CHECK(it.idx == 0);

  blockers = blocker_list_build(NULL, (wchar_t const *[]){L"iD.exe"}, 1, &err);
  if (!TEST_SUCCEEDED(blockers != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(blocker_next(blockers, &it) == false);
  TEST_CHECK(blocker_next(blockers, NULL) == false);

cleanup:
  blocker_list_destroy(&blockers);
}

/**
 * @brief The look fills an image for the ids it holds, leaves the others empty and says how many
 *
 * The empty image is how a caller reads "no such process at that moment", and the count is how
 * it tells a look that found nobody from one that failed, which is SIZE_MAX.
 */
static void test_snapshot_names_what_it_finds(void) {
  struct process_mock_entry entries[] = {
      {.pid = 100, .image = L"iD.exe", .start_ms = 1000, .ends = true},
      {.pid = 200, .image = L"audiodg.exe", .start_ms = 1000, .ends = true},
  };
  struct process_mock_script const script = {
      .entries = entries,
      .entry_count = sizeof(entries) / sizeof(entries[0]),
  };
  struct process_name names[4];
  struct ov_error err = {0};
  size_t found = 0;

  TEST_ASSERT(process_mock_install(&script));

  TEST_CASE("an id the script holds is answered with its image and counted");
  memset(names, 0, sizeof(names));
  names[0].pid = 100;
  names[1].pid = 200;
  found = process_manager()->take_snapshot(names, 2, &err);
  if (!TEST_SUCCEEDED(found != SIZE_MAX, &err)) {
    TEST_MSG("want the look to answer, got SIZE_MAX");
    goto cleanup;
  }
  TEST_CHECK_(found == 2, "want 2 found, got %zu", found);
  TEST_CHECK(wcscmp(names[0].image, L"iD.exe") == 0);
  TEST_MSG("want iD.exe, got %ls", names[0].image);
  TEST_CHECK(wcscmp(names[1].image, L"audiodg.exe") == 0);
  TEST_MSG("want audiodg.exe, got %ls", names[1].image);

  TEST_CASE("an id the script does not hold is left empty and is not counted");
  memset(names, 0, sizeof(names));
  names[0].pid = 100;
  names[1].pid = 999;
  found = process_manager()->take_snapshot(names, 2, &err);
  if (!TEST_SUCCEEDED(found != SIZE_MAX, &err)) {
    TEST_MSG("want the look to answer, got SIZE_MAX");
    goto cleanup;
  }
  TEST_CHECK_(found == 1, "want 1 found, got %zu", found);
  TEST_CHECK(names[0].image[0] != L'\0');
  TEST_CHECK(names[1].image[0] == L'\0');

  TEST_CASE("a name list the machine holds none of is 0, not a failure");
  memset(names, 0, sizeof(names));
  names[0].pid = 998;
  names[1].pid = 999;
  found = process_manager()->take_snapshot(names, 2, &err);
  if (!TEST_SUCCEEDED(found != SIZE_MAX, &err)) {
    TEST_MSG("want the look to answer, got SIZE_MAX");
    goto cleanup;
  }
  TEST_CHECK_(found == 0, "want none found, got %zu", found);

  TEST_CASE("the same id twice is answered in both of its places");
  memset(names, 0, sizeof(names));
  names[0].pid = 100;
  names[1].pid = 100;
  found = process_manager()->take_snapshot(names, 2, &err);
  if (!TEST_SUCCEEDED(found != SIZE_MAX, &err)) {
    TEST_MSG("want the look to answer, got SIZE_MAX");
    goto cleanup;
  }
  TEST_CHECK(wcscmp(names[0].image, L"iD.exe") == 0);
  TEST_CHECK(wcscmp(names[1].image, L"iD.exe") == 0);

  TEST_CASE("an image the caller left filled is replaced");
  memset(names, 0, sizeof(names));
  names[0].pid = 999;
  wcscpy(names[0].image, L"stale.exe");
  found = process_manager()->take_snapshot(names, 1, &err);
  if (!TEST_SUCCEEDED(found != SIZE_MAX, &err)) {
    TEST_MSG("want the look to answer, got SIZE_MAX");
    goto cleanup;
  }
  TEST_CHECK(names[0].image[0] == L'\0');

  TEST_CASE_(NULL);

cleanup:
  process_mock_reset();
}

TEST_LIST = {
    {"record_names_the_running_process", test_record_names_the_running_process},
    {"a_process_this_tool_never_closes_is_left_out", test_a_process_this_tool_never_closes_is_left_out},
    {"record_older_than_the_process_is_refused", test_record_older_than_the_process_is_refused},
    {"record_of_a_gone_process_is_no_blocker", test_record_of_a_gone_process_is_no_blocker},
    {"no_records_means_no_blockers", test_no_records_means_no_blockers},
    {"close_without_blocker", test_close_without_blocker},
    {"walk_names_every_entry", test_walk_names_every_entry},
    {"walk_of_nothing", test_walk_of_nothing},
    {"snapshot_names_what_it_finds", test_snapshot_names_what_it_finds},
    {"mock_answers_the_processes", test_mock_answers_the_processes},
    {NULL, NULL},
};
