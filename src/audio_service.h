#pragma once

#include <ovbase.h>

#include <stdbool.h>

/**
 * @brief Pause the Windows Audio service, so the audio engine lets the device go
 *
 * The audio engine (audiodg.exe) is started by this service and is restarted within
 * milliseconds whenever it is terminated, so closing it on its own never releases a device
 * for longer than that.  Pausing the service that starts it leaves the device alone until
 * the service is started again, which is the only way a device that is working normally can
 * be detached.
 *
 * The service is the one Windows names "Windows Audio"; it is stopped while the call waits,
 * so the caller knows when the device is free.
 *
 * @param err receives the refusal of the system
 * @return false when the service could not be paused
 */
bool audio_service_pause(struct ov_error *const err);

/**
 * @brief Start the Windows Audio service again
 *
 * It has to be called after every pause, whatever came in between, or the machine is left
 * without sound.
 *
 * @param err receives the refusal of the system
 * @return false when the service could not be started again
 */
bool audio_service_resume(struct ov_error *const err);
