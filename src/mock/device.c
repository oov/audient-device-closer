#include "mock/device.h"

#include <windows.h>

#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#ifdef ADC_MOCK
/** @brief How long the stand-in waits between two looks while the device is not back */
enum { adc_mock_watch_step_ms = 10 };

/** @brief The nodes of the installed script, copied from what the caller handed over */
static struct device_mock_node *g_nodes = NULL;
/** @brief The service the installed nodes belong to */
static wchar_t const *g_service = NULL;

/** @brief Whether the stand-in cannot list its devnodes */
static bool g_list_fails = false;
/** @brief Whether a script is installed at all */
static bool g_installed = false;

/**
 * @brief The node of a devnode, NULL when the script has none of that name
 *
 * @param devnode the devnode to look for
 * @return the node, or NULL
 */
static struct device_mock_node *find_node(wchar_t const *const devnode) {
  size_t const n = (g_nodes != NULL) ? OV_ARRAY_LENGTH(g_nodes) : 0;

  for (size_t i = 0; i < n; i++) {
    if (wcscmp(g_nodes[i].devnode, devnode) == 0) {
      return &g_nodes[i];
    }
  }
  return NULL;
}

static NODISCARD bool foreach_devnode(wchar_t const *const service,
                                      bool (*on_devnode)(wchar_t const *devnode, void *ctx, struct ov_error *err),
                                      void *const ctx,
                                      struct ov_error *const err) {
  size_t const n = (g_nodes != NULL) ? OV_ARRAY_LENGTH(g_nodes) : 0;

  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if ((g_service == NULL) || (wcscmp(service, g_service) != 0)) {
    return true; // this service is not the one the script describes, so it has no devnode
  }
  if (g_list_fails) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!on_devnode(g_nodes[i].devnode, ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
  }
  return true;
}

static NODISCARD bool facts_of(wchar_t const *const devnode, struct device_facts *const out, struct ov_error *const err) {
  struct device_mock_node const *const node = find_node(devnode);

  if (node == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    return false;
  }
  *out = node->facts;
  return true;
}

static NODISCARD ov_tribool parent_of(wchar_t const *const devnode, wchar_t **const buf, struct ov_error *const err) {
  struct device_mock_node const *const node = find_node(devnode);

  if (node == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    return ov_indeterminate;
  }
  if (node->parent == NULL) {
    return ov_false; // this node is at the root of the script
  }
  if (!ov_sprintf_wchar(buf, err, NULL, L"%ls", node->parent)) {
    OV_ERROR_ADD_TRACE(err);
    return ov_indeterminate;
  }
  return ov_true;
}

/** @brief One watch of the stand-in, answered as soon as it is asked for */
struct device_watch {
  async_notify_fn notify;    //!< the caller's call, may be NULL
  void *userdata;            //!< handed to notify
  struct device_facts facts; //!< what the last look that could be taken reported
  struct ov_error *err;      //!< borrowed, the caller's
  bool failed;               //!< whether the last look could not be taken
};

/** @brief One watch of the stand-in, answered on the thread that asked for it */
static NODISCARD struct device_watch *watch(wchar_t const *const devnode,
                                            unsigned long const max_wait_ms,
                                            bool (*const on_update_device)(struct device_facts const *facts),
                                            async_notify_fn const notify,
                                            void *const userdata,
                                            struct ov_error *const err) {
  struct device_mock_node *const node = find_node(devnode);
  struct device_watch *w = NULL;
  ULONGLONG const deadline = GetTickCount64() + max_wait_ms;

  if (on_update_device == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (node == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
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
  if (node->look_fails) {
    w->failed = true;
    OV_ERROR_SET_GENERIC(w->err, ov_error_generic_fail);
  } else {
    // the answer is the caller's, not the stand-in's: it is asked again and again, and the
    // bound is what keeps a device that never comes back from holding the wait forever
    struct device_facts facts = node->facts;

    for (;;) {
      w->facts = facts;
      if (on_update_device(&facts)) {
        break;
      }
      if (GetTickCount64() >= deadline) {
        break;
      }
      // the script says what the node reports from the second look on, so the node itself
      // keeps what the test wrote
      facts = node->restored;
      Sleep(adc_mock_watch_step_ms);
    }
  }
  if (notify != NULL) {
    notify(userdata, false);
  }
  return w;
}

static NODISCARD struct device_facts watch_take(struct device_watch **const w, struct ov_error *const err) {
  struct device_facts facts = {.problem_code = DEVICE_PROBLEM_NO_REPORT};

  if ((w == NULL) || (*w == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if ((*w)->failed) {
    facts = (struct device_facts){.problem_code = DEVICE_PROBLEM_NO_REPORT};
  } else {
    facts = (*w)->facts;
  }
  OV_FREE(w);

cleanup:
  return facts;
}

static void watch_cancel(struct device_watch *const w) {
  (void)w; // the stand-in is already answered by the time it is asked for
}

/**
 * @brief The sentence this program has for one problem the machine named
 *
 * The stand-in has two: none for a devnode it reports no problem of, and one for every
 * other code.  Telling the codes themselves apart is the machine's own business, which is
 * what the test of the real one is for.
 *
 * @param problem_code the number the script reported
 * @return the identifier to translate, NULL when there is no sentence to show
 */
static char const *problem_id(uint32_t const problem_code) {
  if (problem_code == 0) {
    return NULL;
  }
  return gettext_noop("The device has a problem that this tool cannot solve.");
}

/** @brief The stand-in machine, answering what the installed script says */
static struct device_manager const kMockDeviceManager = {
    .foreach_devnode = foreach_devnode,
    .facts_of = facts_of,
    .parent_of = parent_of,
    .problem_id = problem_id,
    .watch = watch,
    .watch_take = watch_take,
    .watch_cancel = watch_cancel,
};

static struct device_manager const *manager(void) { return &kMockDeviceManager; }

bool device_mock_install(struct device_mock_script const *const script) {
  if ((script == NULL) || (script->service == NULL)) {
    return false;
  }
  device_mock_reset();
  if (script->node_count > 0) {
    if (!OV_ARRAY_GROW(&g_nodes, script->node_count)) {
      return false;
    }
    memcpy(g_nodes, script->nodes, script->node_count * sizeof(g_nodes[0]));
    OV_ARRAY_SET_LENGTH(g_nodes, script->node_count);
  }
  g_service = script->service;
  g_list_fails = script->list_fails;
  g_installed = true;
  device_manager_select(manager());
  return true;
}

bool device_mock_set_facts(wchar_t const *const devnode, struct device_facts const *const facts) {
  struct device_mock_node *const node = find_node(devnode);

  if (node == NULL) {
    return false;
  }
  node->facts = *facts;
  return true;
}

void device_mock_reset(void) {
  if (g_nodes != NULL) {
    OV_ARRAY_DESTROY(&g_nodes);
  }
  g_service = NULL;
  g_list_fails = false;
  g_installed = false;
  device_manager_select(NULL);
}

#endif // ADC_MOCK
