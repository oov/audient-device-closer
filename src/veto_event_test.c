#include "veto_event.c"

#include <ovtest.h>

#include <string.h>

#include <ovarray.h>

static char const kXmlOtherApp[] = "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
                                   "<System><Provider Name='Microsoft-Windows-Kernel-PnP' Guid='{9c205a39-1250-487d-abd7-e831c6290539}'/>"
                                   "<EventID>225</EventID><Version>2</Version><Level>3</Level><Task>223</Task><Opcode>0</Opcode>"
                                   "<Keywords>0x8000000000000000</Keywords>"
                                   "<TimeCreated SystemTime='2000-01-01T00:00:00.0000000Z'/>"
                                   "<EventRecordID>1</EventRecordID><Correlation/>"
                                   "<Execution ProcessID='4' ThreadID='0'/>"
                                   "<Channel>System</Channel><Computer>some-pc</Computer><Security UserID='S-1-5-18'/></System>"
                                   "<EventData>"
                                   "<Data Name='ProcessId'>4321</Data>"
                                   "<Data Name='ProcessNameLength'>39</Data>"
                                   "<Data Name='ProcessName'>\\Device\\HarddiskVolume1\\Apps\\holder.exe</Data>"
                                   "<Data Name='DeviceInstanceLength'>35</Data>"
                                   "<Data Name='DeviceInstance'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2</Data>"
                                   "<Data Name='VetoingDevicesLength'>36</Data>"
                                   "<Data Name='VetoingDevices'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2\n</Data>"
                                   "</EventData></Event>";

static char const kXmlAudiodg[] = "<Event><System><TimeCreated SystemTime='2000-01-01T00:00:00.2500000Z'/></System>"
                                  "<EventData>"
                                  "<Data Name='ProcessId'>5678</Data>"
                                  "<Data Name='ProcessName'>\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe</Data>"
                                  "<Data Name='DeviceInstance'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2</Data>"
                                  "</EventData></Event>";

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
  TEST_CHECK(c.device_instance != NULL && strcmp(c.device_instance, "USB\\VID_2708&PID_0008\\6&deadbee&0&2") == 0);
  TEST_CHECK(c.event_time_ms != 0);
  veto_event_prune(NULL, 0);
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
  TEST_CHECK(veto_event_parse_xml("<Event><EventData></EventData></Event>", &c) == false);
  TEST_CHECK(veto_event_parse_xml("<Event><EventData><Data Name='ProcessId'>0</Data></EventData></Event>", &c) == false);
}

/**
 * @brief A value is kept in full
 *
 * The parser carried every value through a fixed buffer on its way to the record, and a
 * device list longer than that buffer was cut without a word.
 */
static void test_parse_long_value(void) {
  static char const head[] = "<Event><EventData><Data Name='ProcessId'>7</Data><Data Name='DeviceInstance'>";
  static char const tail[] = "</Data></EventData></Event>";
  struct veto_event c;
  struct veto_event *list = NULL;
  char xml[sizeof(head) + 2048 + sizeof(tail)];
  static size_t const want = 1500;

  memset(&c, 0, sizeof(c));
  memcpy(xml, head, sizeof(head) - 1);
  memset(xml + (sizeof(head) - 1), 'x', want);
  memcpy(xml + (sizeof(head) - 1) + want, tail, sizeof(tail));
  if (!TEST_CHECK(veto_event_parse_xml(xml, &c) == true)) {
    goto cleanup;
  }
  if (TEST_CHECK(c.device_instance != NULL)) {
    if (!TEST_CHECK(strlen(c.device_instance) == want)) {
      TEST_MSG("want the device list to keep all %zu characters, got %zu", want, strlen(c.device_instance));
    }
    TEST_CHECK(c.device_instance[0] == 'x');
    TEST_CHECK(c.device_instance[want - 1] == 'x');
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief A record for the lists of the other checks
 *
 * @param pid the process the record names
 * @param device the device the record names
 * @param ms the moment of the record
 * @return the record to hand to the list helpers
 */
static struct veto_event make_item(uint32_t const pid, char const *const device, uint64_t const ms) {
  struct veto_event c;
  memset(&c, 0, sizeof(c));
  c.pid = pid;
  c.event_time_ms = ms;
  if (device) {
    size_t const len = strlen(device) + 1;
    if (OV_REALLOC(&c.device_instance, len, sizeof(c.device_instance[0]))) {
      memcpy(c.device_instance, device, len);
    }
  }
  return c;
}

/**
 * @brief The newest of the duplicates is the one that survives
 */
static void test_prune_duplicates_keeps_newest(void) {
  struct veto_event *list = NULL;
  struct veto_event a = make_item(100, "USB\\dev", 1000);
  struct veto_event b = make_item(100, "USB\\dev", 2000); // newer duplicate
  struct veto_event d = make_item(100, "TUSBAUDIO\\ks", 1500);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  TEST_CHECK(OV_ARRAY_PUSH(&list, d));
  veto_event_prune(&list, 0);
  TEST_CHECK(OV_ARRAY_LENGTH(list) == 2);
  for (size_t i = 0; i < OV_ARRAY_LENGTH(list); i++) {
    if (list[i].device_instance && strcmp(list[i].device_instance, "USB\\dev") == 0) {
      TEST_CHECK(list[i].event_time_ms == 2000);
    }
  }

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief Records older than the window are gone
 */
static void test_prune_since(void) {
  struct veto_event *list = NULL;
  struct veto_event a = make_item(1, "USB\\a", 100);
  struct veto_event b = make_item(2, "USB\\b", 500);
  struct veto_event c = make_item(3, "USB\\c", 900);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  veto_event_prune(&list, 500);
  TEST_CHECK(OV_ARRAY_LENGTH(list) == 2);
  TEST_CHECK(list[0].pid == 2);
  TEST_CHECK(list[1].pid == 3);

cleanup:
  veto_event_destroy_list(&list);
}

/**
 * @brief A prune that drops nothing must not shuffle the list
 */
static void test_prune_empty(void) {
  struct veto_event *list = NULL;
  veto_event_prune(&list, 10);
  TEST_CHECK(list == NULL);
  veto_event_destroy_list(&list);
}

TEST_LIST = {
    {"parse_fields", test_parse_fields},
    {"parse_time", test_parse_time},
    {"parse_garbage", test_parse_garbage},
    {"parse_long_value", test_parse_long_value},
    {"prune_duplicates", test_prune_duplicates_keeps_newest},
    {"prune_since", test_prune_since},
    {"prune_empty", test_prune_empty},
    {NULL, NULL},
};
