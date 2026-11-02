#pragma once

#include <ovbase.h>

/**
 * @brief What this run was asked to do
 */
struct options {
  bool auto_run;                  //!< the scheduler started this run, so no window waits for a person
  bool close_mixer;               //!< close iD Mixer without asking
  bool allow_audio_service_pause; //!< allow pausing the Windows Audio service without asking
};

/**
 * @brief The options a plain start of this program runs with
 *
 * @param out filled in with the defaults
 */
void options_default(struct options *const out);

/**
 * @brief Read the switches of the command line
 *
 * Unknown tokens are ignored on purpose.  A switch that is spelled out twice keeps its last
 * answer.
 *
 * @param command_line the command line of this process, NULL is allowed
 * @param out filled in with what the command line says
 * @param err receives a failure of the parsing
 * @return false only on a hard failure
 */
NODISCARD bool options_parse(NATIVE_CHAR const *const command_line, struct options *const out, struct ov_error *const err);
