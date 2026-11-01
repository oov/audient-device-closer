#pragma once

#ifdef ADC_MOCK

#  include "device_manager.h"

/**
 * @brief One devnode of a stand-in machine
 */
struct device_mock_node {
  wchar_t const *devnode;       //!< instance ID of the KS devnode, never NULL
  wchar_t const *parent;        //!< instance ID of the device above it, NULL when it is at the root
  struct device_facts facts;    //!< what this node reports
  struct device_facts restored; //!< what it reports once a wait looked and the answer was no
  bool look_fails;              //!< whether a look of a wait on it cannot be taken
};

/**
 * @brief The devnodes one service of a stand-in machine has
 *
 * The stand-in answers exactly what the script says, so a run can be taken through the
 * fault this tool cures without a device that has it.
 */
struct device_mock_script {
  wchar_t const *service;               //!< the service the nodes belong to, never NULL
  struct device_mock_node const *nodes; //!< the nodes, may be NULL when there are none
  size_t node_count;                    //!< how many nodes that is
  bool list_fails;                      //!< whether the devnodes cannot be listed at all
};

/**
 * @brief Make this program work against a stand-in machine from here on
 *
 * The facts are copied, so the script may live on the stack of the caller.  device_mock_reset
 * puts the machine of the system back.
 *
 * @param script the script to answer with, may not be NULL
 * @return false when the script could not be taken
 */
bool device_mock_install(struct device_mock_script const *script);

/**
 * @brief Tell the stand-in machine that one devnode reports something else now
 *
 * This is how a run that repairs a device sees it come back: the removal of the stand-in
 * turns the node healthy here.
 *
 * @param devnode the node to change
 * @param facts what it reports from now on
 * @return false when no node of that name is in the script
 */
bool device_mock_set_facts(wchar_t const *devnode, struct device_facts const *facts);

/**
 * @brief Go back to the machine of the system
 */
void device_mock_reset(void);

#endif // ADC_MOCK
