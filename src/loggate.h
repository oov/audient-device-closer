#ifndef ADC_LOGGATE_H
#define ADC_LOGGATE_H

#include <ovbase.h>

/**
 * @brief Suppresses the repetition of the same error message
 *
 * The device state is polled every few seconds.  Without this, a permanent failure would
 * append the same block to the log forever.
 *
 * The state deliberately carries no ov_error: the only way it can fail is running out of
 * memory while remembering a message, and in that case repeating the message is the safe
 * answer, not an error the caller could act upon.
 */
struct loggate {
  char *last;
};

/**
 * @brief Report whether this message is worth writing again
 *
 * @param gate NULL is allowed and reports everything
 * @param text the message as it would reach the log, NULL is allowed
 * @return TRUE when this message differs from the previous one, in which case it is
 *         remembered.  FALSE means the caller should not report it again.
 */
bool loggate_should_emit(struct loggate *const gate, char const *const text);

/**
 * @brief Forget the remembered message
 *
 * @param gate NULL is allowed
 */
void loggate_release(struct loggate *const gate);

#endif /* ADC_LOGGATE_H */
