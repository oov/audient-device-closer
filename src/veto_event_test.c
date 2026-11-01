#include "veto_event.c"

#include "mock/veto_log.h"

#include "veto_log_manager.h"

#include <ovtest.h>

#include <string.h>
#include <wchar.h>

#include <ovarray.h>

/**
 * @brief How many records the list holds
 *
 * The list is walked rather than asked, because that is the only way it is read.
 *
 * @param list the list to walk
 * @return the number of records
 */
static size_t list_length(struct veto_event const *const list) {
  struct veto_event_iter it = {0};
  size_t n = 0;

  while (veto_event_next(list, &it)) {
    n++;
  }
  return n;
}

static wchar_t const kXmlOtherApp[] =
    L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
    L"<System><Provider Name='Microsoft-Windows-Kernel-PnP' Guid='{9c205a39-1250-487d-abd7-e831c6290539}'/>"
    L"<EventID>225</EventID><Version>2</Version><Level>3</Level><Task>223</Task><Opcode>0</Opcode>"
    L"<Keywords>0x8000000000000000</Keywords>"
    L"<TimeCreated SystemTime='2000-01-01T00:00:00.0000000Z'/>"
    L"<EventRecordID>1</EventRecordID><Correlation/>"
    L"<Execution ProcessID='4' ThreadID='0'/>"
    L"<Channel>System</Channel><Computer>some-pc</Computer><Security UserID='S-1-5-18'/></System>"
    L"<EventData>"
    L"<Data Name='ProcessId'>4321</Data>"
    L"<Data Name='ProcessNameLength'>39</Data>"
    L"<Data Name='ProcessName'>\\Device\\HarddiskVolume1\\Apps\\holder.exe</Data>"
    L"<Data Name='DeviceInstanceLength'>35</Data>"
    L"<Data Name='DeviceInstance'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2</Data>"
    L"<Data Name='VetoingDevicesLength'>36</Data>"
    L"<Data Name='VetoingDevices'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2\n</Data>"
    L"</EventData></Event>";

static wchar_t const kXmlAudiodg[] = L"<Event><System><TimeCreated SystemTime='2000-01-01T00:00:00.2500000Z'/></System>"
                                     L"<EventData>"
                                     L"<Data Name='ProcessId'>5678</Data>"
                                     L"<Data Name='ProcessName'>\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe</Data>"
                                     L"<Data Name='DeviceInstance'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2</Data>"
                                     L"</EventData></Event>";

static wchar_t const kXmlEmpty[] = L"<Event><EventData></EventData></Event>";
static wchar_t const kXmlNoPid[] = L"<Event><EventData><Data Name='ProcessId'>0</Data></EventData></Event>";

/**
 * @brief Every field of a real veto record ends up in the right member
 */
static void test_parse_fields(void) {
  struct veto_event c;
  struct veto_event *list = NULL;

  memset(&c, 0, sizeof(c));
  if (!TEST_CHECK(veto_event_parse_xml(kXmlOtherApp, &c) == true)) {
    goto cleanup;
  }
  TEST_CHECK(c.pid == 4321);
  TEST_CHECK(c.device_instance != NULL && wcscmp(c.device_instance, L"USB\\VID_2708&PID_0008\\6&deadbee&0&2") == 0);
  TEST_CHECK(c.event_time_ms != 0);
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief The moment of the record is read from its SystemTime attribute
 */
static void test_parse_time(void) {
  struct veto_event c;
  struct veto_event d;
  struct veto_event *list = NULL;

  memset(&c, 0, sizeof(c));
  memset(&d, 0, sizeof(d));
  if (!TEST_CHECK(veto_event_parse_xml(kXmlOtherApp, &c) == true)) {
    goto cleanup;
  }
  if (!TEST_CHECK(veto_event_parse_xml(kXmlAudiodg, &d) == true)) {
    goto cleanup;
  }
  TEST_CHECK(d.event_time_ms > c.event_time_ms);
  TEST_CHECK((d.event_time_ms - c.event_time_ms) >= 200);
  TEST_CHECK((d.event_time_ms - c.event_time_ms) <= 300);
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));
  TEST_CHECK(OV_ARRAY_PUSH(&list, d));
  memset(&d, 0, sizeof(d));

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief A record that names no process is refused
 */
static void test_parse_garbage(void) {
  struct veto_event c;
  TEST_CHECK(veto_event_parse_xml(NULL, &c) == false);
  TEST_CHECK(veto_event_parse_xml(kXmlEmpty, &c) == false);
  TEST_CHECK(veto_event_parse_xml(kXmlNoPid, &c) == false);
}

/**
 * @brief A value is kept in full
 *
 * The parser carried every value through a fixed buffer on its way to the record, and a
 * device list longer than that buffer was cut without a word.
 */
static void test_parse_long_value(void) {
  static wchar_t const head[] = L"<Event><EventData><Data Name='ProcessId'>7</Data><Data Name='DeviceInstance'>";
  static wchar_t const tail[] = L"</Data></EventData></Event>";
  struct veto_event c;
  struct veto_event *list = NULL;
  wchar_t xml[(sizeof(head) / sizeof(head[0])) + 2048 + (sizeof(tail) / sizeof(tail[0]))];
  static size_t const want = 1500;

  memset(&c, 0, sizeof(c));
  memcpy(xml, head, sizeof(head) - sizeof(head[0]));
  for (size_t i = 0; i < want; i++) {
    xml[(sizeof(head) / sizeof(head[0])) - 1 + i] = L'x';
  }
  memcpy(&xml[(sizeof(head) / sizeof(head[0])) - 1 + want], tail, sizeof(tail));
  if (!TEST_CHECK(veto_event_parse_xml(xml, &c) == true)) {
    goto cleanup;
  }
  if (TEST_CHECK(c.device_instance != NULL)) {
    if (!TEST_CHECK(wcslen(c.device_instance) == want)) {
      TEST_MSG("want the device list to keep all %zu characters, got %zu", want, wcslen(c.device_instance));
    }
    TEST_CHECK(c.device_instance[0] == L'x');
    TEST_CHECK(c.device_instance[want - 1] == L'x');
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief The lookup goes through the log that is installed
 *
 * The whole point of the stand-in is that the program reads the log through one place, so
 * a script installed here has to be what the log manager answers with.
 */
static void test_mock_answers_the_lookup(void) {
  struct veto_log_mock_record const records[] = {
      {.pid = 4321,
       .event_time_ms = 1000,
       .path = L"\\Device\\HarddiskVolume1\\Apps\\holder.exe",
       .device_instance = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2"},
      {.pid = 5678,
       .event_time_ms = 2000,
       .path = L"\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe",
       .device_instance = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2"},
  };
  struct veto_log_mock_script const script = {
      .records = records,
      .record_count = sizeof(records) / sizeof(records[0]),
  };
  struct veto_log_wait *lookup = NULL;
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  TEST_ASSERT(veto_log_mock_install(&script));
  lookup = veto_log_manager()->wait(L"System", L"Microsoft-Windows-Kernel-PnP", 225, 500, 1000, NULL, NULL, &err);
  if (TEST_CHECK(lookup != NULL)) {
    list = veto_log_manager()->wait_take(&lookup);
  }
  if (TEST_CHECK(list != NULL)) {
    TEST_CHECK_(list_length(list) == 2, "want the two records of the script, got %zu", list_length(list));
    TEST_CHECK(list[0].pid == 4321);
    TEST_CHECK(list[1].pid == 5678);
    TEST_CHECK(wcscmp(list[0].device_instance, L"USB\\VID_2708&PID_0008\\6&deadbee&0&2") == 0);
    TEST_CHECK_(veto_log_mock_read_count() == 1, "want one lookup, got %zu", veto_log_mock_read_count());
  }

  TEST_CASE("the script handed back what the caller asked for");
  TEST_CHECK(veto_log_mock_since_ms() == 500);
  TEST_CHECK(veto_log_mock_max_wait_ms() == 1000);

  veto_event_destroy_list(&list);
  veto_log_mock_reset();
}

/**
 * @brief A stand-in with no record answers an empty list, not a failure
 */
static void test_mock_answers_nothing(void) {
  struct veto_log_mock_script const script = {0};
  struct veto_log_wait *lookup = NULL;
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  TEST_ASSERT(veto_log_mock_install(&script));
  lookup = veto_log_manager()->wait(L"System", L"Microsoft-Windows-Kernel-PnP", 225, 0, 1000, NULL, NULL, &err);
  if (TEST_CHECK(lookup != NULL)) {
    list = veto_log_manager()->wait_take(&lookup);
  }
  if (TEST_CHECK(list != NULL)) {
    TEST_CHECK_(list_length(list) == 0, "want an empty list, got %zu", list_length(list));
  }
  veto_event_destroy_list(&list);
  veto_log_mock_reset();
}

/**
 * @brief The stand-in speaks the same language as the log it stands in for
 *
 * The script names the parts of a record, and the log it stands in for writes them as an
 * event of XML; a stand-in that handed the parts over directly would answer from a path of
 * its own, and the run would be tested against a reader no machine has.
 */
static void test_mock_record_survives_the_reader(void) {
  struct veto_log_mock_record const records[] = {
      {.pid = 4321,
       .event_time_ms = 1000,
       .path = L"\\Device\\HarddiskVolume1\\Apps\\holder.exe",
       .device_instance = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2"},
  };
  struct veto_log_mock_script const script = {
      .records = records,
      .record_count = sizeof(records) / sizeof(records[0]),
  };
  struct veto_log_wait *lookup = NULL;
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  TEST_ASSERT(veto_log_mock_install(&script));
  lookup = veto_log_manager()->wait(L"System", L"Microsoft-Windows-Kernel-PnP", 225, 0, 1000, NULL, NULL, &err);
  if (TEST_CHECK(lookup != NULL)) {
    list = veto_log_manager()->wait_take(&lookup);
  }
  if (TEST_CHECK(list != NULL)) {
    TEST_CHECK_(list_length(list) == 1, "want the one record, got %zu", list_length(list));
    TEST_CHECK(list[0].pid == 4321);
    TEST_CHECK(list[0].event_time_ms == 1000);
    TEST_CHECK_(list[0].path != NULL && wcscmp(list[0].path, records[0].path) == 0, "want the path of the script back");
    TEST_CHECK_(list[0].device_instance != NULL && wcscmp(list[0].device_instance, records[0].device_instance) == 0,
                "want the device of the script back");
  }
  veto_event_destroy_list(&list);
  veto_log_mock_reset();
}

/**
 * @brief A record the reader refuses is not one the stand-in hands over
 *
 * The stand-in is the log of the system, and the log holds records the reader cannot make
 * anything of; the answer of the stand-in must be what that reader makes of them.
 */
static void test_mock_drops_a_record_the_reader_refuses(void) {
  struct veto_log_mock_record const records[] = {
      {.pid = 0, .event_time_ms = 1000, .path = L"holder.exe", .device_instance = L"USB\\dev"},
      {.pid = 500, .event_time_ms = 2000, .path = L"good.exe", .device_instance = L"USB\\dev"},
  };
  struct veto_log_mock_script const script = {
      .records = records,
      .record_count = sizeof(records) / sizeof(records[0]),
  };
  struct veto_log_wait *lookup = NULL;
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  TEST_ASSERT(veto_log_mock_install(&script));
  lookup = veto_log_manager()->wait(L"System", L"Microsoft-Windows-Kernel-PnP", 225, 0, 1000, NULL, NULL, &err);
  if (TEST_CHECK(lookup != NULL)) {
    list = veto_log_manager()->wait_take(&lookup);
  }
  if (TEST_CHECK(list != NULL)) {
    if (TEST_CHECK_(list_length(list) == 1, "want the one readable record, got %zu", list_length(list))) {
      TEST_CHECK(list[0].pid == 500);
    }
  }
  veto_event_destroy_list(&list);
  veto_log_mock_reset();
}

/**
 * @brief What the emitter of a check hands to the list
 */
struct emit_case {
  wchar_t const *const *xmls; //!< the events to hand over
  size_t count;               //!< how many there are
  bool fail;                  //!< whether the emitter itself fails
  size_t emitted;             //!< how many events were handed over
};

/**
 * @brief Hand every event of the case over, the way a log would
 */
static bool emit_case(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err) {
  struct emit_case *const c = (struct emit_case *)userdata;
  bool success = false;

  for (size_t i = 0; i < c->count; i++) {
    if (!ctx->emit(ctx, c->xmls[i], err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    c->emitted++;
  }
  if (c->fail) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief An event the list is built from becomes a record of it
 */
static void test_list_build_reads_what_the_emitter_hands_over(void) {
  wchar_t const *const xmls[] = {kXmlOtherApp, kXmlAudiodg};
  struct emit_case c = {.xmls = xmls, .count = sizeof(xmls) / sizeof(xmls[0])};
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  list = veto_event_list_build(emit_case, &c, &err);
  if (!TEST_SUCCEEDED(list != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(list_length(list) == 2, "want the two events of the emitter, got %zu", list_length(list));
  TEST_CHECK(list[0].pid == 4321);
  TEST_CHECK(list[1].pid == 5678);

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief An emitter that hands over nothing answers an empty list, not a failure
 *
 * A record that is not there is an answer: the machine had nothing to say about this
 * attempt.  A failure is when the log could not be read at all.
 */
static void test_list_build_withholds_an_empty_list(void) {
  struct emit_case c = {0};
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  list = veto_event_list_build(emit_case, &c, &err);
  if (!TEST_SUCCEEDED(list != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK_(list_length(list) == 0, "want an empty list, got %zu", list_length(list));

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief An emitter that fails answers nothing, and the reason is the caller's
 */
static void test_list_build_answers_the_failure_of_the_emitter(void) {
  wchar_t const *const xmls[] = {kXmlOtherApp};
  struct emit_case c = {.xmls = xmls, .count = 1, .fail = true};
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  list = veto_event_list_build(emit_case, &c, &err);
  TEST_FAILED_WITH(list != NULL || ov_error_is(&err, ov_error_type_generic, ov_error_generic_fail) != true,
                   &err,
                   ov_error_type_generic,
                   ov_error_generic_fail);
  veto_event_destroy_list(&list);
}

/**
 * @brief An event the reader makes nothing of is left out, not a failure
 *
 * The log of a machine holds records of every kind, and the ones this reader cannot make a
 * record of are not a reason to give up on the ones it can.
 */
static void test_list_build_leaves_out_what_it_cannot_read(void) {
  wchar_t const *const xmls[] = {kXmlNoPid, kXmlOtherApp, kXmlEmpty};
  struct emit_case c = {.xmls = xmls, .count = sizeof(xmls) / sizeof(xmls[0])};
  struct veto_event *list = NULL;
  struct ov_error err = {0};

  list = veto_event_list_build(emit_case, &c, &err);
  if (!TEST_SUCCEEDED(list != NULL, &err)) {
    goto cleanup;
  }
  if (TEST_CHECK_(list_length(list) == 1, "want only the readable event, got %zu", list_length(list))) {
    TEST_CHECK(list[0].pid == 4321);
  }

cleanup:
  veto_event_destroy_list(&list);
}

TEST_LIST = {
    {"parse_fields", test_parse_fields},
    {"parse_time", test_parse_time},
    {"parse_garbage", test_parse_garbage},
    {"parse_long_value", test_parse_long_value},
    {"mock_answers_the_lookup", test_mock_answers_the_lookup},
    {"mock_answers_nothing", test_mock_answers_nothing},
    {"mock_record_survives_the_reader", test_mock_record_survives_the_reader},
    {"mock_drops_a_record_the_reader_refuses", test_mock_drops_a_record_the_reader_refuses},
    {"list_build_reads_what_the_emitter_hands_over", test_list_build_reads_what_the_emitter_hands_over},
    {"list_build_withholds_an_empty_list", test_list_build_withholds_an_empty_list},
    {"list_build_answers_the_failure_of_the_emitter", test_list_build_answers_the_failure_of_the_emitter},
    {"list_build_leaves_out_what_it_cannot_read", test_list_build_leaves_out_what_it_cannot_read},
    {NULL, NULL},
};
