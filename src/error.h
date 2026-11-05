#pragma once

/**
 * @brief The one error type of this program's own failures
 *
 * ovbase keeps the types 0-3 for itself.  Every answer of this program uses this one type,
 * so a caller can tell it apart from a system error without reading the message.
 */
enum {
  ov_error_type_app = 0x10,
};

/**
 * @brief The codes of ov_error_type_app
 *
 * A value belongs to one part of the program only; the name of each code says which part
 * raises it.  removal_error_none is the "no code" sentinel.
 */
enum app_error_code {
  removal_error_none = 0,          //!< no removal error: the outcome was not a refusal of the removal
  removal_error_veto = 1,          //!< a process still holds the device; the caller closes it and asks again
  removal_error_access_denied = 2, //!< this program may not ask for the removal
  removal_error_call_failed = 3,   //!< an earlier step of the removal already failed
  removal_error_device_gone = 4,   //!< the device was not there any more
  removal_error_refused = 5,       //!< the system refused for a reason of its own
  task_error_not_installed = 6,    //!< this copy is not the one the installer put on the machine
};
