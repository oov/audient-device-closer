#include "veto_event.h"

#include <string.h>
#include <time.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovnum.h>
#include <ovprintf.h>

/**
 * @brief Find the value of one <Data> element of the event
 *
 * The value is not copied: it is a span of the event, and copying it is the business of
 * whoever has to outlive the event.
 *
 * @param xml the rendered event
 * @param name the Name attribute to look for
 * @param out_value receives the span of the value, it is not terminated
 * @param out_len receives the length of the value
 * @return false when the event holds no such element or its value is empty
 */
static bool find_data(wchar_t const *const xml, wchar_t const *const name, wchar_t const **const out_value, size_t *const out_len) {
  wchar_t const *tag = NULL;

  *out_value = NULL;
  *out_len = 0;
  for (tag = wcsstr(xml, L"<Data"); tag != NULL; tag = wcsstr(tag + 5, L"<Data")) {
    wchar_t const *name_attr = wcsstr(tag, L"Name=");
    if (name_attr == NULL) {
      continue;
    }
    wchar_t const *q = name_attr + 5;
    wchar_t const quote = *q;
    if ((quote != L'"') && (quote != L'\'')) {
      continue;
    }
    q++;
    wchar_t const *const end = wcschr(q, quote);
    if (end == NULL) {
      continue;
    }
    size_t const got = (size_t)(end - q);
    wchar_t found[64];
    if ((got + 1) > (sizeof(found) / sizeof(found[0]))) {
      continue;
    }
    memcpy(found, q, got * sizeof(found[0]));
    found[got] = L'\0';
    if (wcscmp(found, name) != 0) {
      continue;
    }
    wchar_t const *const gt = wcschr(end, L'>');
    if (gt == NULL) {
      continue;
    }
    wchar_t const *const lt = wcschr(gt, L'<');
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
static size_t unescape_into(wchar_t const *const src, size_t const src_len, wchar_t *const out, size_t const out_size) {
  size_t o = 0;

  out[0] = L'\0';
  for (size_t i = 0; (i < src_len) && ((o + 1) < out_size); i++) {
    wchar_t const *const p = &src[i];
    if ((*p != L'&') || ((i + 5) >= src_len)) {
      out[o++] = *p;
      continue;
    }
    if (wcsncmp(p, L"&amp;", 5) == 0) {
      out[o++] = L'&';
      i += 4;
    } else if (wcsncmp(p, L"&lt;", 4) == 0) {
      out[o++] = L'<';
      i += 3;
    } else if (wcsncmp(p, L"&gt;", 4) == 0) {
      out[o++] = L'>';
      i += 3;
    } else if (wcsncmp(p, L"&quot;", 6) == 0) {
      out[o++] = L'"';
      i += 5;
    } else if (wcsncmp(p, L"&apos;", 6) == 0) {
      out[o++] = L'\'';
      i += 5;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = L'\0';
  return o;
}

/**
 * @brief Read the value of one <Data> element into a buffer of the caller
 *
 * A value that does not fit is refused: a cut one would be read as another value.
 *
 * @param xml the rendered event
 * @param name the Name attribute to look for
 * @param out receives the value with its entities resolved, terminated
 * @param out_size room of out
 * @return false when the element is missing, empty or its value does not fit
 */
static bool read_data(wchar_t const *const xml, wchar_t const *const name, wchar_t *const out, size_t const out_size) {
  wchar_t const *value = NULL;
  size_t value_len = 0;

  if (!find_data(xml, name, &value, &value_len) || ((value_len + 1) > out_size)) {
    out[0] = L'\0';
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
 * @param xml the rendered event
 * @param name the Name attribute to look for
 * @return the value with its entities resolved, NULL when the element is missing or empty
 * @note Release with OV_ARRAY_DESTROY.
 */
static wchar_t *read_data_dup(wchar_t const *const xml, wchar_t const *const name) {
  wchar_t const *value = NULL;
  size_t value_len = 0;
  wchar_t *out = NULL;

  if (!find_data(xml, name, &value, &value_len)) {
    return NULL;
  }
  if (!OV_ARRAY_GROW(&out, value_len + 1)) {
    return NULL;
  }
  unescape_into(value, value_len, out, value_len + 1);
  OV_ARRAY_SET_LENGTH(out, value_len);
  return out;
}

/**
 * @brief The moment of a SystemTime attribute
 *
 * @param value the attribute value, "2000-01-01T00:00:00.0000000Z"
 * @return the moment in epoch milliseconds UTC, 0 when the value cannot be read
 */
static uint64_t parse_system_time_ms(wchar_t const *const value) {
  struct tm tmv;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int parsed = 0;
  wchar_t const *dot = NULL;
  long long seconds = 0;
  uint64_t ms = 0;

  memset(&tmv, 0, sizeof(tmv));
  parsed = swscanf(value, L"%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second);
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
  dot = wcschr(value, L'.');
  if (dot != NULL) {
    wchar_t frac[4] = {L'0', L'0', L'0', L'\0'};
    for (int i = 0; i < 3; i++) {
      wchar_t const c = dot[1 + i];
      if ((c < L'0') || (c > L'9')) {
        break;
      }
      frac[i] = c;
    }
    int64_t frac_ms = 0;
    if (ov_atoi_wchar(frac, &frac_ms, true)) {
      ms = (uint64_t)frac_ms;
    }
  }
  return ((uint64_t)seconds * 1000ULL) + ms;
}

/**
 * @brief Read the value of one attribute of the event
 *
 * A value that does not fit is refused: nothing here reads one that long, and a cut one
 * would be read as another value.
 *
 * @param xml the rendered event
 * @param name the attribute name including its equals sign
 * @param out receives the value, terminated
 * @param out_size room of out
 * @return false when the attribute is missing or its value does not fit
 */
static bool read_attr(wchar_t const *const xml, wchar_t const *const name, wchar_t *const out, size_t const out_size) {
  wchar_t const *p = wcsstr(xml, name);
  wchar_t quote = 0;
  wchar_t const *end = NULL;
  size_t got = 0;

  out[0] = L'\0';
  if (p == NULL) {
    return false;
  }
  p += wcslen(name);
  quote = *p;
  if ((quote != L'"') && (quote != L'\'')) {
    return false;
  }
  p++;
  end = wcschr(p, quote);
  if (end == NULL) {
    return false;
  }
  got = (size_t)(end - p);
  if ((got + 1) > out_size) {
    return false;
  }
  memcpy(out, p, got * sizeof(out[0]));
  out[got] = L'\0';
  return true;
}

/**
 * @brief Parse one rendered event into a record
 *
 * The input is one rendered event (EvtRenderEventXml).  Only the log that writes the events
 * reads them, so the parser stays behind the builder rather than being a thing a caller can
 * ask for: what a caller gets is a list of records, not the parts of one.
 *
 * @param xml the event as EvtRenderEventXml produced it
 * @param out filled in when the event names a process; the strings become the caller's
 * @return false when the event carries no readable process id, true otherwise
 */
static NODISCARD bool veto_event_parse_xml(wchar_t const *const xml, struct veto_event *const out) {
  wchar_t text[64];
  wchar_t time_value[64];
  uint64_t pid = 0;
  bool success = false;

  if ((xml == NULL) || (out == NULL)) {
    return false; // nothing to write the record into, the cleanup would touch it
  }
  memset(out, 0, sizeof(*out));

  if (!read_data(xml, L"ProcessId", text, sizeof(text) / sizeof(text[0]))) {
    goto cleanup;
  }
  if (!ov_atou_wchar(text, &pid, false)) {
    goto cleanup;
  }
  out->pid = (uint32_t)pid;

  out->path = read_data_dup(xml, L"ProcessName");
  out->device_instance = read_data_dup(xml, L"DeviceInstance");
  if (out->device_instance != NULL) {
    wchar_t *const nl = wcspbrk(out->device_instance, L"\r\n");
    if (nl != NULL) {
      *nl = L'\0'; // the first entry of the list is the device that vetoed
    }
  }
  if (read_attr(xml, L"SystemTime=", time_value, sizeof(time_value) / sizeof(time_value[0]))) {
    out->event_time_ms = parse_system_time_ms(time_value);
  }
  success = true;

cleanup:
  if (!success || (out->pid == 0)) {
    if (out->path != NULL) {
      OV_ARRAY_DESTROY(&out->path);
    }
    if (out->device_instance != NULL) {
      OV_ARRAY_DESTROY(&out->device_instance);
    }
    memset(out, 0, sizeof(*out));
    success = false;
  }
  return success;
}

/**
 * @brief What a list that is being built keeps beyond what the emitter reaches
 *
 * The context the emitter is given is the first member, so the two are the same address
 * and emit can reach what the list holds without the emitter knowing of it.
 */
struct veto_event_context_private {
  struct veto_event_context ctx; //!< what the emitter is given, must stay first
  struct veto_event *list;       //!< the records so far
};

/**
 * @brief Take one event into the list
 *
 * @param ctx the context the emitter was given
 * @param xml the event as the log writes it
 * @param err receives the failure
 * @return false when the list could not take the event
 */
static NODISCARD bool emit_event(struct veto_event_context *const ctx, wchar_t const *const xml, struct ov_error *const err) {
  struct veto_event_context_private *const self = (struct veto_event_context_private *)ctx;
  struct veto_event item;
  bool success = false;

  memset(&item, 0, sizeof(item));
  if (!veto_event_parse_xml(xml, &item)) {
    // an event the reader makes nothing of is not one the log failed to write
    success = true;
    goto cleanup;
  }
  if (!OV_ARRAY_PUSH(&self->list, item)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(&item, 0, sizeof(item)); // the list owns the strings now
  success = true;

cleanup:
  if (item.path != NULL) {
    OV_ARRAY_DESTROY(&item.path);
  }
  if (item.device_instance != NULL) {
    OV_ARRAY_DESTROY(&item.device_instance);
  }
  return success;
}

/**
 * @brief Build a list of the records an emitter reads
 *
 * @param emitter called once, handing its events to ctx->emit
 * @param userdata handed to the emitter
 * @param err receives the failure of the emitter or of the list
 * @return the records, empty but never NULL when the emitter answered; NULL with err set
 *         when it did not
 */
NODISCARD struct veto_event *
veto_event_list_build(bool (*const emitter)(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err),
                      void *const userdata,
                      struct ov_error *const err) {
  struct veto_event_context_private self;
  struct veto_event *list = NULL;
  bool success = false;

  if (emitter == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  memset(&self, 0, sizeof(self));
  self.ctx.emit = emit_event;
  if (!emitter(&self.ctx, userdata, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if ((self.list == NULL) && !OV_ARRAY_GROW(&self.list, 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  list = self.list;
  self.list = NULL;
  success = true;

cleanup:
  if (!success) {
    if (self.list != NULL) {
      veto_event_destroy_list(&self.list);
    }
  }
  return success ? list : NULL;
}

/**
 * @brief Take the next record of the list
 *
 * @param list the list to walk, NULL walks nothing
 * @param it where the walk stands, zeroed before the first call
 * @return true when it now holds the next record, false when the walk is over
 */
NODISCARD bool veto_event_next(struct veto_event const *const list, struct veto_event_iter *const it) {
  size_t const n = OV_ARRAY_LENGTH(list);

  if ((it == NULL) || (it->idx >= n)) {
    return false;
  }
  it->pid = list[it->idx].pid;
  it->event_time_ms = list[it->idx].event_time_ms;
  it->path = list[it->idx].path;
  it->device_instance = list[it->idx].device_instance;
  it->idx++;
  return true;
}

/**
 * @brief Release the list and every record in it
 *
 * @param list set to NULL afterwards
 */
void veto_event_destroy_list(struct veto_event **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  size_t const n = OV_ARRAY_LENGTH(*list);
  for (size_t i = 0; i < n; i++) {
    if ((*list)[i].path != NULL) {
      OV_ARRAY_DESTROY(&(*list)[i].path);
    }
    if ((*list)[i].device_instance != NULL) {
      OV_ARRAY_DESTROY(&(*list)[i].device_instance);
    }
  }
  OV_ARRAY_DESTROY(list);
}
