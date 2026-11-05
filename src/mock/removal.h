#pragma once

#ifdef ADC_MOCK

#  include "../removal_manager.h"

/**
 * @brief What a stand-in machine answers to the removal
 */
struct removal_mock_script {
  size_t refusals;                    //!< how many removals are refused before one is let through
  enum app_error_code code;           //!< the code a refusal carries
  wchar_t const *vetoed_device;       //!< the device a veto names
  unsigned long delay_ms;             //!< how long the answer takes, milliseconds
  void (*on_success)(void *userdata); //!< called when a removal is let through, may be NULL
  void *userdata;                     //!< handed back to on_success
};

/**
 * @brief Make this program release devices through a stand-in from here on
 *
 * The script is copied, so it may live on the stack of the caller.
 *
 * @param script what the stand-in answers, may not be NULL
 * @return false when the script could not be taken
 */
bool removal_mock_install(struct removal_mock_script const *script);

/**
 * @brief How often the removal was asked for since the stand-in was installed
 *
 * @return the number of removals asked for
 */
size_t removal_mock_remove_count(void);

/**
 * @brief Go back to the machine of the system
 */
void removal_mock_reset(void);

#endif // ADC_MOCK
