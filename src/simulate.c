#include "simulate.h"

#ifdef ADC_MOCK

#  include <wchar.h>

#  include "mock/clock.h"
#  include "mock/device.h"
#  include "mock/process.h"
#  include "mock/removal.h"
#  include "mock/veto_log.h"

/**
 * @brief The device the made-up fault is about
 *
 * The KS devnode sits under the USB instance, which is the ancestor the removal record
 * names, the way it does on a machine where the sleep took the endpoints of a unit.
 */
static wchar_t const kKsDevnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
static wchar_t const kUsbInstance[] = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2";
static wchar_t const kService[] = L"audientusbaudioks";

/** @brief The processes of the made-up machine, walked in place so `ended` sticks */
static struct process_mock_entry g_processes[] = {
    {.pid = 4000, .image = L"iD.exe", .start_ms = 1000, .ends = true},
};

/** @brief The facts of the device once it is back */
static struct device_facts const kRestored = {.problem_code = 0, .endpoint_present = true, .endpoint_registered = true};

/** @brief The facts of the device the made-up fault is about */
static struct device_facts const kFaulted = {.problem_code = 0, .endpoint_present = false, .endpoint_registered = true};

/** @brief The facts of a device that works */
static struct device_facts const kHealthy = {.problem_code = 0, .endpoint_present = true, .endpoint_registered = true};

/** @brief The devnodes of the made-up machine, walked in place so the facts can change */
static struct device_mock_node g_devnodes[] = {
    {.devnode = kKsDevnode, .parent = kUsbInstance, .facts = kFaulted, .restored = kRestored},
    {.devnode = kUsbInstance, .facts = kHealthy},
};

/** @brief The one record the made-up machine keeps, so it can be pointed at */
static struct veto_log_mock_record g_records[1];

/**
 * @brief The device comes back once the removal is let through
 *
 * The removal of a stand-in the run succeeded on is what puts the endpoints back on the
 * bus, so a run that gets that far takes the device through the confirmation.
 */
static void on_removal_success(void *const userdata) {
  (void)userdata;
  device_mock_set_facts(kKsDevnode, &kRestored);
}

/**
 * @brief Make the stand-ins of one made-up machine
 *
 * Every script is built whole, so a field a stand-in grows later is off rather than whatever
 * the stack happened to hold: one filled in field by field is a script a new field quietly
 * joins uninitialized.
 *
 * @param devnodes the nodes the service has, may be NULL when it has none
 * @param node_count how many that is
 * @param list_fails whether the nodes cannot be listed at all
 * @param removals what the removal answers
 * @return true when every stand-in was taken
 */
static bool install(struct device_mock_node const *const devnodes,
                    size_t const node_count,
                    bool const list_fails,
                    struct removal_mock_script const *const removals) {
  struct device_mock_script const devices = {.service = kService, .nodes = devnodes, .node_count = node_count, .list_fails = list_fails};

  g_records[0].pid = g_processes[0].pid;
  g_records[0].event_time_ms = g_processes[0].start_ms + 500; // the record is of the process, not older than it
  g_records[0].path = L"\\Device\\HarddiskVolume1\\Apps\\iD.exe";
  g_records[0].device_instance = kUsbInstance;
  {
    struct process_mock_script const processes = {.entries = g_processes, .entry_count = sizeof(g_processes) / sizeof(g_processes[0])};
    struct veto_log_mock_script const log = {.records = g_records, .record_count = 1};

    if (!process_mock_install(&processes) || !device_mock_install(&devices) || !removal_mock_install(removals) ||
        !veto_log_mock_install(&log)) {
      // a stand-in that cannot be installed is a debug aid that did not come up, and the
      // machine the program is on is the safer answer
      process_mock_reset();
      device_mock_reset();
      removal_mock_reset();
      veto_log_mock_reset();
      return false;
    }
  }
  clock_mock_install(&(struct clock_mock_script){.now_ms = 2000});
  return true;
}

/** @brief The nodes the stand-in has, put back to the fault before every scenario */
static size_t const kNodeCount = sizeof(g_devnodes) / sizeof(g_devnodes[0]);

static void reset_nodes(void) {
  g_devnodes[0].facts = kFaulted;
  g_devnodes[0].restored = kRestored;
  g_devnodes[1].facts = kHealthy;
}

bool simulate_install(wchar_t const *const scenario) {
  reset_nodes();
  if (wcscmp(scenario, L"slow_removal") == 0) {
    struct removal_mock_script const removals = {.refusals = 0, .delay_ms = 3000, .on_success = on_removal_success};
    return install(g_devnodes, kNodeCount, false, &removals);
  }
  if (wcscmp(scenario, L"access_denied") == 0) {
    struct removal_mock_script const removals = {.refusals = 1, .code = app_error_removal_access_denied, .delay_ms = 3000};
    return install(g_devnodes, kNodeCount, false, &removals);
  }
  if (wcscmp(scenario, L"unreadable") == 0) {
    struct removal_mock_script const removals = {0};
    return install(g_devnodes, kNodeCount, true, &removals);
  }
  if (wcscmp(scenario, L"no_device") == 0) {
    struct removal_mock_script const removals = {0};
    return install(NULL, 0, false, &removals);
  }
  return false;
}

#endif // ADC_MOCK
