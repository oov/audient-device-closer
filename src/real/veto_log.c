#include "veto_log_manager.h"

#include <windows.h>

#include <winevt.h>

#include <string.h>
#include <time.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "veto_event.h"

/**
 * @brief What the lookup hands the list as it reads
 *
 * The subscription and the buffer outlive each event and are reused by the next one, so
 * they belong to the lookup rather than to the emitter the list calls.
 */
struct veto_log_wait {
  struct async *async;          //!< the thread of the lookup
  struct async_context *ctx;    //!< the lookup, as its thread reaches it
  HANDLE record;                //!< up once the log has something to read
  HANDLE stop;                  //!< up once the caller asks the lookup to stop
  wchar_t const *log_name;      //!< the log to read
  wchar_t const *provider_name; //!< the provider that writes the records
  unsigned short event_id;      //!< the veto event of that provider
  uint64_t since_ms;            //!< records older than this belong to an earlier attempt
  unsigned long max_wait_ms;    //!< how long the lookup waits at most
  async_notify_fn notify;       //!< the caller's call, may be NULL
  void *userdata;               //!< handed to notify
  EVT_HANDLE subscription;      //!< what the log is read through
  EVT_HANDLE moment_ctx;        //!< renders the moment of an event, made once
  wchar_t *xml;                 //!< the render buffer, reused by the loop
  size_t taken;                 //!< how many records were handed over
  struct veto_event *list;      //!< the records, empty but never NULL when the lookup did not fail
  struct ov_error *err;         //!< borrowed, the caller's, written by the thread of the lookup
  bool failed;                  //!< whether the lookup itself failed
};

/**
 * @brief How far before the attempt a record may be written and still be the one it waits for
 *
 * A record cannot be older than the removal that made the system write it, but the moment
 * this run took is not necessarily the moment the log stamped: a machine that wakes from
 * sleep has its clock stepped by the time service right after the wake, and how far a step
 * goes cannot be known, the more so as a user may have set the clock.  The margin only
 * decides how much of the log is read and then let go, so a minute: enough for a step at a
 * resume, small enough that the read stays a handful of records.
 */
enum { veto_event_clock_margin_ms = 60000 };

/**
 * @brief The moment before which no record of this attempt can have been written
 *
 * @param since_ms when the attempt was made, epoch milliseconds UTC
 * @return the moment with the margin taken off
 */
static uint64_t oldest_record_ms(uint64_t const since_ms) {
  return (since_ms > veto_event_clock_margin_ms) ? (since_ms - veto_event_clock_margin_ms) : 0;
}

/**
 * @brief One moment in the form the query compares a SystemTime against
 *
 * @param out receives the value, "2000-01-01T00:00:01.0000000Z"
 * @param out_size room of out
 * @param time_ms_utc the moment, epoch milliseconds UTC
 * @return false when the moment could not be written
 */
static bool format_system_time(wchar_t *const out, size_t const out_size, uint64_t const time_ms_utc) {
  __time64_t const seconds = (__time64_t)(time_ms_utc / 1000);
  struct tm utc;
  wchar_t stamp[24];
  int head = 0;

  memset(&utc, 0, sizeof(utc));
  if (_gmtime64_s(&utc, &seconds) != 0) {
    return false;
  }
  head = ov_snprintf_wchar(stamp,
                           sizeof(stamp) / sizeof(stamp[0]),
                           NULL,
                           L"%04d-%02d-%02dT%02d:%02d:%02d",
                           utc.tm_year + 1900,
                           utc.tm_mon + 1,
                           utc.tm_mday,
                           utc.tm_hour,
                           utc.tm_min,
                           utc.tm_sec);
  if ((head <= 0) || ((size_t)head >= (sizeof(stamp) / sizeof(stamp[0])))) {
    return false;
  }
  head = ov_snprintf_wchar(out, out_size, NULL, L"%ls.%03d0000Z", stamp, (int)(time_ms_utc % 1000));
  return (head > 0) && ((size_t)head < out_size);
}

/**
 * @brief The moment of an event, read from the event itself
 *
 * The log stamps every event with the moment it was written, and the value is read without
 * rendering the event as XML: a record of an earlier attempt is then told apart from this
 * one before anything is parsed.
 *
 * @param context the render context that names the moment, made once by the lookup
 * @param event the event to read
 * @param out_ms receives the moment, epoch milliseconds UTC
 * @param err receives the failure of the read
 * @return false when the moment could not be read
 */
static NODISCARD bool
read_event_moment(EVT_HANDLE const context, EVT_HANDLE const event, uint64_t *const out_ms, struct ov_error *const err) {
  EVT_VARIANT value;
  DWORD used = 0;
  DWORD count = 0;

  memset(&value, 0, sizeof(value));
  *out_ms = 0;
  if (!EvtRender(context, event, EvtRenderEventValues, sizeof(value), &value, &used, &count)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if ((value.Type != EvtVarTypeFileTime) || (value.FileTimeVal == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return false;
  }
  *out_ms = (uint64_t)((value.FileTimeVal - 116444736000000000ULL) / 10000ULL);
  return true;
}

/**
 * @brief Read one event and hand it to the list when it was written for this attempt
 *
 * The log stamps a record with the moment it was written, and a machine that wakes from
 * sleep may have its clock stepped right after the wake, which is why the query and this
 * check both carry the margin.
 *
 * @param event the event to read
 * @param w the lookup, its render buffers are reused
 * @param ctx where a record of this attempt goes
 * @param err receives a failure that is worth stopping the read for
 * @return false on such a failure; an event of an earlier attempt is not one
 */
static NODISCARD bool
provide_event(EVT_HANDLE const event, struct veto_log_wait *const w, struct veto_event_context *const ctx, struct ov_error *const err) {
  uint64_t moment_ms = 0;
  DWORD used = 0;
  bool success = false;

  if (!read_event_moment(w->moment_ctx, event, &moment_ms, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (moment_ms < oldest_record_ms(w->since_ms)) {
    // a record of an earlier attempt is not the answer of this one, and is not a failure
    success = true;
    goto cleanup;
  }
  if (!EvtRender(NULL, event, EvtRenderEventXml, 0, NULL, &used, NULL) && (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!OV_ARRAY_GROW(&w->xml, (size_t)used)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!EvtRender(NULL, event, EvtRenderEventXml, used, w->xml, &used, NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!ctx->emit(ctx, w->xml, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  w->taken++;
  success = true;

cleanup:
  return success;
}

/**
 * @brief What one batch of the subscription answered
 */
enum batch_result {
  BATCH_MORE,   //!< there are more events to read
  BATCH_DONE,   //!< the subscription holds no more events
  BATCH_FAILED, //!< the read failed, err says why
};

/**
 * @brief Read one batch of the subscription: up to 8 events, their handles are closed again
 *
 * @param w the lookup, its render buffer is reused
 * @param ctx where the records of this attempt go
 * @param err receives the failure of the read
 * @return BATCH_DONE at the end of what the subscription holds, BATCH_FAILED when err says why
 */
static NODISCARD enum batch_result
query_events_step(struct veto_log_wait *const w, struct veto_event_context *const ctx, struct ov_error *const err) {
  enum batch_result result = BATCH_FAILED;
  EVT_HANDLE items[8] = {0};
  DWORD got = 0;

  memset(items, 0, sizeof(items));
  if (!EvtNext(w->subscription, 8, items, 0, 0, &got)) {
    DWORD const le = GetLastError();
    if ((le == ERROR_NO_MORE_ITEMS) || (le == ERROR_TIMEOUT)) {
      result = BATCH_DONE;
      goto cleanup;
    }
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
    goto cleanup;
  }
  for (DWORD i = 0; i < got; i++) {
    if (!provide_event(items[i], w, ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  result = BATCH_MORE;

cleanup:
  for (DWORD i = 0; i < 8; i++) {
    if (items[i]) {
      EvtClose(items[i]);
    }
  }
  return result;
}

/**
 * @brief Take everything the subscription holds at this moment
 *
 * The call never waits: it is made when the subscription said there is something, and
 * whatever is not taken now is taken at the next signal.
 *
 * @param w the lookup, its render buffer is reused
 * @param ctx where the records of this attempt go
 * @param err receives the failure of the read
 * @return false when the read failed
 */
static NODISCARD bool drain_subscription(struct veto_log_wait *const w, struct veto_event_context *const ctx, struct ov_error *const err) {
  for (;;) {
    enum batch_result const batch = query_events_step(w, ctx, err);
    if (batch == BATCH_FAILED) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    if (batch == BATCH_DONE) {
      return true;
    }
  }
}

/**
 * @brief Tell the caller what came of the lookup
 *
 * Called on the thread that asked the lookup to stop, or on the thread of the lookup once it
 * is over, which is what stopped says.
 *
 * @param userdata the lookup
 * @param stopped true when the caller asked the lookup to stop
 */
static void wait_notify(void *const userdata, bool const stopped) {
  struct veto_log_wait *const w = (struct veto_log_wait *)userdata;

  if (stopped) {
    if (!SetEvent(w->stop)) {
      // the ask never reaches the wait, so the lookup ends at its bound with no records
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      w->failed = true;
    }
    return;
  }
  if (w->notify != NULL) {
    w->notify(w->userdata, false);
  }
}

/**
 * @brief Wait for the records of this attempt, handing them to the list as they come
 *
 * Runs on the thread of the lookup.  The wait is answered by the subscription, so a record
 * wakes it as soon as it is written, and the bound the caller named ends it when none
 * comes.  A record of an earlier attempt is not handed over, so what the list holds is the
 * answer of this attempt and nothing else.  An ask to stop ends the wait at once as well.
 *
 * @param ctx the list being built
 * @param userdata the lookup
 * @param err receives the failure of the wait or of the list
 * @return false when the wait failed, true otherwise, an empty list included
 */
static NODISCARD bool emit_records(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err) {
  struct veto_log_wait *const w = (struct veto_log_wait *)userdata;
  ULONGLONG const deadline = GetTickCount64() + w->max_wait_ms;
  bool success = false;

  if (w->subscription == NULL) {
    OV_ERROR_ADD_TRACE(w->err);
    goto cleanup;
  }
  for (;;) {
    ULONGLONG const now = GetTickCount64();
    HANDLE ends[2];

    if (!drain_subscription(w, ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (w->taken > 0) {
      break; // what this run waits for is here
    }
    if (now >= deadline) {
      break; // the bound is up, and the subscription had nothing for this attempt
    }
    ends[0] = w->stop;
    ends[1] = w->record;
    switch (WaitForMultipleObjects(2, ends, FALSE, (DWORD)(deadline - now))) {
    case WAIT_OBJECT_0:
      success = true;
      goto cleanup; // the caller asked to stop, and what was read is the answer
    case WAIT_TIMEOUT:
      break; // the bound is up, and the subscription had nothing for this attempt
    case (WAIT_OBJECT_0 + 1):
      break; // a record was written, and the read at the top of the loop takes it
    default:
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (w->subscription != NULL) {
    EvtClose(w->subscription);
    w->subscription = NULL;
  }
  if (w->moment_ctx != NULL) {
    EvtClose(w->moment_ctx);
    w->moment_ctx = NULL;
  }
  if (w->record != NULL) {
    if (!CloseHandle(w->record)) {
      if (success) {
        OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(w->err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    }
    w->record = NULL;
  }
  if (w->xml != NULL) {
    OV_ARRAY_DESTROY(&w->xml);
  }
  return success;
}

/**
 * @brief Open the subscription and read what this attempt wrote
 *
 * Runs on the thread of the lookup, so the one that has the window is free while the log
 * writes what this attempt waits for.
 *
 * @param ctx the lookup, as this thread may reach it
 */
static void wait_body(struct async_context *const ctx) {
  struct veto_log_wait *const w = (struct veto_log_wait *)ctx->userdata;
  wchar_t *query = NULL;
  wchar_t from_stamp[32];
  bool success = false;

  w->ctx = ctx;
  w->record = CreateEventW(NULL, FALSE, FALSE, NULL); // auto reset: the signal is spent by the wait
  if (w->record == NULL) {
    OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!format_system_time(from_stamp, sizeof(from_stamp) / sizeof(from_stamp[0]), oldest_record_ms(w->since_ms))) {
    OV_ERROR_SET_GENERIC(w->err, ov_error_generic_unexpected);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&query,
                        w->err,
                        NULL,
                        L"*[System[Provider[@Name='%ls'] and EventID=%u and TimeCreated[@SystemTime >= '%ls']]]",
                        w->provider_name,
                        (unsigned)w->event_id,
                        from_stamp)) {
    OV_ERROR_ADD_TRACE(w->err);
    goto cleanup;
  }
  // the oldest record is asked for first as well, so a record that was written before this
  // lookup was made is not missed: the race between the removal and the subscription is gone.
  // The subscription is answered through the event, and nothing is asked to call back: a
  // callback and an event together are refused, and the records are read on this thread.
  w->subscription = EvtSubscribe(NULL, w->record, w->log_name, query, NULL, NULL, NULL, EvtSubscribeStartAtOldestRecord);
  if (w->subscription == NULL) {
    OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  {
    LPCWSTR paths[1];

    paths[0] = L"Event/System/TimeCreated/@SystemTime";
    w->moment_ctx = EvtCreateRenderContext(1, paths, EvtRenderContextValues);
  }
  if (w->moment_ctx == NULL) {
    OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  if (query != NULL) {
    OV_ARRAY_DESTROY(&query);
  }
  if (!success) {
    w->failed = true;
    return;
  }
  w->list = veto_event_list_build(emit_records, w, w->err);
  if (w->list == NULL) {
    w->failed = true;
  }
}

static NODISCARD struct veto_log_wait *wait(wchar_t const *const log_name,
                                            wchar_t const *const provider_name,
                                            unsigned short const event_id,
                                            uint64_t const since_ms,
                                            unsigned long const max_wait_ms,
                                            async_notify_fn const notify,
                                            void *const userdata,
                                            struct ov_error *const err) {
  struct veto_log_wait *w = NULL;
  bool success = false;

  if ((log_name == NULL) || (provider_name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!OV_REALLOC(&w, 1, sizeof(*w))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(w, 0, sizeof(*w));
  w->log_name = log_name;
  w->provider_name = provider_name;
  w->event_id = event_id;
  w->since_ms = since_ms;
  w->max_wait_ms = max_wait_ms;
  w->notify = notify;
  w->userdata = userdata;
  w->err = err;
  w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (w->stop == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // the event a stop sets is made here, so an ask is answered from the first moment the
  // operation is there rather than only once its body got that far
  w->async = async_start(wait_body, w, wait_notify, err);
  if (w->async == NULL) {
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success && (w != NULL)) {
    if (w->stop != NULL) {
      if (!CloseHandle(w->stop)) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    }
    OV_FREE(&w);
  }
  return success ? w : NULL;
}
static NODISCARD struct veto_event *wait_take(struct veto_log_wait **const w) {
  struct veto_event *list = NULL;

  if ((w == NULL) || (*w == NULL)) {
    return NULL;
  }
  if (!async_release(&(*w)->async, (*w)->err)) {
    OV_ERROR_ADD_TRACE((*w)->err);
    goto cleanup;
  }
  if (!(*w)->failed) {
    list = (*w)->list;
    (*w)->list = NULL;
  }

cleanup:
  if ((w != NULL) && (*w != NULL)) {
    if (!CloseHandle((*w)->stop)) {
      if (list == NULL) {
        OV_ERROR_PUSH((*w)->err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      } else {
        OV_ERROR_SET_HRESULT((*w)->err, HRESULT_FROM_WIN32(GetLastError()));
      }
      list = NULL;
    }
    OV_FREE(w);
  }
  return list;
}

static void wait_cancel(struct veto_log_wait *const w) {
  if (w != NULL) {
    async_cancel(w->async);
  }
}

/** @brief The log of the system, read through the event log service */
static struct veto_log_manager const kSystemVetoLogManager = {
    .wait = wait,
    .wait_take = wait_take,
    .wait_cancel = wait_cancel,
};

struct veto_log_manager const *veto_log_manager_system(void) { return &kSystemVetoLogManager; }
