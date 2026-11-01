#include "mock/veto_log.h"

#ifdef ADC_MOCK

#  include "veto_log_manager.h"

#  include <string.h>
#  include <time.h>
#  include <wchar.h>

#  include <ovarray.h>
#  include <ovprintf.h>
#  include <ovprintf_ex.h>

#  include "veto_event.h"

/** @brief What the stand-in answers */
static struct veto_log_mock_script g_script = {0};
/** @brief How often the log was read */
static size_t g_reads = 0;
/** @brief The moment the last read was made with */
static uint64_t g_since_ms = 0;
/** @brief The bound the last read was made with */
static size_t g_max_wait_ms = 0;
/** @brief Whether a script is installed at all */
static bool g_installed = false;

/**
 * @brief Copy one value with the entities an event writes it with
 *
 * @param out receives the copy, release with OV_ARRAY_DESTROY; NULL when value is NULL
 * @param value the text to copy, NULL copies nothing
 * @return false when the copy could not be built
 */
static NODISCARD bool escape_dup(wchar_t **const out, wchar_t const *const value) {
  size_t need = 1;
  size_t o = 0;

  if (value == NULL) {
    return true;
  }
  for (wchar_t const *p = value; *p != L'\0'; p++) {
    switch (*p) {
    case L'&':
      need += 5;
      break;
    case L'<':
    case L'>':
      need += 4;
      break;
    case L'"':
    case L'\'':
      need += 6;
      break;
    default:
      need += 1;
      break;
    }
  }
  if (!OV_ARRAY_GROW(out, need)) {
    return false;
  }
  for (wchar_t const *p = value; *p != L'\0'; p++) {
    wchar_t const *entity = NULL;

    switch (*p) {
    case L'&':
      entity = L"&amp;";
      break;
    case L'<':
      entity = L"&lt;";
      break;
    case L'>':
      entity = L"&gt;";
      break;
    case L'"':
      entity = L"&quot;";
      break;
    case L'\'':
      entity = L"&apos;";
      break;
    default:
      (*out)[o++] = *p;
      continue;
    }
    size_t const len = wcslen(entity);
    memcpy(&(*out)[o], entity, len * sizeof(wchar_t));
    o += len;
  }
  (*out)[o] = L'\0';
  OV_ARRAY_SET_LENGTH(*out, o);
  return true;
}

/**
 * @brief One moment as the attribute an event writes it in
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
 * @brief Render one record of the script the way the log writes it
 *
 * The stand-in is the log of the system, so what it hands over is the event that log would
 * have written, not the parts of one: the run is then tried against the reader every
 * machine puts the event through.
 *
 * @param out receives the event, release with OV_ARRAY_DESTROY
 * @param from the record of the script to render
 * @return false when the event could not be rendered
 */
static NODISCARD bool render_event(wchar_t **const out, struct veto_log_mock_record const *const from) {
  wchar_t stamp[32];
  wchar_t *system_time = NULL;
  wchar_t *path = NULL;
  wchar_t *device = NULL;
  wchar_t *path_data = NULL;
  wchar_t *device_data = NULL;
  bool success = false;

  if (!escape_dup(&path, from->path) || !escape_dup(&device, from->device_instance)) {
    goto cleanup;
  }
  if (from->event_time_ms != 0) {
    if (!format_system_time(stamp, sizeof(stamp) / sizeof(stamp[0]), from->event_time_ms)) {
      goto cleanup;
    }
    if (!ov_sprintf_wchar(&system_time, NULL, NULL, L"<TimeCreated SystemTime='%ls'/>", stamp)) {
      goto cleanup;
    }
  }
  if ((path != NULL) && !ov_sprintf_wchar(&path_data, NULL, NULL, L"<Data Name='ProcessName'>%ls</Data>", path)) {
    goto cleanup;
  }
  if ((device != NULL) && !ov_sprintf_wchar(&device_data, NULL, NULL, L"<Data Name='DeviceInstance'>%ls</Data>", device)) {
    goto cleanup;
  }
  if (!ov_sprintf_wchar(out,
                        NULL,
                        NULL,
                        L"<Event><System>%ls</System><EventData>"
                        L"<Data Name='ProcessId'>%u</Data>%ls%ls"
                        L"</EventData></Event>",
                        (system_time != NULL) ? system_time : L"",
                        (unsigned)from->pid,
                        (path_data != NULL) ? path_data : L"",
                        (device_data != NULL) ? device_data : L"")) {
    goto cleanup;
  }
  success = true;

cleanup:
  if (system_time != NULL) {
    OV_ARRAY_DESTROY(&system_time);
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
  if (device != NULL) {
    OV_ARRAY_DESTROY(&device);
  }
  if (path_data != NULL) {
    OV_ARRAY_DESTROY(&path_data);
  }
  if (device_data != NULL) {
    OV_ARRAY_DESTROY(&device_data);
  }
  return success;
}

/**
 * @brief What the stand-in answers, and what it reads on the way
 */
struct veto_log_mock_read {
  struct veto_log_mock_record const *records; //!< the records to hand over
  size_t count;                               //!< how many there are
};

/**
 * @brief Hand the records of the script over as the events a log writes
 *
 * @param ctx where the events go
 * @param userdata the records to hand over
 * @param err receives the failure of the rendering or of the list
 * @return false when the list could not take one of them
 */
static NODISCARD bool emit_script(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err) {
  struct veto_log_mock_read const *const read = (struct veto_log_mock_read *)userdata;
  bool success = false;
  wchar_t *xml = NULL;

  for (size_t i = 0; i < read->count; i++) {
    if (!render_event(&xml, &read->records[i])) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    if (!ctx->emit(ctx, xml, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (xml != NULL) {
    OV_ARRAY_DESTROY(&xml);
  }
  return success;
}

/** @brief One lookup of the stand-in, answered as soon as it is asked for */
struct veto_log_wait {
  async_notify_fn notify;  //!< the caller's call, may be NULL
  void *userdata;          //!< handed to notify
  struct veto_event *list; //!< the records, empty but never NULL when the lookup did not fail
  struct ov_error *err;    //!< borrowed, the caller's
  bool failed;             //!< whether the lookup itself failed
};

static NODISCARD struct veto_log_wait *wait(wchar_t const *const log_name,
                                            wchar_t const *const provider_name,
                                            unsigned short const event_id,
                                            uint64_t const since_ms,
                                            unsigned long const max_wait_ms,
                                            async_notify_fn const notify,
                                            void *const userdata,
                                            struct ov_error *const err) {
  struct veto_log_wait *w = NULL;

  struct veto_log_mock_record const *from = NULL;
  size_t count = 0;

  (void)log_name;
  (void)provider_name;
  (void)event_id;
  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!OV_REALLOC(&w, 1, sizeof(*w))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  memset(w, 0, sizeof(*w));
  w->notify = notify;
  w->userdata = userdata;
  w->err = err;
  g_reads++;
  g_since_ms = since_ms;
  g_max_wait_ms = max_wait_ms;
  if (g_script.read_fails) {
    OV_ERROR_SET_GENERIC(w->err, ov_error_generic_fail);
    w->failed = true;
    if (notify != NULL) {
      notify(userdata, false);
    }
    return w;
  }
  from = ((g_reads >= 2) && (g_script.second_read != NULL)) ? g_script.second_read : g_script.records;
  count = ((g_reads >= 2) && (g_script.second_read != NULL)) ? g_script.second_read_count : g_script.record_count;
  {
    struct veto_log_mock_read read = {.records = from, .count = count};

    w->list = veto_event_list_build(emit_script, &read, w->err);
  }
  if (w->list == NULL) {
    w->failed = true;
  }
  if (notify != NULL) {
    notify(userdata, false);
  }
  return w;
}

static NODISCARD struct veto_event *wait_take(struct veto_log_wait **const w) {
  struct veto_event *list = NULL;

  if ((w == NULL) || (*w == NULL)) {
    return NULL;
  }
  if (!(*w)->failed) {
    list = (*w)->list;
    (*w)->list = NULL;
  }
  OV_FREE(w);
  return list;
}

static void wait_cancel(struct veto_log_wait *const w) {
  (void)w; // the stand-in is already answered by the time it is asked for
}

/** @brief The stand-in log, answering what the installed script says */
static struct veto_log_manager const kMockVetoLogManager = {
    .wait = wait,
    .wait_take = wait_take,
    .wait_cancel = wait_cancel,
};

static struct veto_log_manager const *manager(void) { return &kMockVetoLogManager; }

bool veto_log_mock_install(struct veto_log_mock_script const *const script) {
  if (script == NULL) {
    return false;
  }
  veto_log_mock_reset();
  g_script = *script;
  g_reads = 0;
  g_installed = true;
  veto_log_manager_select(manager());
  return true;
}

size_t veto_log_mock_read_count(void) { return g_reads; }

uint64_t veto_log_mock_since_ms(void) { return g_since_ms; }

size_t veto_log_mock_max_wait_ms(void) { return g_max_wait_ms; }

void veto_log_mock_reset(void) {
  memset(&g_script, 0, sizeof(g_script));
  g_reads = 0;
  g_since_ms = 0;
  g_max_wait_ms = 0;
  g_installed = false;
  veto_log_manager_select(NULL);
}

#endif // ADC_MOCK
