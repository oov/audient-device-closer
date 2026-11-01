#pragma once

#ifdef ADC_MOCK

#  include <ovbase.h>

#  include <wchar.h>

/**
 * @brief Put a made-up machine in place of the one this program is on
 *
 * A Debug build can be told to answer as if the machine were in a state this program can
 * only meet by chance otherwise.  What each name stands for is written next to the field it
 * is read into, see struct settings.
 *
 * @param scenario the name of the machine to answer with, may not be NULL
 * @return false when the name is not one of them, or a stand-in could not be taken; the
 *         machine of the system is in place in that case
 */
bool simulate_install(wchar_t const *scenario);

#endif // ADC_MOCK
