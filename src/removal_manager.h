#pragma once

#include <ovbase.h>

#include <wchar.h>

#include "async.h"
#include "error.h"

/**
 * @brief One removal, running on a thread of its own
 *
 * Asking the configuration manager to remove a subtree takes seconds on a machine whose
 * device is in use, and the call answers when it is over.  A caller that has a window
 * hands the ask here and the thread of the removal reports back; every call on this side
 * is answered at once.
 */
struct removal_op;

/**
 * @brief The release of a device from the machine
 *
 * One question, because releasing is one act: whoever answers it either released the device
 * or says why not, and the why is the code of ov_error_type_app.
 */
struct removal_manager {
  /**
   * @brief Start releasing one device
   *
   * Does not detect, does not look up an event and does not close any process: the device
   * named here is the one released.
   *
   * @param devnode instance ID of the KS devnode, may not be NULL or empty
   * @param notify called with stopped true on the thread that asks the removal to stop, and
   *        with stopped false on the thread of the removal once it is over; may be NULL
   * @param userdata handed to notify
   * @param err receives the failure to start the removal and, on the thread of the removal,
   *        the refusal of the machine; it has to outlive the removal: what the removal raises
   *        is written into it while op_release waits for it to be over.  NULL is allowed and
   *        drops both
   * @return the removal, release it with op_release; NULL when it could not be started
   */
  struct removal_op *(*remove_start)(wchar_t const *devnode, async_notify_fn notify, void *userdata, struct ov_error *err);

  /**
   * @brief Ask the removal to stop
   *
   * Only the thread that started the removal may call this.
   *
   * @param op the removal
   */
  void (*op_cancel)(struct removal_op *op);

  /**
   * @brief Wait for the removal to be over and take its answer
   *
   * Only the thread that started the removal may call this, and only once the removal told
   * it that it is over.  What the machine answered is already in the err the removal was
   * started with, so this asks for none.
   *
   * @param op the removal, set to NULL afterwards
   * @return true when the device was released
   */
  bool (*op_release)(struct removal_op **op);
};

/**
 * @brief The machine this program is working against
 *
 * @return the manager in use, never NULL
 */
struct removal_manager const *removal_manager(void);

#ifdef ADC_MOCK
/**
 * @brief Work against another machine from here on
 *
 * @param manager the machine to work against, NULL to go back to the machine of the system
 */
void removal_manager_select(struct removal_manager const *manager);
#endif // ADC_MOCK

/**
 * @brief The machine of the system itself
 *
 * @return the manager that asks the configuration manager, never NULL
 */
struct removal_manager const *removal_manager_system(void);

/**
 * @brief Was the removal refused because a process still holds the device
 *
 * A veto is not a give-up: the caller closes what holds the device and asks again.  The
 * code is read through this one place, so the meaning of a veto lives next to the call
 * that raises it.
 *
 * @param err the error of a removal that failed
 * @return true when the code is removal_error_veto
 */
bool removal_error_is_veto(struct ov_error const *const err);

/**
 * @brief The code of a removal error, for a caller that carries it further
 *
 * @param err the error of a removal that failed
 * @return the code of ov_error_type_app, removal_error_none when err carries no removal error
 */
enum app_error_code removal_error_of(struct ov_error const *const err);
