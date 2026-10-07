#include "culprit.h"

#include <windows.h>

#include <winevt.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <ovarray.h>
#include <ovnum.h>
#include <ovprintf_ex.h>

/**
 * @brief Find the value of one <Data> element of the event
 *
 * The value is not copied: it is a span of the event, and copying it is the business of
 * whoever has to outlive the event.
 *
 * @param xml the rendered event, UTF-8
 * @param name the Name attribute to look for
 * @param out_value receives the span of the value, it is not terminated
 * @param out_len receives the length of the value
 * @return false when the event holds no such element or its value is empty
 */
static bool find_data(char const *const xml, char const *const name, char const **const out_value, size_t *const out_len) {
  char const *tag = NULL;

  *out_value = NULL;
  *out_len = 0;
  for (tag = strstr(xml, "<Data"); tag != NULL; tag = strstr(tag + 5, "<Data")) {
    char const *name_attr = strstr(tag, "Name=");
    if (name_attr == NULL) {
      continue;
    }
    char const *q = name_attr + 5;
    char const quote = *q;
    if ((quote != '"') && (quote != '\'')) {
      continue;
    }
    q++;
    char const *const end = strchr(q, quote);
    if (end == NULL) {
      continue;
    }
    size_t const got = (size_t)(end - q);
    char found[64];
    if ((got + 1) > sizeof(found)) {
      continue;
    }
    memcpy(found, q, got);
    found[got] = '\0';
    if (strcmp(found, name) != 0) {
      continue;
    }
    char const *const gt = strchr(end, '>');
    if (gt == NULL) {
      continue;
    }
    char const *const lt = strchr(gt, '<');
    if (lt == NULL) {
      continue;
    }
    *out_value = gt + 1;
    *out_len = (size_t)(lt - (gt + 1));
    return *out_len > 0;
  }
  return false;
}

/**
 * @brief Write a value with the XML entities resolved
 *
 * @param src the raw value
 * @param src_len length of the raw value
 * @param out receives the text, always terminated
 * @param out_size room of out
 * @return how many characters were written, the rest did not fit
 */
static size_t unescape_into(char const *const src, size_t const src_len, char *const out, size_t const out_size) {
  size_t o = 0;

  out[0] = '\0';
  for (size_t i = 0; (i < src_len) && ((o + 1) < out_size); i++) {
    char const *const p = &src[i];
    if ((*p != '&') || ((i + 5) >= src_len)) {
      out[o++] = *p;
      continue;
    }
    if (strncmp(p, "&amp;", 5) == 0) {
      out[o++] = '&';
      i += 4;
    } else if (strncmp(p, "&lt;", 4) == 0) {
      out[o++] = '<';
      i += 3;
    } else if (strncmp(p, "&gt;", 4) == 0) {
      out[o++] = '>';
      i += 3;
    } else if (strncmp(p, "&quot;", 6) == 0) {
      out[o++] = '"';
      i += 5;
    } else if (strncmp(p, "&apos;", 6) == 0) {
      out[o++] = '\'';
      i += 5;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = '\0';
  return o;
}

/**
 * @brief Read the value of one <Data> element into a buffer of the caller
 *
 * A value that does not fit is refused: a cut one would be read as another value.
 *
 * @param xml the rendered event, UTF-8
 * @param name the Name attribute to look for
 * @param out receives the value with its entities resolved, terminated
 * @param out_size room of out
 * @return false when the element is missing, empty or its value does not fit
 */
static bool read_data(char const *const xml, char const *const name, char *const out, size_t const out_size) {
  char const *value = NULL;
  size_t value_len = 0;

  if (!find_data(xml, name, &value, &value_len) || ((value_len + 1) > out_size)) {
    out[0] = '\0';
    return false;
  }
  return unescape_into(value, value_len, out, out_size) > 0;
}

/**
 * @brief Read the value of one <Data> element into a copy of its own
 *
 * The value becomes the caller's: the render buffer is reused for the next event, so a
 * record keeps its own.
 *
 * @param xml the rendered event, UTF-8
 * @param name the Name attribute to look for
 * @return the value with its entities resolved, NULL when the element is missing or empty
 * @note Release with OV_FREE.
 */
static char *read_data_dup(char const *const xml, char const *const name MEM_FILEPOS_PARAMS) {
  char const *value = NULL;
  size_t value_len = 0;
  char *out = NULL;

  if (!find_data(xml, name, &value, &value_len)) {
    return NULL;
  }
  if (!ov_mem_realloc(&out, value_len + 1, sizeof(out[0]) MEM_FILEPOS_VALUES_PASSTHRU)) {
    return NULL;
  }
  unescape_into(value, value_len, out, value_len + 1);
  return out;
}

/**
 * @brief Read the value of one <Data> element into a copy of its own, at the position of the
 *        call
 *
 * The caller should not have to write MEM_FILEPOS_VALUES itself: this appends the position
 * of the call, which read_data_dup hands down to the allocator.
 *
 * @param xml the rendered event, UTF-8
 * @param name the Name attribute to look for
 * @return the value with its entities resolved, NULL when the element is missing or empty
 * @note Release with OV_FREE.
 */
#define READ_DATA_DUP(xml, name) read_data_dup((xml), (name)MEM_FILEPOS_VALUES)

/**
 * @brief The moment of a SystemTime attribute
 *
 * @param value the attribute value, "2000-01-01T00:00:00.0000000Z"
 * @return the moment in epoch milliseconds UTC, 0 when the value cannot be read
 */
static long long parse_system_time_ms(char const *const value) {
  struct tm tmv;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int parsed = 0;
  char const *dot = NULL;
  long long seconds = 0;
  long long ms = 0;

  memset(&tmv, 0, sizeof(tmv));
  parsed = sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second);
  if (parsed != 6) {
    return 0;
  }
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_sec = second;
  tmv.tm_isdst = 0;
  seconds = (long long)_mkgmtime(&tmv);
  dot = strchr(value, '.');
  if (dot != NULL) {
    char frac[4] = {'0', '0', '0', '\0'};
    for (int i = 0; i < 3; i++) {
      char const c = dot[1 + i];
      if ((c < '0') || (c > '9')) {
        break;
      }
      frac[i] = c;
    }
    int64_t frac_ms = 0;
    if (ov_atoi_char(frac, &frac_ms, true)) {
      ms = frac_ms;
    }
  }
  return (seconds * 1000LL) + ms;
}

/**
 * @brief Read the value of one attribute of the event
 *
 * A value that does not fit is refused: nothing here reads one that long, and a cut one
 * would be read as another value.
 *
 * @param xml the rendered event, UTF-8
 * @param name the attribute name including its equals sign
 * @param out receives the value, terminated
 * @param out_size room of out
 * @return false when the attribute is missing or its value does not fit
 */
static bool read_attr(char const *const xml, char const *const name, char *const out, size_t const out_size) {
  char const *p = strstr(xml, name);
  char quote = 0;
  char const *end = NULL;
  size_t got = 0;

  out[0] = '\0';
  if (p == NULL) {
    return false;
  }
  p += strlen(name);
  quote = *p;
  if ((quote != '"') && (quote != '\'')) {
    return false;
  }
  p++;
  end = strchr(p, quote);
  if (end == NULL) {
    return false;
  }
  got = (size_t)(end - p);
  if ((got + 1) > out_size) {
    return false;
  }
  memcpy(out, p, got);
  out[got] = '\0';
  return true;
}

/**
 * @brief Parse one rendered event into a record
 *
 * The input is one rendered event (EvtRenderEventXml).
 *
 * @param xml_utf8 the event as EvtRenderEventXml produced it, UTF-8
 * @param out filled in when the event names a process; the strings become the caller's
 * @return false when the event carries no readable process id, true otherwise
 */
bool culprit_parse_xml(char const *const xml_utf8, struct culprit *const out) {
  char text[64];
  char time_value[64];
  uint64_t pid = 0;

  if ((xml_utf8 == NULL) || (out == NULL)) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  if (!read_data(xml_utf8, "ProcessId", text, sizeof(text))) {
    goto cleanup;
  }
  if (!ov_atou_char(text, &pid, false)) {
    goto cleanup;
  }
  out->pid = (uint32_t)pid;

  out->path = READ_DATA_DUP(xml_utf8, "ProcessName");
  out->command_line = READ_DATA_DUP(xml_utf8, "CommandLine");
  out->device_instance = READ_DATA_DUP(xml_utf8, "DeviceInstance");
  if (out->device_instance != NULL) {
    char *const nl = strpbrk(out->device_instance, "\r\n");
    if (nl != NULL) {
      *nl = '\0'; // the first entry of the list is the device that vetoed
    }
  }
  if (read_attr(xml_utf8, "SystemTime=", time_value, sizeof(time_value))) {
    out->event_time_ms = parse_system_time_ms(time_value);
  }

cleanup:
  if (out->pid == 0) {
    OV_FREE(&out->path);
    OV_FREE(&out->command_line);
    OV_FREE(&out->device_instance);
    memset(out, 0, sizeof(*out));
    return false;
  }
  return true;
}

/**
 * @brief Drop what the caller must not act on any more
 *
 * Events older than since_ms and duplicates of the same pid + device go,
 * the newest of the duplicates is kept.
 *
 * @param list the list to shrink in place
 * @param since_ms events older than this are dropped, 0 keeps every age
 */
void culprit_prune(struct culprit **const list, long long const since_ms) {
  size_t n = 0;
  size_t kept = 0;

  if ((list == NULL) || (*list == NULL)) {
    goto cleanup;
  }
  n = OV_ARRAY_LENGTH(*list);

  for (size_t i = 0; i < n; i++) {
    if ((*list)[i].pid == 0) {
      continue;
    }
    for (size_t j = i + 1; j < n; j++) {
      bool same_device = false;
      if ((*list)[j].pid != (*list)[i].pid) {
        continue;
      }
      if (((*list)[i].device_instance == NULL) && ((*list)[j].device_instance == NULL)) {
        same_device = true;
      } else if (((*list)[i].device_instance != NULL) && ((*list)[j].device_instance != NULL)) {
        same_device = (strcmp((*list)[i].device_instance, (*list)[j].device_instance) == 0);
      }
      if (!same_device) {
        continue;
      }
      struct culprit *const loser = ((*list)[i].event_time_ms >= (*list)[j].event_time_ms) ? &(*list)[j] : &(*list)[i];
      loser->pid = 0;
      if (loser == &(*list)[i]) {
        break;
      }
    }
  }

  for (size_t i = 0; i < n; i++) {
    bool const too_old = (since_ms != 0) && ((*list)[i].event_time_ms != 0) && ((*list)[i].event_time_ms < since_ms);
    if (((*list)[i].pid == 0) || too_old) {
      continue;
    }
    if (kept != i) {
      struct culprit tmp = (*list)[kept];
      (*list)[kept] = (*list)[i];
      (*list)[i] = tmp;
    }
    kept++;
  }
  OV_ARRAY_SET_LENGTH(*list, kept);

cleanup:
  for (size_t i = kept; i < n; i++) {
    OV_FREE(&(*list)[i].path);
    OV_FREE(&(*list)[i].command_line);
    OV_FREE(&(*list)[i].device_instance);
    (*list)[i].pid = 0;
  }
}

/**
 * @brief Number of records in the list
 *
 * @param list NULL is allowed
 * @return the number of records, 0 for NULL
 */
size_t culprit_count(struct culprit const *const list) {
  if (list == NULL) {
    return 0;
  }
  return OV_ARRAY_LENGTH(list);
}

/**
 * @brief Drop every entry that names one of the given processes
 *
 * A process this tool closed on its own is not a reason the device was blocked, so it must
 * not appear in the list the user is shown.  The caller passes the processes it closed, not
 * the image names, so an entry is dropped only when it is the exact instance that was
 * closed: the same image running somewhere else is a different blocker.
 *
 * @param list the list to shrink in place
 * @param pids the process ids that were closed
 * @param count how many of them there are
 */
void culprit_drop_pids(struct culprit **const list, uint32_t const *const pids, size_t const count) {
  size_t n = 0;
  size_t kept = 0;

  if ((list == NULL) || (*list == NULL) || (pids == NULL) || (count == 0)) {
    return;
  }
  n = OV_ARRAY_LENGTH(*list);
  for (size_t i = 0; i < n; i++) {
    bool drop = false;
    for (size_t k = 0; k < count; k++) {
      if (((*list)[i].pid != 0) && ((*list)[i].pid == pids[k])) {
        drop = true;
        break;
      }
    }
    if (drop) {
      continue;
    }
    if (kept != i) {
      struct culprit tmp = (*list)[kept];
      (*list)[kept] = (*list)[i];
      (*list)[i] = tmp;
    }
    kept++;
  }
  OV_ARRAY_SET_LENGTH(*list, kept);
  for (size_t i = kept; i < n; i++) {
    OV_FREE(&(*list)[i].path);
    OV_FREE(&(*list)[i].command_line);
    OV_FREE(&(*list)[i].device_instance);
    (*list)[i].pid = 0;
  }
}

/**
 * @brief Drop the members of the list and leave it empty
 *
 * Used when the device was closed: nothing blocks it any more, so whatever a record named
 * before is history.  An empty list and an absent one mean the same thing to the caller.
 *
 * @param list the list to empty in place
 */
void culprit_clear(struct culprit **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(*list);
  for (size_t i = 0; i < n; i++) {
    OV_FREE(&(*list)[i].path);
    OV_FREE(&(*list)[i].command_line);
    OV_FREE(&(*list)[i].device_instance);
    (*list)[i].pid = 0;
  }
  OV_ARRAY_SET_LENGTH(*list, 0);
}

/**
 * @brief Release the list and every record in it
 *
 * @param list set to NULL afterwards
 */
void culprit_destroy_list(struct culprit **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(*list);
  for (size_t i = 0; i < n; i++) {
    OV_FREE(&(*list)[i].path);
    OV_FREE(&(*list)[i].command_line);
    OV_FREE(&(*list)[i].device_instance);
  }
  OV_ARRAY_DESTROY(list);
}

/**
 * @brief Render one event of a batch and append it to the list when it names a process
 *
 * xml and utf8 are passed by address so that the loop can reuse them; ov_sprintf_* clears
 * an existing buffer before formatting, and OV_REALLOC resizes xml in place.
 *
 * @param event the event to render
 * @param xml the wide render buffer, reused by the loop
 * @param utf8 the UTF-8 form of the render, reused by the loop
 * @param out_list receives the record when the event names a process
 * @param err receives a failure that is worth stopping the query for
 * @return false on such a failure; an event that cannot be read is not one
 */
static bool
harvest_event(EVT_HANDLE const event, wchar_t **const xml, char **const utf8, struct culprit **const out_list, struct ov_error *const err) {
  bool success = false;
  DWORD used = 0;
  struct culprit item;

  memset(&item, 0, sizeof(item));
  if (!EvtRender(NULL, event, EvtRenderEventXml, 0, NULL, &used, NULL) && (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!OV_REALLOC(xml, (size_t)used, sizeof((*xml)[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (!EvtRender(NULL, event, EvtRenderEventXml, used, *xml, &used, NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!ov_sprintf_char(utf8, err, NULL, "%ls", *xml)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!culprit_parse_xml(*utf8, &item)) {
    success = true; // an event we cannot read must not fail the whole query
    goto cleanup;
  }
  if (!OV_ARRAY_PUSH(out_list, item)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    OV_FREE(&item.path);
    OV_FREE(&item.command_line);
    OV_FREE(&item.device_instance);
  }
  return success;
}

/**
 * @brief Render the events of one batch and append the readable ones to the list
 *
 * It holds no resources of its own, so a failure only ends the batch.
 *
 * @param items the events of the batch
 * @param got how many there are
 * @param xml the wide render buffer, reused by the loop
 * @param utf8 the UTF-8 form of the render, reused by the loop
 * @param out_list receives the records of the readable events
 * @param err receives a failure that is worth stopping the query for
 * @return false on such a failure
 */
static bool harvest_batch(EVT_HANDLE const *const items,
                          DWORD const got,
                          wchar_t **const xml,
                          char **const utf8,
                          struct culprit **const out_list,
                          struct ov_error *const err) {
  for (DWORD i = 0; i < got; i++) {
    if (!harvest_event(items[i], xml, utf8, out_list, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
  }
  return true;
}

/**
 * @brief What one batch of the query answered
 */
enum batch_result {
  BATCH_MORE,   //!< there are more events to read
  BATCH_DONE,   //!< the query holds no more events
  BATCH_FAILED, //!< the read failed, err says why
};

/**
 * @brief Read one batch of the query: up to 8 events, their handles are closed again
 *
 * @param q the query to advance
 * @param xml the wide render buffer, reused by the loop
 * @param utf8 the UTF-8 form of the render, reused by the loop
 * @param out_list receives the records of the readable events
 * @param err receives the failure of the read
 * @return BATCH_DONE at the end of the query, BATCH_FAILED when err says why
 */
static enum batch_result
query_events_step(EVT_HANDLE const q, wchar_t **const xml, char **const utf8, struct culprit **const out_list, struct ov_error *const err) {
  enum batch_result result = BATCH_FAILED;
  EVT_HANDLE items[8] = {0};
  DWORD got = 0;

  memset(items, 0, sizeof(items));
  if (!EvtNext(q, 8, items, 1000, 0, &got)) {
    DWORD const le = GetLastError();
    if ((le == ERROR_NO_MORE_ITEMS) || (le == ERROR_TIMEOUT)) {
      result = BATCH_DONE;
      goto cleanup;
    }
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(le));
    goto cleanup;
  }
  if (!harvest_batch(items, got, xml, utf8, out_list, err)) {
    goto cleanup;
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
 * @brief Read every event the query matches
 *
 * @param channel the event log to ask
 * @param query the XPath the log understands
 * @param out_list receives the records of the readable events
 * @param err receives the failure of the query
 * @return false when the query itself failed
 */
static bool
query_events(wchar_t const *const channel, wchar_t const *const query, struct culprit **const out_list, struct ov_error *const err) {
  bool success = false;
  EVT_HANDLE q = NULL;
  wchar_t *xml = NULL;
  char *utf8 = NULL;

  q = EvtQuery(NULL, channel, query, EvtQueryChannelPath | EvtQueryForwardDirection);
  if (q == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  for (;;) {
    enum batch_result const result = query_events_step(q, &xml, &utf8, out_list, err);
    if (result == BATCH_FAILED) {
      break;
    }
    if (result == BATCH_DONE) {
      success = true;
      break;
    }
  }

cleanup:
  if (q) {
    EvtClose(q);
  }
  OV_FREE(&xml);
  if (utf8) {
    OV_ARRAY_DESTROY(&utf8);
  }
  return success;
}

/**
 * @brief Read the veto events of the last 24 hours
 *
 * They come in the order the log returns them.
 *
 * @param log_name event log that holds the records
 * @param provider_name provider that writes them
 * @param event_id the veto event of that provider
 * @param err receives the failure of the query itself
 * @return NULL only when the query itself failed; an empty array means there is no veto event
 */
struct culprit *
culprit_read(char const *const log_name, char const *const provider_name, unsigned short const event_id, struct ov_error *const err) {
  struct culprit *list = NULL;
  wchar_t *channel = NULL;
  wchar_t *query = NULL;
  bool success = false;

  if ((log_name == NULL) || (provider_name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!ov_sprintf_wchar(&channel, err, NULL, L"%s", log_name)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&query,
                        err,
                        NULL,
                        L"*[System[Provider[@Name='%s'] and EventID=%u and TimeCreated[timediff(@SystemTime) <= 86400000]]]",
                        provider_name,
                        (unsigned)event_id)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!query_events(channel, query, &list, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (channel != NULL) {
    OV_ARRAY_DESTROY(&channel); // made by ov_sprintf_wchar
    channel = NULL;
  }
  if (query != NULL) {
    OV_ARRAY_DESTROY(&query);
    query = NULL;
  }
  if (success && (list == NULL) && !OV_ARRAY_GROW(&list, 1)) {
    success = false;
  }
  if (!success) {
    culprit_destroy_list(&list);
  }
  return success ? list : NULL;
}
