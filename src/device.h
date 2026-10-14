#pragma once

#include <ovbase.h>

#include <stddef.h>

/**
 * @brief What the state of the device is, in the terms the dialog offers
 */
enum device_state {
  DEVICE_STATE_HEALTHY,      //!< at least one endpoint of the service is present
  DEVICE_STATE_FAULTED,      //!< the devnode is fine and the interfaces are registered, yet none is present
  DEVICE_STATE_PROBLEM,      //!< the system reports a problem code, which is not this tool's failure to cure
  DEVICE_STATE_ABSENT,       //!< there is no device of that service
  DEVICE_STATE_NO_INTERFACE, //!< the devnode is fine, yet none of the endpoint categories is registered for it
  DEVICE_STATE_QUERY_FAILED, //!< the queries themselves failed, err of the call says why
};

/**
 * @brief One supported device: the identifier that matches it and the name that tells it apart
 *
 * The identifier is a device instance ID as the driver package lists it, for example
 * "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS".  A devnode sits under that identifier, so a devnode
 * belongs to the device when it starts with the identifier.
 */
struct device_entry {
  char *id;   //!< device instance identifier, a prefix of the devnode, never NULL
  char *name; //!< the name of the product the settings show, never NULL
};

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void device_string_free(char **const value);

/**
 * @brief The entry of the list a devnode belongs to
 *
 * The identifier of an entry is a prefix of the devnode of its device,
 * so the devnode says which entry names it.  A devnode no entry names
 * is one the list does not cover.
 *
 * @param devnode instance ID of the KS devnode, UTF-8
 * @param devices the devices this program looks at, NULL or empty means
 *                no entry covers any devnode
 * @param device_count how many devices the list holds
 * @param err receives the refused arguments, or a not_found for a devnode no entry names
 * @return the index of the entry, SIZE_MAX when no entry names the devnode
 */
size_t device_find_entry(char const *const devnode,
                         struct device_entry const *const devices,
                         size_t const device_count,
                         struct ov_error *const err);

/**
 * @brief Does a devnode name the device an identifier names
 *
 * An identifier of a device, the one without the unit, names
 * every unit of it: the devnode is the same device when it
 * starts with the identifier.  An identifier of one unit
 * names that unit alone: the devnode is the same device
 * when it is the identifier.  The letters of the ASCII are
 * the only ones with a case, so the two spellings of a
 * letter name the same device.
 *
 * @param devnode instance ID of the devnode, UTF-8
 * @param id the identifier of a device or of a unit, UTF-8
 * @return true when devnode and id name the same device
 */
bool device_is_same(char const *const devnode, char const *const id);

/**
 * @brief One device of the service this program looks at, and what state it is in
 */
struct device_target {
  char *devnode;           //!< instance ID of the KS devnode in UTF-8, never NULL
  enum device_state state; //!< what the tests of its endpoints answered for it
};

/**
 * @brief Enumerate the present devnodes of the service and state each one
 *
 * Every devnode that belongs to one of the devices is measured and enters the list with its
 * state, so all the connected devices are covered.  Detection only, never modifies a
 * device.
 *
 * @param service DEVPKEY_Device_Service of the KS devnodes
 * @param devices the devices this program looks at, NULL or empty says
 *                every devnode of the service is a match
 * @param device_count how many devices the list holds
 * @param err receives the failure of the queries, or a not_found for a service no device
 *            of it is present
 * @return the list of the targets, NULL when no device is present or the queries failed;
 *         release with device_targets_release
 */
struct device_target *device_probe_all(char const *const service,
                                       struct device_entry const *const devices,
                                       size_t const device_count,
                                       struct ov_error *const err);

/**
 * @brief Release the list of the targets
 *
 * @param targets the list to release, set to NULL afterwards, NULL is allowed
 */
void device_targets_release(struct device_target **const targets);

/**
 * @brief Keep the faulted units of the list and let the others go
 *
 * The repair is for the devices the sleep took the endpoints of, so a device that is
 * healthy, absent or otherwise broken is not one of them.
 *
 * @param targets the list to shrink in place, set to NULL afterwards, NULL is allowed
 */
void device_targets_keep_faulted(struct device_target **const targets);

/**
 * @brief State one devnode the caller already knows by name
 *
 * Detection only.  Used to follow a device after its release, the way the confirm stage
 * does.  The state the devnode is in and the failure of the queries are told apart by the
 * value: a devnode no endpoint is registered for is DEVICE_STATE_NO_INTERFACE, while a
 * query that failed is DEVICE_STATE_QUERY_FAILED.
 *
 * @param devnode instance ID of the KS devnode, UTF-8
 * @param err receives the failure of the queries
 * @return the state the devnode is in, DEVICE_STATE_NO_INTERFACE when no endpoint is
 *         registered for it, DEVICE_STATE_QUERY_FAILED when the queries failed
 */
enum device_state device_probe_devnode(char const *const devnode, struct ov_error *const err);

/**
 * @brief Does the removal record name one of the ancestors of a devnode
 *
 * The removal is refused for the USB parent of a KS devnode, so a record belongs to the
 * devnode when its DeviceInstance is that ancestor's instance ID.  The ancestors are
 * compared by their exact instance IDs, case as Windows spells them.
 *
 * @param device_instance the DeviceInstance of the removal record, UTF-8, may not be NULL
 * @param devnode instance ID of the KS devnode, UTF-8, may not be NULL
 * @param err receives the failure of the queries
 * @return ov_true when the record names an ancestor, ov_false when it does not,
 *         ov_indeterminate when the ancestors could not be walked, with err set
 */
ov_tribool device_culprit_names_ancestor(char const *const device_instance, char const *const devnode, struct ov_error *const err);
