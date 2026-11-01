#include <windows.h>

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "instance.h"
#include "logger.h"
#include "options.h"
#include "paths.h"
#include "settings.h"
#include "simulate.h"
#include "ui.h"

/**
 * @brief Tell the person at the machine that the settings file could not be read
 *
 * The name the program looks for is the name of the executable with its extension replaced, so
 * what the dialog shows is that name as it came out, not a description of the rule that made
 * it.
 *
 * The translation resources are loaded before this is called, so the dialog follows the
 * language of the user.  A failure to load a translation is reported to the log only and
 * stays untranslated on purpose.
 *
 * @param path the file that could not be read, may be NULL
 */
static void show_startup_error(wchar_t const *const path) {
  wchar_t *text = NULL;

  wchar_t unknown[64];
  if (OV_SNPRINTF(unknown, sizeof(unknown) / sizeof(unknown[0]), NULL, L"%s", gettext("(unknown)")) < 0) {
    goto cleanup;
  }

  if (!ov_sprintf_char2wchar(&text,
                             NULL,
                             "%1$ls",
                             gettext("Could not read the settings file.\n"
                                     "It has to sit next to the program as %1$ls."),
                             path ? paths_file_name(path) : unknown)) {
    goto cleanup;
  }

  wchar_t title[256];
  if (OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), NULL, L"%s", gettext("Audient Device Closer")) < 0) {
    goto cleanup;
  }
  MessageBoxW(NULL, text, title, MB_OK | MB_ICONERROR);

cleanup:
  if (text != NULL) {
    OV_ARRAY_DESTROY(&text);
  }
}

/**
 * @brief Start the program: read the settings and open the window
 *
 * @param hInstance unused
 * @param hPrevInstance unused
 * @param pCmdLine unused, the command line is read through GetCommandLineW
 * @param nCmdShow unused
 * @return the code the process exits with
 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
  struct ov_init_options init = ov_init_get_default_options();
  struct ov_error err = {0};
  struct options options;
  struct settings settings;
  struct mo *mo = NULL;
  wchar_t *path = NULL;
  struct instance instance = {0};
  int result = 1;

  // trans: This dagger helps UTF-8 detection. You don't need to translate this.
  (void)gettext_noop("†");
  (void)hInstance;
  (void)hPrevInstance;
  (void)nCmdShow;

  memset(&settings, 0, sizeof(settings));

  init.output_func = logger_output;
  if (!ov_init(&init)) {
    return 2;
  }
  logger_init();
  {
    // a log that cannot be opened is worth a report and nothing more: without it the
    // summaries of everything below simply have nowhere to go
    struct ov_error log_err = {0};

    if (!logger_open(&log_err)) {
      OV_ERROR_REPORT(&log_err, NULL);
    }
  }

  // the copy that finds the mutex taken is a second start: it shows the window that is
  // already there and leaves, with the answer a start that did what it was asked to do
  instance = instance_take(&err);
  if (instance.handle == NULL) {
    if (ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))) {
      OV_ERROR_DESTROY(&err);
      // the copy at the screen is the one to reach, and the reason the mutex was not taken
      // is handled here, so nothing is left to report
      if (!ui_activate_running_window(&err)) {
        OV_ERROR_ADD_TRACE(&err);
        goto cleanup;
      }
      result = 0;
      goto cleanup;
    }
    // without the mutex no other copy can be ruled out, so this copy goes on with its
    // window and the log carries why the check is missing
    OV_ERROR_REPORT(&err, NULL);
  }

  {
    struct ov_error mo_err = {0};

    mo = mo_parse_from_resource(GetModuleHandleW(NULL), &mo_err);
    OV_ERROR_REPORT(&mo_err, NULL);
  }
  if (mo != NULL) {
    mo_set_default(mo);
  }

  options_default(&options);
  if (!options_parse(pCmdLine, &options, &err)) {
    OV_ERROR_ADD_TRACE(&err);
    goto cleanup;
  }
  path = settings_default_path(&err);
  if (path == NULL) {
    show_startup_error(NULL);
    goto cleanup;
  }
  if (!settings_load(path, &settings, &err)) {
    show_startup_error(path);
    goto cleanup;
  }
#ifdef ADC_MOCK
  if ((settings.simulate != NULL) && (settings.simulate[0] != L'\0')) {
    // the settings named a made-up machine, so every part of the run is pointed at it here,
    // before the window opens and any of it is read
    if (!simulate_install(settings.simulate)) {
      OV_ERROR_SETF(&err,
                    ov_error_type_generic,
                    ov_error_generic_invalid_argument,
                    "the settings name a made-up machine this build does not know: %1$ls",
                    "the settings name a made-up machine this build does not know: %1$ls",
                    settings.simulate);
      show_startup_error(path);
      goto cleanup;
    }
  }
#endif

  if (ui_run(&options, &settings, &err)) {
    result = 0;
  }

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  if (!instance_release(&instance, &err)) {
    OV_ERROR_REPORT(&err, NULL);
  }
  if (!logger_close(&err)) {
    OV_ERROR_REPORT(&err, NULL); // the log may be closed already, and then nothing is written
  }
  if (path != NULL) {
    OV_ARRAY_DESTROY(&path);
  }
  settings_release(&settings);
  if (mo != NULL) {
    mo_set_default(NULL);
    mo_free(&mo);
    mo = NULL;
  }
  ov_exit();
  return result;
}
