#include "options.h"

#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief The options a plain start of this program runs with
 *
 * @param out filled in with the defaults
 */
void options_default(struct options *const out) {
  out->auto_run = false;
  out->close_mixer = true;
  out->allow_audio_service_pause = false;
}

/**
 * @brief One token of the command line
 *
 * The command line is what the system hands the process, and the system hands it as a wide
 * text, so it is read as one: a form of the same text in UTF-8 would only have to be
 * converted back to be read.
 */
struct scanner {
  wchar_t const *text;
  size_t pos;
};

/**
 * @brief Read the next token of the command line
 *
 * Spaces separate the tokens; a token may be wrapped in double quotes to carry spaces.
 *
 * @param sc the scan to advance
 * @param token receives the token without its quotes
 * @param token_size room of token
 * @return false at the end of the command line
 */
static bool scanner_next(struct scanner *const sc, wchar_t *const token, size_t const token_size) {
  while (sc->text[sc->pos] == L' ' || sc->text[sc->pos] == L'\t') {
    sc->pos++;
  }
  if (sc->text[sc->pos] == L'\0') {
    return false;
  }
  bool quoted = false;
  if (sc->text[sc->pos] == L'"') {
    quoted = true;
    sc->pos++;
  }
  size_t n = 0;
  for (; sc->text[sc->pos] != L'\0'; sc->pos++) {
    if (quoted) {
      if (sc->text[sc->pos] == L'"') {
        sc->pos++;
        break;
      }
    } else if (sc->text[sc->pos] == L' ' || sc->text[sc->pos] == L'\t') {
      break;
    }
    if (n + 1 < token_size) {
      token[n++] = sc->text[sc->pos];
    }
  }
  token[n] = L'\0';
  return true;
}

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
NODISCARD bool options_parse(NATIVE_CHAR const *const command_line, struct options *const out, struct ov_error *const err) {
  wchar_t token[256];
  struct scanner sc = {NULL, 0};

  if (!out) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  options_default(out);
  if ((command_line == NULL) || (command_line[0] == 0)) {
    return true;
  }

  sc.text = command_line;
  sc.pos = 0;
  while (scanner_next(&sc, token, sizeof(token) / sizeof(token[0]))) {
    if (wcscmp(token, L"-auto-run") == 0) {
      out->auto_run = true;
    } else if (wcscmp(token, L"-close-mixer") == 0) {
      out->close_mixer = true;
    } else if (wcscmp(token, L"-no-close-mixer") == 0) {
      out->close_mixer = false;
    } else if (wcscmp(token, L"-pause-audio-service") == 0) {
      out->allow_audio_service_pause = true;
    } else if (wcscmp(token, L"-no-pause-audio-service") == 0) {
      out->allow_audio_service_pause = false;
    }
  }
  return true;
}
