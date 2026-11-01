#pragma once

#include <ovbase.h>

#include <stdint.h>
#include <wchar.h>

#include "async.h"

/** @brief One watch of a devnode, running on a thread of its own */
struct device_watch;

/**
 * @brief What the machine reports about one devnode
 *
 * problem_code is the machine's own number: 0 means it reports no problem, and any other
 * value is a problem it named.  DEVICE_PROBLEM_NO_REPORT is the only value this layer gives
 * a meaning to, and it answers a devnode that could not be asked.
 */
struct device_facts {
  uint32_t problem_code;    //!< 0, a problem the machine named, or DEVICE_PROBLEM_NO_REPORT
  bool endpoint_present;    //!< at least one endpoint of either category is present
  bool endpoint_registered; //!< at least one endpoint of either category is registered
};

/** @brief The code a devnode that could not be asked is answered with */
static const uint32_t DEVICE_PROBLEM_NO_REPORT = UINT32_MAX;

/**
 * @brief The questions this program puts to the machine about its devices
 *
 * The questions are the ones the program works in, not the ones the configuration manager
 * happens to offer: which devnodes a service has, what one of them reports, and which
 * device sits above it.  Each implementation answers them for one machine.
 *
 * Every call that answers reports the failure through err.  A question that was answered is
 * never turned into an error, so a state the caller did not expect stays an answer.
 */
struct device_manager {
  /**
   * @brief Ask for the devnodes of one service, one at a time
   *
   * @param service DEVPKEY_Device_Service of the KS devnodes
   * @param on_devnode the callback that looks at one devnode, may not be NULL
   * @param ctx handed back to on_devnode
   * @param err receives the failure of the question or the refusal of the callback
   * @return false when the walk could not be completed
   */
  bool (*foreach_devnode)(wchar_t const *service,
                          bool (*on_devnode)(wchar_t const *devnode, void *ctx, struct ov_error *err),
                          void *ctx,
                          struct ov_error *err);

  /**
   * @brief Ask what one devnode reports
   *
   * @param devnode instance ID of the KS devnode, may not be NULL
   * @param out filled in when the question was answered
   * @param err receives the failure of the question
   * @return false when the devnode could not be asked
   */
  bool (*facts_of)(wchar_t const *devnode, struct device_facts *out, struct ov_error *err);

  /**
   * @brief Ask which device sits one level above a devnode
   *
   * The buffer belongs to the caller and is reused: the call writes into it and grows it
   * when it is too small, so a walk that asks again and again keeps one buffer instead of
   * taking a new one every time.
   *
   * @param devnode instance ID of the KS devnode, may not be NULL
   * @param buf the buffer to write the instance above into, NULL for a new one, reused when
   *            it is not NULL, release with OV_ARRAY_DESTROY
   * @param err receives the failure of the question
   * @return ov_true when the devnode has one and buf holds it, ov_false when it has none,
   *         ov_indeterminate when the question failed, with err set
   */
  ov_tribool (*parent_of)(wchar_t const *devnode, wchar_t **buf, struct ov_error *err);

  /**
   * @brief The sentence this program has for one problem the machine named
   *
   * The answer is the identifier the translations are keyed by, not the sentence of the
   * reader's language: the call that knows the language turns it with gettext().  NULL
   * means there is no sentence to show, for a code this machine has none of its own for.
   *
   * @param problem_code the machine's own number, 0 or DEVICE_PROBLEM_NO_REPORT allowed
   * @return the identifier to translate, NULL when there is no sentence to show
   */
  char const *(*problem_id)(uint32_t problem_code);

  /**
   * @brief Watch a devnode until the caller says it is done
   *
   * The devnode is looked at now and then, and what the machine reports is handed to
   * on_update_device: an answer of true ends the watch, and an answer of false is followed by
   * another look.  What the state of the device means is not the machine's to decide, so the
   * caller says which of the answers it waits for.  Between two looks the watch waits for a
   * device interface to show up rather than asking again and again, and it roughly does not
   * care which interface it was: any of them is a moment worth looking again, and a look
   * that finds the devnode away is answered by waiting once more.
   *
   * A look that cannot be taken is one the wait goes on without: right after a removal the
   * devnode is often not there yet, and that is what the wait is for.  The reason of the
   * last look is the one that says how the watch ended, so it is kept and handed back.
   *
   * on_update_device is called on the thread of the watch, so it may not call into the
   * caller's windows and may not wait.
   *
   * @param devnode instance ID of the KS devnode, may not be NULL
   * @param max_wait_ms how long to watch at most, milliseconds
   * @param on_update_device called on the thread of the watch with what the machine reports,
   *        may not be NULL; true ends the watch
   * @param on_done called from the thread of the watch when it is over, may be NULL
   * @param userdata handed to on_done
   * @param err receives the failure to start the watch
   * @return the watch, read with watch_take; NULL when it could not be started
   */
  struct device_watch *(*watch)(wchar_t const *devnode,
                                unsigned long max_wait_ms,
                                bool (*on_update_device)(struct device_facts const *facts),
                                async_notify_fn notify,
                                void *userdata,
                                struct ov_error *err);

  /**
   * @brief Hand back what the watch last saw and release it
   *
   * Called on the caller's own thread once the end of the watch was called.  The failure is
   * moved into err, so err has to be empty.
   *
   * @param w the watch, set to NULL afterwards
   * @param err receives the failure of the last look
   * @return what the last look reported, DEVICE_PROBLEM_NO_REPORT when it could not be taken
   */
  struct device_facts (*watch_take)(struct device_watch **w, struct ov_error *err);

  /**
   * @brief Ask the watch to stop, from the caller's thread
   *
   * It has to be the thread that started the watch: the caller's own state is what the ask
   * reaches, and breaking this is a fault of the caller.
   *
   * @param w the watch
   */
  void (*watch_cancel)(struct device_watch *w);
};

/**
 * @brief The machine this program is working against
 *
 * The machine of the system until another one is named, so a run that never asks for a
 * stand-in reads the machine it is on.
 *
 * @return the manager in use, never NULL
 */
struct device_manager const *device_manager(void);

#ifdef ADC_MOCK
/**
 * @brief Work against another machine from here on
 *
 * Every caller of device_probe_all and its neighbours reads the devices through this one
 * place, so a stand-in named here is what the whole program sees from now on.
 *
 * @param manager the machine to work against, NULL to go back to the machine of the system
 */
void device_manager_select(struct device_manager const *manager);
#endif // ADC_MOCK

/**
 * @brief The machine of the system itself
 *
 * @return the manager that asks the configuration manager, never NULL
 */
struct device_manager const *device_manager_system(void);
