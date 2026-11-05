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
 * raises it.  app_error_none is the "no code" sentinel.
 */
enum app_error_code {
  app_error_none = 0,                  //!< no removal error: the outcome was not a refusal of the removal
  app_error_removal_veto = 1,          //!< a process still holds the device; the caller closes it and asks again
  app_error_removal_access_denied = 2, //!< this program may not ask for the removal
  app_error_removal_call_failed = 3,   //!< an earlier step of the removal already failed
  app_error_removal_device_gone = 4,   //!< the device was not there any more
  app_error_removal_refused = 5,       //!< the system refused for a reason of its own
  app_error_task_not_installed = 6,    //!< this copy is not the one the installer put on the machine
};
