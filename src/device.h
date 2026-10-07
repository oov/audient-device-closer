#pragma once

#include <ovbase.h>

#include <stddef.h>

/**
 * @brief What the state of the device is, in the terms the dialog offers
 */
enum device_state {
  DEVICE_STATE_HEALTHY, //!< at least one endpoint of the service is present
  DEVICE_STATE_FAULTED, //!< the devnode is fine and the interfaces are registered, yet none is present
  DEVICE_STATE_PROBLEM, //!< the system reports a problem code, which is not this tool's failure to cure
  DEVICE_STATE_ABSENT,  //!< there is no device of that service
  DEVICE_STATE_UNKNOWN, //!< nothing was found that the states above could be decided from
};

/**
 * @brief Decide the state of the device from what the system reports
 *
 * @param problem_code a CM_PROB_* value of the devnode
 * @param render_present interfaces of the render category that are present
 * @param render_registered interfaces of the render category that are registered
 * @param capture_present interfaces of the capture category that are present
 * @param capture_registered interfaces of the capture category that are registered
 * @return the state the dialog shows
 */
enum device_state device_evaluate(
    unsigned long problem_code, size_t render_present, size_t render_registered, size_t capture_present, size_t capture_registered);

/**
 * @brief First present devnode of the KS service
 *
 * @param service DEVPKEY_Device_Service of the KS devnode
 * @param instance_prefix only devices under this prefix are looked at, NULL means no filter
 * @param err receives the failure of the query
 * @return the devnode in UTF-8, NULL when no device of the service is present
 * @note Release with device_string_free.
 */
char *device_ks_devnode(char const *const service, char const *const instance_prefix, struct ov_error *const err);

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void device_string_free(char **const value);

/**
 * @brief Look at the device and report what state it is in
 *
 * Detection only.  Never modifies the device.
 *
 * @param service DEVPKEY_Device_Service of the KS devnode
 * @param instance_prefix only devices under this prefix are looked at, NULL means no filter
 * @param err receives the failure of the checks; a state that is neither healthy nor faulted
 *            always carries one
 * @return the state the dialog shows
 */
enum device_state device_probe(char const *const service, char const *const instance_prefix, struct ov_error *const err);
