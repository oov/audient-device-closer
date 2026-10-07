#include <ovtest.h>

#include <string.h>

#include <ovarray.h>

#include "culprit.h"

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
                                   "<Data Name='CommandLineLength'>21</Data>"
                                   "<Data Name='CommandLine'>&quot;C:\\Apps\\holder.exe&quot; </Data>"
                                   "<Data Name='VetoingDevicesLength'>36</Data>"
                                   "<Data Name='VetoingDevices'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2\n</Data>"
                                   "</EventData></Event>";

static char const kXmlAudiodg[] = "<Event><System><TimeCreated SystemTime='2000-01-01T00:00:00.2500000Z'/></System>"
                                  "<EventData>"
                                  "<Data Name='ProcessId'>5678</Data>"
                                  "<Data Name='ProcessName'>\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe</Data>"
                                  "<Data Name='DeviceInstance'>USB\\VID_2708&amp;PID_0008\\6&amp;deadbee&amp;0&amp;2</Data>"
                                  "<Data Name='CommandLine'>C:\\WINDOWS\\system32\\AUDIODG.EXE 0x0000000000000000</Data>"
                                  "</EventData></Event>";

/**
 * @brief Every field of a real veto record ends up in the right member
 */
static void test_parse_fields(void) {
  struct culprit c;
  struct culprit *list = NULL;

  memset(&c, 0, sizeof(c));
  if (!TEST_CHECK(culprit_parse_xml(kXmlOtherApp, &c) == true)) {
    goto cleanup;
  }
  TEST_CHECK(c.pid == 4321);
  TEST_CHECK(c.path != NULL && strcmp(c.path, "\\Device\\HarddiskVolume1\\Apps\\holder.exe") == 0);
  TEST_CHECK(c.device_instance != NULL && strcmp(c.device_instance, "USB\\VID_2708&PID_0008\\6&deadbee&0&2") == 0);
  TEST_CHECK(c.command_line != NULL && strcmp(c.command_line, "\"C:\\Apps\\holder.exe\" ") == 0);
  TEST_CHECK(c.event_time_ms != 0);
  culprit_prune(NULL, 0);
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief The moment of the record is read from its SystemTime attribute
 */
static void test_parse_time(void) {
  struct culprit c;
  struct culprit d;
  struct culprit *list = NULL;

  memset(&c, 0, sizeof(c));
  memset(&d, 0, sizeof(d));
  if (!TEST_CHECK(culprit_parse_xml(kXmlOtherApp, &c) == true)) {
    goto cleanup;
  }
  if (!TEST_CHECK(culprit_parse_xml(kXmlAudiodg, &d) == true)) {
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
  culprit_destroy_list(&list);
}

/**
 * @brief A record that names no process is refused
 */
static void test_parse_garbage(void) {
  struct culprit c;
  TEST_CHECK(culprit_parse_xml(NULL, &c) == false);
  TEST_CHECK(culprit_parse_xml("<Event><EventData></EventData></Event>", &c) == false);
  TEST_CHECK(culprit_parse_xml("<Event><EventData><Data Name='ProcessId'>0</Data></EventData></Event>", &c) == false);
}

/**
 * @brief A value is kept in full
 *
 * The parser carried every value through a fixed buffer on its way to the record, and a
 * command line longer than that buffer was cut without a word.
 */
static void test_parse_long_value(void) {
  static char const head[] = "<Event><EventData><Data Name='ProcessId'>7</Data><Data Name='CommandLine'>";
  static char const tail[] = "</Data></EventData></Event>";
  struct culprit c;
  struct culprit *list = NULL;
  char xml[sizeof(head) + 2048 + sizeof(tail)];
  static size_t const want = 1500;

  memset(&c, 0, sizeof(c));
  memcpy(xml, head, sizeof(head) - 1);
  memset(xml + (sizeof(head) - 1), 'x', want);
  memcpy(xml + (sizeof(head) - 1) + want, tail, sizeof(tail));
  if (!TEST_CHECK(culprit_parse_xml(xml, &c) == true)) {
    goto cleanup;
  }
  if (TEST_CHECK(c.command_line != NULL)) {
    if (!TEST_CHECK(strlen(c.command_line) == want)) {
      TEST_MSG("want the command line to keep all %zu characters, got %zu", want, strlen(c.command_line));
    }
    TEST_CHECK(c.command_line[0] == 'x');
    TEST_CHECK(c.command_line[want - 1] == 'x');
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  memset(&c, 0, sizeof(c));

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief A record for the lists of the other checks
 *
 * @param pid the process the record names
 * @param device the device the record names
 * @param ms the moment of the record
 * @return the record to hand to the list helpers
 */
static struct culprit make_item(uint32_t const pid, char const *const device, long long const ms) {
  struct culprit c;
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
  struct culprit *list = NULL;
  struct culprit a = make_item(100, "USB\\dev", 1000);
  struct culprit b = make_item(100, "USB\\dev", 2000); // newer duplicate
  struct culprit d = make_item(100, "TUSBAUDIO\\ks", 1500);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  TEST_CHECK(OV_ARRAY_PUSH(&list, d));
  culprit_prune(&list, 0);
  TEST_CHECK(OV_ARRAY_LENGTH(list) == 2);
  for (size_t i = 0; i < OV_ARRAY_LENGTH(list); i++) {
    if (list[i].device_instance && strcmp(list[i].device_instance, "USB\\dev") == 0) {
      TEST_CHECK(list[i].event_time_ms == 2000);
    }
  }

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief Records older than the window are gone
 */
static void test_prune_since(void) {
  struct culprit *list = NULL;
  struct culprit a = make_item(1, "USB\\a", 100);
  struct culprit b = make_item(2, "USB\\b", 500);
  struct culprit c = make_item(3, "USB\\c", 900);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  culprit_prune(&list, 500);
  TEST_CHECK(OV_ARRAY_LENGTH(list) == 2);
  TEST_CHECK(list[0].pid == 2);
  TEST_CHECK(list[1].pid == 3);

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief A prune that drops nothing must not shuffle the list
 */
static void test_prune_empty(void) {
  struct culprit *list = NULL;
  culprit_prune(&list, 10);
  TEST_CHECK(list == NULL);
  culprit_destroy_list(&list);
}

/**
 * @brief A process this tool closed is not a reason the device was blocked
 *
 * The list the user is shown must not name it, or the dialog contradicts what the user
 * just watched happen.
 */
static void test_drop_pids(void) {
  struct culprit *list = NULL;
  static uint32_t const gone[] = {100, 300};
  struct culprit a = make_item(100, "USB\\dev", 1000); // closed by this run
  struct culprit b = make_item(200, "USB\\dev", 2000); // still blocking
  struct culprit c = make_item(300, "USB\\dev", 3000); // closed by this run
  struct culprit d = make_item(400, "USB\\dev", 4000); // still blocking

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  TEST_CHECK(OV_ARRAY_PUSH(&list, c));
  TEST_CHECK(OV_ARRAY_PUSH(&list, d));

  culprit_drop_pids(&list, gone, sizeof(gone) / sizeof(gone[0]));
  if (!TEST_CHECK(OV_ARRAY_LENGTH(list) == 2)) {
    goto cleanup;
  }
  TEST_CHECK(list[0].pid == 200);
  TEST_CHECK(list[1].pid == 400);

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief Dropping everything leaves an empty list, not a list that still holds dead entries
 */
static void test_drop_pids_all(void) {
  struct culprit *list = NULL;
  static uint32_t const gone[] = {100, 200};
  struct culprit a = make_item(100, "USB\\dev", 1000);
  struct culprit b = make_item(200, "USB\\dev", 2000);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  culprit_drop_pids(&list, gone, sizeof(gone) / sizeof(gone[0]));
  TEST_CHECK(culprit_count(list) == 0);

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief An empty set of pids must not touch the list, and the arguments may be absent
 */
static void test_drop_pids_noop(void) {
  struct culprit *list = NULL;
  static uint32_t const gone[] = {999};
  struct culprit a = make_item(100, "USB\\dev", 1000);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  culprit_drop_pids(&list, gone, 0);
  TEST_CHECK(culprit_count(list) == 1);
  culprit_drop_pids(&list, NULL, 1);
  TEST_CHECK(culprit_count(list) == 1);
  culprit_drop_pids(NULL, gone, 1);
  TEST_CHECK(culprit_count(list) == 1);

cleanup:
  culprit_destroy_list(&list);
}

/**
 * @brief A list kept from an earlier attempt is history once the device was closed
 *
 * Nothing blocks the device any more, so the records must not reach the user.
 */
static void test_clear(void) {
  struct culprit *list = NULL;
  struct culprit a = make_item(100, "USB\\dev", 1000);
  struct culprit b = make_item(200, "USB\\dev", 2000);

  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, a))) {
    goto cleanup;
  }
  TEST_CHECK(OV_ARRAY_PUSH(&list, b));
  culprit_clear(&list);
  TEST_CHECK(culprit_count(list) == 0);
  culprit_clear(&list);
  TEST_CHECK(culprit_count(list) == 0);
  culprit_clear(NULL);

cleanup:
  culprit_destroy_list(&list);
}

TEST_LIST = {
    {"parse_fields", test_parse_fields},
    {"parse_time", test_parse_time},
    {"parse_garbage", test_parse_garbage},
    {"parse_long_value", test_parse_long_value},
    {"prune_duplicates", test_prune_duplicates_keeps_newest},
    {"prune_since", test_prune_since},
    {"prune_empty", test_prune_empty},
    {"drop_pids", test_drop_pids},
    {"drop_pids_all", test_drop_pids_all},
    {"drop_pids_noop", test_drop_pids_noop},
    {"clear", test_clear},
    {NULL, NULL},
};
