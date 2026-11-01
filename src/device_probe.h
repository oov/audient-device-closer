#pragma once

#include <ovbase.h>

#include <stdint.h>
#include <wchar.h>

struct device_facts;

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
 *
 * A devnode and the identifier that matches it are what the system is asked for and what the
 * settings file spells, so the identifier is kept as the settings spell it, and the name is
 * only ever shown to a person.
 */
struct device_entry {
  wchar_t *id;   //!< device instance identifier, a prefix of the devnode, never NULL
  wchar_t *name; //!< the name of the product the settings show, never NULL
};

/**
 * @brief The entry of the list a devnode belongs to
 *
 * The identifier of an entry is a prefix of the devnode of its device,
 * so the devnode says which entry names it.  A devnode no entry names
 * is one the list does not cover.
 *
 * @param devnode instance ID of the KS devnode
 * @param devices the devices this program looks at, NULL or empty means
 *                no entry covers any devnode
 * @param device_count how many devices the list holds
 * @param err receives the refused arguments, or a not_found for a devnode no entry names
 * @return the index of the entry, SIZE_MAX when no entry names the devnode
 */
NODISCARD size_t device_find_entry(wchar_t const *const devnode,
                                   struct device_entry const *const devices,
                                   size_t const device_count,
                                   struct ov_error *const err);

/**
 * @brief One device of the service this program looks at, and what state it is in
 */
struct device_target {
  wchar_t *devnode;        //!< instance ID of the KS devnode, never NULL
  enum device_state state; //!< what the tests of its endpoints answered for it
  uint32_t problem_code;   //!< 0 when the machine named none, DEVICE_PROBLEM_NO_REPORT when it could not be asked
};

/**
 * @brief Enumerate the present devnodes of the service and state each one
 *
 * Every devnode that belongs to one of the devices is measured and enters the list with its
 * state, so all the connected devices are covered.  Detection only, never modifies a
 * device.
 *
 * @param service DEVPKEY_Device_Service of the KS devnodes
 * @param devices the devices this program looks at, may not be NULL or empty
 * @param device_count how many devices the list holds
 * @param err receives the failure of the queries, or a not_found for a service no device
 *            of it is present
 * @return the list of the targets, NULL when no device is present or the queries failed;
 *         release with device_targets_release
 */
NODISCARD struct device_target *device_probe_all(wchar_t const *const service,
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
 * @param devnode instance ID of the KS devnode
 * @param err receives the failure of the queries
 * @return the state the devnode is in, DEVICE_STATE_NO_INTERFACE when no endpoint is
 *         registered for it, DEVICE_STATE_QUERY_FAILED when the queries failed
 */
NODISCARD enum device_state device_probe_devnode(wchar_t const *const devnode, struct ov_error *const err);

/**
 * @brief The state the facts stand for
 *
 * @param facts what the machine reported about the devnode
 * @return the state, DEVICE_STATE_QUERY_FAILED when the devnode could not be asked
 */
NODISCARD enum device_state device_facts_state(struct device_facts const *facts);

/**
 * @brief Is the device in the state this tool wants it in
 *
 * The answer is what a run waits for once the device is released, so it is the one place
 * that says what healthy is: no problem code, and an endpoint of the service present.
 *
 * @param facts what the machine reported about the devnode
 * @return true when the device is healthy
 */
bool device_facts_is_healthy(struct device_facts const *const facts);

/**
 * @brief Does the removal record name one of the ancestors of a devnode
 *
 * The removal is refused for the USB parent of a KS devnode, so a record belongs to the
 * devnode when its DeviceInstance is that ancestor's instance ID.  The ancestors are
 * compared by their exact instance IDs, case as Windows spells them.
 *
 * @param device_instance the DeviceInstance of the removal record, may not be NULL
 * @param devnode instance ID of the KS devnode, may not be NULL
 * @param err receives the failure of the queries
 * @return ov_true when the record names an ancestor, ov_false when it does not,
 *         ov_indeterminate when the ancestors could not be walked, with err set
 */
NODISCARD ov_tribool device_veto_event_names_ancestor(wchar_t const *const device_instance,
                                                      wchar_t const *const devnode,
                                                      struct ov_error *const err);
