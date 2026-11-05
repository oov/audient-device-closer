#include "ui.h"

#include "task.h"
#include "theme.h"
#include "ui_blockers_window.h"
#include "ui_device_picker.h"
#include "ui_progress_window.h"

#include <windows.h>

#include <commctrl.h>

#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovutf.h>

#include "device_manager.h"
#include "device_probe.h"
#include "error.h"
#include "layout.h"
#include "logger.h"
#include "options.h"
#include "repair.h"
#include "settings.h"

#define IDC_CLOSE_BTN 2
#define IDC_CHK_MIXER 5
#define IDC_CHK_AUDIO_SERVICE 10
#define IDC_GROUP_SETTINGS 11
#define IDC_GROUP_INSTALL 7
#define IDC_TASK_INSTALL 8
#define IDC_TASK_REMOVE 9

#define WM_ADC_STAGE (WM_APP + 1)
#define WM_ADC_DONE (WM_APP + 2)
#define WM_ADC_ASYNC (WM_APP + 3)

/** @brief The timer that starts a run of the scheduler after its delay */
enum { IDT_AUTORUN = 1 };

/**
 * @brief The geometry of the main window, in logical units
 *
 * The settings group and the group of the task buttons share one band along the bottom
 * edge; the button that closes the device sits above it.  A logical unit is 1/96 of an
 * inch, see layout.h.
 */
enum main_layout_size {
  ADC_MAIN_SECTION_GAP = 20, /* the button to the group band: wider than GAP */
  ADC_MAIN_GROUP_GAP = 12,   /* between the two groups of the band */

  ADC_MAIN_BUTTON_H = 30,
  ADC_MAIN_BUTTON_W = 280,
  ADC_MAIN_CHECK_H = 22,
  /* The room a check box caption needs beyond its text: the box of the control and the gap
     to the caption.  The box belongs to comctl32 and is as tall as the text; these are the
     values it uses at 96dpi. */
  ADC_MAIN_CHECK_BOX_W = 16,
  ADC_MAIN_CHECK_BOX_GAP = 4,

  /* The two groups of the band share one top line.  The height is the room the taller of
     them needs, so their frames line up along the top and the bottom edge. */
  ADC_MAIN_GROUP_INSET = 12, /* control to the group frame */
  ADC_MAIN_GROUP_H = 88,     /* 2 * INSET + 2 * TASK_H + TASK_GAP */
  ADC_MAIN_TASK_H = 26,
  ADC_MAIN_TASK_GAP = 12, /* between the two task buttons */

  /* The width the settings group never goes below.  Its real width is measured from its own
     captions at run time, see settings_group_width: a translation can be longer than the
     English text and a language can bring a font of its own, so a number here could only be
     right by accident. */
  ADC_MAIN_SETTINGS_MIN_W = 300,
  ADC_MAIN_CAPTION_PAD = 8, /* slack on top of the measured text width */

  ADC_MAIN_MIN_CLIENT_EXTRA_W = 280, /* beside the settings group: 2 * MARGIN_X + GROUP_GAP + 2 * INSET + the least task width */

  ADC_MAIN_WINDOW_W = ADC_MAIN_SETTINGS_MIN_W + ADC_MAIN_MIN_CLIENT_EXTRA_W, /* the two groups side by side */
  ADC_MAIN_WINDOW_H = ADC_LAYOUT_MARGIN_TOP + ADC_MAIN_BUTTON_H + ADC_MAIN_SECTION_GAP + ADC_LAYOUT_CAPTION_H / 2 + ADC_MAIN_GROUP_H +
      ADC_LAYOUT_MARGIN_BOTTOM, /* the button over the band */
};

/**
 * @brief Every rectangle of the main window for one client size and dpi
 *
 * All values are physical pixels in the client coordinates of the window.  The whole
 * geometry of the window is computed here, the window procedure only hands these
 * rectangles to MoveWindow(), which is what makes the layout testable without a screen.
 */
struct main_layout {
  RECT button;              /* the button that closes the device */
  RECT check_mixer;         /* close the mixer automatically */
  RECT check_audio_service; /* allow pausing the audio service */
  RECT settings_frame;      /* the line around the check boxes */
  RECT settings_caption;    /* its caption, riding on the frame line */
  RECT install_frame;       /* the line around the task buttons */
  RECT install_caption;     /* its caption, riding on the frame line */
  RECT task_install;        /* register the scheduled task */
  RECT task_remove;         /* remove the scheduled task */
  int min_client_width;     /* the narrowest client area that shows both groups */
};

struct repair_job {
  HWND hwnd;
  struct settings const *settings;
  struct device_target *targets; // the devices to repair, NULL says the run looks for them
  bool allow_mixer_close;
  bool allow_audio_service_pause;
  bool wait_first;        // the run of the scheduler: it waits for the devices before it looks
  HANDLE cancel;          // the ask to stop, signaled by the UI thread and read by the run
  struct repair_run *run; // the run, NULL until it is taken up
  bool ok;
  bool leave;   // nothing to fix: the run leaves the window behind
  size_t total; // how many steps the run says the longest way takes, 0 until it says
  struct repair_result result;
  struct ov_error err;
};

/**
 * @brief The style of the window of the application
 *
 * Without a thick frame: the window keeps the size of its layout.
 */
static DWORD const main_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

/**
 * @brief What the window does by itself after the first look at the device
 *
 * The task starts this program at every resume from sleep and nobody stands in front of
 * the window then: the state this tool is for is acted on at once, and every other state
 * is a run with nothing to do, so the window goes away by itself instead of waiting for a
 * person who is not there.  A run a person started waits for that person, whatever the
 * state is.
 */
enum ui_auto_run {
  UI_AUTO_RUN_WAIT,   //!< nothing happens by itself
  UI_AUTO_RUN_REPAIR, //!< the faulty state: close the device at once
  UI_AUTO_RUN_LEAVE,  //!< nothing to fix here: the window closes itself
};

/**
 * @brief What a press of the button does with the devices it finds
 *
 * The answer comes from the state the devices are in at the moment of the press, not from
 * the state an earlier one saw: a run that repaired a device leaves it working, and the
 * next press has to find that out for itself.
 */
enum ui_press {
  UI_PRESS_NOT_REPAIRABLE, //!< no device is connected, or none this tool can handle
  UI_PRESS_REPAIR,         //!< at least one device is faulted: repair those right away
  UI_PRESS_ASK,            //!< every device works: ask which one to release
};

/**
 * @brief What a run nobody is watching does with the state it finds
 *
 * @param auto_run the scheduler started this run
 * @param state the state of the device
 * @return what the run should do next
 */
static enum ui_auto_run ui_auto_run_action(bool const auto_run, enum device_state const state) {
  if (!auto_run) {
    return UI_AUTO_RUN_WAIT;
  }
  return (state == DEVICE_STATE_FAULTED) ? UI_AUTO_RUN_REPAIR : UI_AUTO_RUN_LEAVE;
}

/**
 * @brief What a run nobody is watching says about the state it found
 *
 * The faulty state is the one such a run answers by working, so it has no sentence here and
 * the work says what came of it.  The state the device is in is what the reader of the log
 * has to learn, so each of the others gets a sentence of its own and nothing else.
 *
 * @param state the state of the device
 * @return the sentence, UTF-8, NULL when the run has work to do
 */
static char const *ui_auto_run_sentence(enum device_state const state) {
  switch (state) {
  case DEVICE_STATE_HEALTHY:
    return "The device is working.";
  case DEVICE_STATE_ABSENT:
    return "No Audient device was found.";
  case DEVICE_STATE_PROBLEM:
    return "The device has a problem that this tool cannot solve.";
  case DEVICE_STATE_NO_INTERFACE:
  case DEVICE_STATE_QUERY_FAILED:
    return "The device state could not be determined.";
  case DEVICE_STATE_FAULTED:
    break;
  }
  return NULL;
}

struct ui_state {
  struct settings const *settings;
  struct options const *options;
  HFONT font;
  HWND close_btn;
  HWND chk_mixer;
  HWND chk_audio_service;
  HWND task_install;
  HWND task_remove;
  HWND group_settings;
  HWND group_install;
  RECT settings_frame_rc;
  RECT settings_caption_rc;
  RECT install_frame_rc;
  RECT install_caption_rc;
  int dpi;
  bool busy;
  bool close_when_done;
  ATOM atom; //!< the class of the window, 0 while it is not registered
  HWND window;
  HWND progress;
  struct repair_job job;
};

/** @brief The class the window of this program registers under */
static wchar_t const main_window_class[] = L"audient_device_closer_main";

/**
 * @brief One logical unit as the window draws it
 *
 * Every number that reaches a control is a logical unit (1/96 inch) and goes through here
 * once, nothing in this file places a control in physical pixels.
 *
 * @param st the window that carries the dpi
 * @param logical the value in logical units
 * @return the value in physical pixels
 */
static int px(struct ui_state const *const st, int const logical) { return layout_scale(logical, (UINT)st->dpi); }

/**
 * @brief Make one control of the window
 *
 * @param st the window the control belongs to
 * @param parent the window to make it a child of
 * @param cls the window class of the control
 * @param text the caption of the control
 * @param style the style of the control
 * @param x the left edge in logical units
 * @param y the top edge in logical units
 * @param w the width in logical units
 * @param h the height in logical units
 * @param id the control id to give it
 * @param err receives the failure of the control or of its attach
 * @return the control, NULL when it could not be made
 */
static HWND child(struct ui_state const *const st,
                  HWND const parent,
                  wchar_t const *const cls,
                  wchar_t const *const text,
                  DWORD const style,
                  int const x,
                  int const y,
                  int const w,
                  int const h,
                  int const id,
                  struct ov_error *const err) {
  HWND const hwnd = CreateWindowExW(
      0, cls, text, WS_CHILD | WS_VISIBLE | style, px(st, x), px(st, y), px(st, w), px(st, h), parent, (HMENU)(INT_PTR)id, NULL, NULL);

  if (hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return NULL;
  }
  if (!theme_attach(hwnd, err)) {
    OV_ERROR_ADD_TRACE(err);
    return NULL;
  }
  return hwnd;
}

static wchar_t const ph[] = L"%1$s";

/** @brief The name the window keeps its state under */
static wchar_t const state_prop[] = L"audient_device_closer_main_state";

/**
 * @brief Switch the controls a press can act on
 *
 * @param st the window that carries the controls
 * @param enabled FALSE while the run owns the screen
 */
static void enable_actions(struct ui_state *const st, bool const enabled) {
  BOOL const on = enabled ? TRUE : FALSE;
  HWND const controls[] = {st->close_btn, st->chk_mixer, st->chk_audio_service, st->task_install, st->task_remove};
  size_t i = 0;

  for (i = 0; i < (sizeof(controls) / sizeof(controls[0])); i++) {
    // EnableWindow answers the state the control was in, not whether the call worked
    if (controls[i] != NULL) {
      EnableWindow(controls[i], on);
    }
  }
}

/**
 * @brief Show one answer of the program in a task dialog
 *
 * The outcome of a repair goes into a task dialog, which is what the UI description asks
 * for when the tool stays open.  The two buttons of the install group report through the
 * same dialog, so a reader sees one kind of answer for one window.
 *
 * @param owner the window that is asking
 * @param title the headline of the dialog
 * @param text the body of the dialog
 * @param error TRUE shows the answer as a warning
 */
static void report_dialog(HWND const owner, wchar_t const *const title, wchar_t const *const text, BOOL const error) {
  TASKDIALOGCONFIG cfg;
  wchar_t app[64];

  memset(&cfg, 0, sizeof(cfg));
  OV_SNPRINTF(app, sizeof(app) / sizeof(app[0]), ph, ph, gettext("Audient Device Closer"));
  cfg.cbSize = sizeof(cfg);
  cfg.hwndParent = owner;
  cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  cfg.pszWindowTitle = app;
  cfg.pszMainInstruction = title;
  cfg.pszContent = text;
  cfg.dwCommonButtons = TDCBF_OK_BUTTON;
  cfg.pszMainIcon = error ? TD_WARNING_ICON : TD_INFORMATION_ICON;
  if (TaskDialogIndirect(&cfg, NULL, NULL, NULL) != S_OK) {
    MessageBoxW(owner, text, title, MB_OK | (error ? MB_ICONWARNING : MB_ICONINFORMATION));
  }
}

/**
 * @brief Register the scheduled task when the button says so
 *
 * @param st the window that holds the settings
 * @param hwnd the window that is asking
 */
static void install_task(struct ui_state *const st, HWND const hwnd) {
  wchar_t title[128];
  wchar_t text[512];
  struct ov_error err = {0};
  bool const close_mixer = (SendMessageW(st->chk_mixer, BM_GETCHECK, 0, 0) == BST_CHECKED);
  bool const allow_audio_service_pause = (SendMessageW(st->chk_audio_service, BM_GETCHECK, 0, 0) == BST_CHECKED);

  bool const ok = task_install(st->settings, close_mixer, allow_audio_service_pause, &err);
  if (!ok && ov_error_is(&err, ov_error_type_app, task_error_not_installed)) {
    // the task would run a file the person who unpacked the program can replace, as the
    // system, so a portable copy never writes one
    OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), ph, ph, gettext("Failed."));
    OV_SNPRINTF(text,
                sizeof(text) / sizeof(text[0]),
                ph,
                ph,
                gettext("For security reasons, the scheduled task can only be registered by the installer version."));
    OV_ERROR_REPORT(&err, NULL);
    report_dialog(hwnd, title, text, TRUE);
    return;
  }

  OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), ph, ph, ok ? gettext("Done.") : gettext("Failed."));
  OV_SNPRINTF(text,
              sizeof(text) / sizeof(text[0]),
              ph,
              ph,
              ok ? gettext("The scheduled task was registered.") : gettext("The scheduled task could not be registered."));
  if (!ok) {
    OV_ERROR_REPORT(&err, NULL); // the dialog says that it failed, the log says why
  }
  report_dialog(hwnd, title, text, ok ? FALSE : TRUE);
}

/**
 * @brief Remove the scheduled task when the button says so
 *
 * @param st the window that holds the settings
 * @param hwnd the window that is asking
 */
static void remove_task(struct ui_state *const st, HWND const hwnd) {
  wchar_t title[128];
  wchar_t text[512];
  struct ov_error err = {0};
  bool const ok = task_uninstall(st->settings, &err);
  bool failed = false;

  if (ok) {
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("The scheduled task was removed."));
  } else if (ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) {
    // the reader asked to have nothing registered, which is the state they already have
    OV_ERROR_DESTROY(&err);
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("The scheduled task is not registered."));
  } else if (ov_error_is(&err, ov_error_type_hresult, HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY))) {
    // the entry went and the folder stayed, which is the removal the reader asked for
    OV_ERROR_DESTROY(&err);
    logger_notef("task", "the entry was removed and the folder that held it stayed");
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("The scheduled task was removed."));
  } else {
    OV_ERROR_REPORT(&err, NULL); // the dialog says that it failed, the log says why
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("The scheduled task could not be removed."));
    failed = true;
  }
  OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), ph, ph, failed ? gettext("Failed.") : gettext("Done."));
  report_dialog(hwnd, title, text, failed ? TRUE : FALSE);
}

/**
 * @brief The room a text takes with the font that is selected on the device
 *
 * @param dc the device context to measure on
 * @param text the text, UTF-8
 * @return the width in pixels, 0 when the text could not be measured
 */
static int text_width(HDC const dc, char const *const text) {
  wchar_t wide[256];
  SIZE size = {0, 0};

  if (text == NULL) {
    return 0;
  }
  memset(wide, 0, sizeof(wide));
  OV_SNPRINTF(wide, sizeof(wide) / sizeof(wide[0]), ph, ph, text);
  if (!GetTextExtentPoint32W(dc, wide, (int)wcslen(wide), &size)) {
    return 0;
  }
  return size.cx;
}

/** @brief The stages a run reports through */
static enum repair_stage const report_stages[] = {
    REPAIR_STAGE_WAIT,
    REPAIR_STAGE_PROBE,
    REPAIR_STAGE_REMOVE,
    REPAIR_STAGE_MIXER,
    REPAIR_STAGE_PAUSE,
    REPAIR_STAGE_VETO_RECORD,
    REPAIR_STAGE_START,
};

/**
 * @brief The stage text that needs the most room
 *
 * The window shows the step of the run behind the stage, and the step is the same for every
 * stage at the widest line: the text that is widest on its own is widest with the step too.
 *
 * @param st the window the run reports into, which knows the font to measure with
 * @param hwnd the window to measure on
 * @param out receives the text, UTF-8, always terminated
 * @param out_size room of out
 * @param err receives the failure of the measurement
 * @return false when the text could not be measured
 */
static NODISCARD bool
widest_stage_text(struct ui_state const *const st, HWND const hwnd, char *const out, size_t const out_size, struct ov_error *const err) {
  char text_buf[256];
  HDC dc = NULL;
  HFONT old = NULL;
  int room = 0;
  size_t i = 0;
  bool success = false;

  if ((out == NULL) || (out_size == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  strncpy(out, ui_progress_window_default_line(), out_size - 1);
  out[out_size - 1] = '\0';
  dc = GetDC(hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (st->font != NULL) {
    old = (HFONT)SelectObject(dc, st->font);
    if ((old == NULL) || (old == HGDI_ERROR)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      old = NULL;
      goto cleanup;
    }
  }
  room = text_width(dc, out);
  for (i = 0; i < (sizeof(report_stages) / sizeof(report_stages[0])); i++) {
    ui_progress_window_line(report_stages[i], st->settings, text_buf, sizeof(text_buf));
    int const width = text_width(dc, text_buf);

    if (width > room) {
      room = width;
      strncpy(out, text_buf, out_size - 1);
      out[out_size - 1] = '\0';
    }
  }
  success = true;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    if (!ReleaseDC(hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
  }
  return success;
}

/**
 * @brief The width the settings group has to have
 *
 * Measured from its own captions with the font this window uses.  A translation can be
 * longer than the English text, and a language can bring a font of its own, so the width
 * cannot be a constant in layout.h: the group would be too narrow for the label.
 *
 * @param st the window that draws the captions
 * @param hwnd the window to measure
 * @param err receives the failure of the measurement
 * @return the width in pixels, SIZE_MAX when the captions could not be read
 */
static NODISCARD size_t settings_group_width(struct ui_state *const st, HWND const hwnd, struct ov_error *const err) {
  char mixer_label[256];
  char const *const captions[] = {
      mixer_label,
      repair_option_audio_service_label(),
  };
  repair_option_mixer_label(st->settings, mixer_label, sizeof(mixer_label));
  int widest = 0;
  HDC dc = NULL;
  HFONT old = NULL;
  size_t i = 0;
  size_t result = SIZE_MAX;

  dc = GetDC(hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (st->font != NULL) {
    old = (HFONT)SelectObject(dc, st->font);
    if ((old == NULL) || (old == HGDI_ERROR)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      old = NULL;
      goto cleanup;
    }
  }
  for (i = 0; i < (sizeof(captions) / sizeof(captions[0])); i++) {
    int const width = text_width(dc, captions[i]);

    if (width > widest) {
      widest = width;
    }
  }
  widest += layout_scale(ADC_MAIN_CHECK_BOX_W + ADC_MAIN_CHECK_BOX_GAP + 2 * ADC_MAIN_GROUP_INSET, (UINT)st->dpi);
  if (widest < layout_scale(ADC_MAIN_SETTINGS_MIN_W, (UINT)st->dpi)) {
    widest = layout_scale(ADC_MAIN_SETTINGS_MIN_W, (UINT)st->dpi);
  }
  result = (size_t)widest;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      result = SIZE_MAX;
    }
    if (!ReleaseDC(hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      result = SIZE_MAX;
    }
  }
  return result;
}

/**
 * @brief The width a group caption needs
 *
 * The caption of a group only needs the room its text takes, a fixed width leaves the
 * frame line running through empty space.
 *
 * @param st the window that draws the caption
 * @param hwnd the window to measure
 * @param caption the static control that carries the caption
 * @param err receives the failure of the measurement
 * @param out receives the width in pixels
 * @return false when the caption could not be measured
 */
static NODISCARD bool
caption_width(struct ui_state *const st, HWND const hwnd, HWND const caption, int *const out, struct ov_error *const err) {
  HDC dc = NULL;
  HFONT old = NULL;
  wchar_t text[64];
  SIZE size = {0, 0};
  int width = 0;
  bool success = false;

  *out = px(st, ADC_MAIN_CAPTION_PAD);
  if (caption == NULL) {
    return true; // the caption is not there, and its room is the padding alone
  }
  memset(text, 0, sizeof(text));
  if (GetWindowTextW(caption, text, (int)(sizeof(text) / sizeof(text[0]))) == 0) {
    DWORD const last = GetLastError();

    if (last != ERROR_SUCCESS) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(last));
      goto cleanup;
    }
    return true; // an empty caption needs no room of its own
  }
  dc = GetDC(hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  old = (HFONT)SelectObject(dc, st->font);
  if ((old == NULL) || (old == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    old = NULL;
    goto cleanup;
  }
  if (!GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  width = size.cx + px(st, ADC_MAIN_CAPTION_PAD);
  if (width <= 0) {
    width = px(st, ADC_MAIN_CAPTION_PAD);
  }
  *out = width;
  success = true;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    if (!ReleaseDC(hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    dc = NULL;
  }
  return success;
}

/**
 * @brief The height the caption text takes with the font of the window
 *
 * The top line of a group frame runs through the middle of the caption, the
 * way a group box of the system draws its own, so the height is measured from
 * the font instead of guessed: a translation can bring a font of its own.
 *
 * @param st the window that draws the captions
 * @param hwnd the window to measure on
 * @param out receives the height in pixels
 * @param err receives the failure of the measurement
 * @return false when the font could not be asked
 */
static NODISCARD bool caption_height(struct ui_state *const st, HWND const hwnd, int *const out, struct ov_error *const err) {
  HDC dc = NULL;
  HFONT old = NULL;
  TEXTMETRICW tm;
  bool success = false;

  *out = 0;
  memset(&tm, 0, sizeof(tm));
  dc = GetDC(hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  old = (HFONT)SelectObject(dc, st->font);
  if ((old == NULL) || (old == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    old = NULL;
    goto cleanup;
  }
  if (!GetTextMetricsW(dc, &tm)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  *out = tm.tmHeight;
  success = true;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    if (!ReleaseDC(hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    dc = NULL;
  }
  return success;
}

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

/**
 * @brief The width of the settings group, measured or its minimum
 *
 * @param measured the width the caller measured, in physical pixels, 0 when there is none
 * @param dpi      the dpi of the window
 * @return the width the group is drawn with, in physical pixels
 */
static int group_min_width(int const measured, UINT const dpi) {
  return (measured > 0) ? measured : layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi);
}

/**
 * @brief The narrowest client area that shows both groups of the band side by side
 *
 * @param settings_min_w the width the settings group needs, in physical pixels
 * @param dpi            the dpi of the window
 * @return the width in physical pixels
 */
static int main_min_client_width(int const settings_min_w, UINT const dpi) {
  return group_min_width(settings_min_w, dpi) + layout_scale(ADC_MAIN_MIN_CLIENT_EXTRA_W, dpi);
}

/**
 * @brief The two boxes of a group: the frame, and the caption that rides on its top line
 *
 * The caption sits at the top of the group and the top line of the frame runs through the
 * middle of the caption text, the arrangement a group box of the system draws on its own.
 * A group the application themes itself therefore looks the same as one the system draws,
 * and the offset of the line is derived from the height of the caption, so it holds at
 * every dpi.
 *
 * @param frame receives the frame
 * @param caption receives the caption
 * @param left the left edge of the frame
 * @param top the top edge of the caption, the top of the group
 * @param width how wide the group is
 * @param height how tall the frame is
 * @param caption_height how tall the caption text is
 * @param caption_width the room the caption takes
 * @param caption_left where the caption starts
 */
static void group_box(RECT *const frame,
                      RECT *const caption,
                      int const left,
                      int const top,
                      int const width,
                      int const height,
                      int const caption_height,
                      int const caption_width,
                      int const caption_left) {
  int const line = top + caption_height / 2;

  frame->left = left;
  frame->top = line;
  frame->right = left + width;
  frame->bottom = line + height;
  caption->left = caption_left;
  caption->top = top;
  caption->right = caption_left + caption_width;
  caption->bottom = top + caption_height;
}

/**
 * @brief Compute the geometry of the main window
 *
 * @param client the client rectangle of the window
 * @param dpi the dpi of the window
 * @param settings_min_w the width the settings group needs in pixels, measured by the
 *                       caller from the captions of its check boxes
 * @param settings_caption_width the measured width of the settings caption in pixels
 * @param install_caption_width the measured width of the install caption in pixels
 * @param caption_height the measured height of a caption text in pixels, taken from the
 *                       font of the window: the top line of a group frame runs through the
 *                       middle of the caption, the way a group box of the system draws its
 *                       own.  0 falls back to the room the layout reserves for a caption.
 * @param out receives the geometry
 */
static void main_compute(RECT const *const client,
                         UINT const dpi,
                         int const settings_min_w,
                         int const settings_caption_width,
                         int const install_caption_width,
                         int const caption_height,
                         struct main_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const reserved = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
  int const caption_h = (caption_height > 0) ? caption_height : reserved;
  int const band_bottom = (client->bottom - client->top) - layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi);
  int const frame_top = band_bottom - layout_scale(ADC_MAIN_GROUP_H, dpi);
  int const group_top = frame_top - caption_h / 2;
  int const button_top = layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi);
  int const inset = layout_scale(ADC_MAIN_GROUP_INSET, dpi);
  int settings_w = group_min_width(settings_min_w, dpi);
  int install_left = margin + settings_w + layout_scale(ADC_MAIN_GROUP_GAP, dpi);
  int install_w = (client->right - client->left) - margin - install_left;
  int min_client_width = main_min_client_width(settings_min_w, dpi);
  int const task_top = frame_top + inset;

  if (install_w < inset * 2) {
    install_w = inset * 2;
  }
  memset(out, 0, sizeof(*out));
  out->button = layout_rect(margin, button_top, layout_scale(ADC_MAIN_BUTTON_W, dpi), layout_scale(ADC_MAIN_BUTTON_H, dpi));
  group_box(&out->settings_frame,
            &out->settings_caption,
            margin,
            group_top,
            settings_w,
            layout_scale(ADC_MAIN_GROUP_H, dpi),
            caption_h,
            settings_caption_width,
            margin + inset);
  group_box(&out->install_frame,
            &out->install_caption,
            install_left,
            group_top,
            install_w,
            layout_scale(ADC_MAIN_GROUP_H, dpi),
            caption_h,
            install_caption_width,
            install_left + inset);
  out->check_mixer = layout_rect(margin + inset, frame_top + inset, settings_w - 2 * inset, layout_scale(ADC_MAIN_CHECK_H, dpi));
  out->check_audio_service = layout_rect(margin + inset,
                                         frame_top + inset + layout_scale(ADC_MAIN_CHECK_H, dpi) + layout_scale(ADC_LAYOUT_GAP, dpi),
                                         settings_w - 2 * inset,
                                         layout_scale(ADC_MAIN_CHECK_H, dpi));
  {
    int const last_bottom = out->check_audio_service.bottom;
    int const shift = ((out->settings_frame.bottom - last_bottom) - (out->check_mixer.top - out->settings_frame.top)) / 2;
    if (shift != 0) {
      out->check_mixer.top += shift;
      out->check_mixer.bottom += shift;
      out->check_audio_service.top += shift;
      out->check_audio_service.bottom += shift;
    }
  }
  out->task_install = layout_rect(install_left + inset, task_top, install_w - 2 * inset, layout_scale(ADC_MAIN_TASK_H, dpi));
  out->task_remove = layout_rect(install_left + inset,
                                 task_top + layout_scale(ADC_MAIN_TASK_H, dpi) + layout_scale(ADC_MAIN_TASK_GAP, dpi),
                                 install_w - 2 * inset,
                                 layout_scale(ADC_MAIN_TASK_H, dpi));
  out->min_client_width = min_client_width;
}

/**
 * @brief Place every control of the window
 *
 * The geometry itself lives in main_compute(), this only hands the rectangles to the
 * window manager.  Keeping the arithmetic out of this file is what makes it testable.
 *
 * @param st the window that holds the controls
 * @param hwnd the window to place them in
 * @param client the client rectangle of the window
 * @param settings_min_w the width the settings group needs, measured for the font of the window
 * @param err receives the failure of the measurement or of the placement
 * @return false when a control could not be placed
 */
static NODISCARD bool
layout(struct ui_state *const st, HWND const hwnd, RECT const *const client, int const settings_min_w, struct ov_error *const err) {
  struct main_layout g;
  int height = 0;
  int settings_w = 0;
  int install_w = 0;
  bool success = false;

  if (!caption_height(st, hwnd, &height, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!caption_width(st, hwnd, st->group_settings, &settings_w, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!caption_width(st, hwnd, st->group_install, &install_w, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  main_compute(client, (UINT)st->dpi, settings_min_w, settings_w, install_w, height, &g);

  if (!MoveWindow(st->close_btn, LX(g.button), LY(g.button), LW(g.button), LH(g.button), TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!MoveWindow(st->chk_mixer, LX(g.check_mixer), LY(g.check_mixer), LW(g.check_mixer), LH(g.check_mixer), TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!MoveWindow(st->chk_audio_service,
                  LX(g.check_audio_service),
                  LY(g.check_audio_service),
                  LW(g.check_audio_service),
                  LH(g.check_audio_service),
                  TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (st->group_settings != NULL) {
    st->settings_frame_rc = g.settings_frame;
    st->settings_caption_rc = g.settings_caption;
    if (st->settings->use_theme) {
      if (!MoveWindow(
              st->group_settings, LX(g.settings_caption), LY(g.settings_caption), LW(g.settings_caption), LH(g.settings_caption), TRUE)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      }
    } else {
      /* the box of the system draws its caption from the top of the control and
         its top line at half the height of the caption, so the control spans the
         caption and the frame: both modes put the line through the middle of the
         same text */
      if (!MoveWindow(st->group_settings,
                      LX(g.settings_frame),
                      LY(g.settings_caption),
                      LW(g.settings_frame),
                      g.settings_frame.bottom - g.settings_caption.top,
                      TRUE)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      }
    }
  }
  if (st->group_install != NULL) {
    st->install_frame_rc = g.install_frame;
    st->install_caption_rc = g.install_caption;
    if (st->settings->use_theme) {
      if (!MoveWindow(
              st->group_install, LX(g.install_caption), LY(g.install_caption), LW(g.install_caption), LH(g.install_caption), TRUE)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      }
    } else {
      if (!MoveWindow(st->group_install,
                      LX(g.install_frame),
                      LY(g.install_caption),
                      LW(g.install_frame),
                      g.install_frame.bottom - g.install_caption.top,
                      TRUE)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      }
    }
    if (!MoveWindow(st->task_install, LX(g.task_install), LY(g.task_install), LW(g.task_install), LH(g.task_install), TRUE)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (!MoveWindow(st->task_remove, LX(g.task_remove), LY(g.task_remove), LW(g.task_remove), LH(g.task_remove), TRUE)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (!InvalidateRect(hwnd, NULL, TRUE)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Put the window at the size its layout asks for
 *
 * The only place that changes the size of the window.
 *
 * @param st             the window state, which knows the dpi the size is worked out for
 * @param hwnd           the window to put at that size
 * @param settings_min_w the width the settings group needs, in pixels
 * @param err receives the failure of the resize
 * @return false when the window could not be put at that size
 */
static NODISCARD bool
apply_window_size(struct ui_state const *const st, HWND const hwnd, int const settings_min_w, struct ov_error *const err) {
  RECT outer;
  RECT now;

  layout_window_rect(main_min_client_width(settings_min_w, (UINT)st->dpi),
                     layout_scale(ADC_MAIN_WINDOW_H, (UINT)st->dpi),
                     (UINT)st->dpi,
                     main_window_style,
                     0,
                     &outer);
  if (!GetWindowRect(hwnd, &now)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return true; // already at the size the layout asks for
  }
  if (!SetWindowPos(hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief Bring the window onto a monitor of its own dpi
 *
 * Everything that depends on the dpi of this window goes through here, so that moving the
 * window to another monitor needs one call only.
 *
 * @param st the window that holds the controls
 * @param hwnd the window to redraw for
 * @param dpi the dpi to draw for
 * @param err receives the failure of the relayout
 * @return false when the window could not be laid out for that dpi
 */
static NODISCARD bool apply_dpi(struct ui_state *const st, HWND const hwnd, int const dpi, struct ov_error *const err) {
  /* the controls the font of the window goes to: a control added
   * here is one line, and the count is the array's own */
  HWND const kids[] = {
      st->close_btn, st->chk_mixer, st->chk_audio_service, st->group_settings, st->group_install, st->task_install, st->task_remove};
  HFONT stale = NULL;
  size_t i = 0;
  RECT rc;
  size_t settings_w = 0;
  bool success = false;

  if (hwnd == NULL || dpi <= 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  stale = st->font;
  st->font = layout_make_font((UINT)dpi);
  if (st->font == NULL) {
    st->font = stale;
    stale = NULL;
    goto cleanup;
  }
  st->dpi = dpi;
  for (i = 0; i < (sizeof(kids) / sizeof(kids[0])); i++) {
    if (kids[i] != NULL) {
      SendMessageW(kids[i], WM_SETFONT, (WPARAM)st->font, TRUE);
    }
  }
  settings_w = settings_group_width(st, hwnd, err);
  if (settings_w == SIZE_MAX) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!apply_window_size(st, hwnd, (int)settings_w, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!GetClientRect(hwnd, &rc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!layout(st, hwnd, &rc, (int)settings_w, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!InvalidateRect(hwnd, NULL, TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  if (stale != NULL) {
    if (!DeleteObject(stale)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      success = false;
    }
    stale = NULL;
  }
  return success;
}

/**
 * @brief The state the reader has to hear about first
 *
 * A problem outranks a state nobody knows, which outranks
 * the fault this tool cures, which outranks a device that
 * works.  A list that holds no device is the state of no
 * device at all.
 *
 * @param targets the devices the walk found
 * @param count how many devices the list holds
 * @param faulted receives the answer: a device is in the state this tool cures
 * @return the state the press answers from
 */
static enum device_state targets_state(struct device_target const *const targets, size_t const count, bool *const faulted) {
  enum device_state state = DEVICE_STATE_ABSENT;
  size_t i = 0;

  *faulted = false;
  for (i = 0; i < count; i++) {
    enum device_state const one = targets[i].state;

    if (one == DEVICE_STATE_FAULTED) {
      *faulted = true;
    }
    if (one == DEVICE_STATE_PROBLEM) {
      state = DEVICE_STATE_PROBLEM; // not this tool's to cure
    } else if ((one == DEVICE_STATE_NO_INTERFACE) && (state != DEVICE_STATE_PROBLEM)) {
      state = DEVICE_STATE_NO_INTERFACE; // nothing is registered, so nothing is trusted
    } else if ((one == DEVICE_STATE_FAULTED) && (state != DEVICE_STATE_PROBLEM) && (state != DEVICE_STATE_NO_INTERFACE)) {
      state = DEVICE_STATE_FAULTED; // the fault this tool cures
    } else if ((one == DEVICE_STATE_HEALTHY) && (state == DEVICE_STATE_ABSENT)) {
      state = DEVICE_STATE_HEALTHY; // a device is there, and it works
    }
  }
  return state;
}

/**
 * @brief Put a message up for the window, reporting a post that did not go through
 *
 * @param hwnd the window the message is for
 * @param msg the message to post
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 */
static void post_to(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  if (!PostMessageW(hwnd, msg, wparam, lparam)) {
    struct ov_error err = {0};

    OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_REPORT(&err, NULL);
  }
}

/**
 * @brief Show the step the run is at
 *
 * @param job the run that is reporting
 * @param stage what the run is waiting for
 * @param step the step of the run
 */
static void report_step(struct repair_job *const job, enum repair_stage const stage, size_t const step) {
  post_to(job->hwnd, WM_ADC_STAGE, (WPARAM)stage, (LPARAM)step);
}

/**
 * @brief The callback the repair reports its progress through
 *
 * The repair counts its own steps from one, the wait of a scheduled run takes the place in
 * front of them.
 *
 * @param stage what the run is waiting for
 * @param step the step of the run
 * @param total unused, the window counts its own steps
 * @param userdata the run that is going
 */
static void repair_progress(enum repair_stage const stage, size_t const step, size_t const total, void *const userdata) {
  struct repair_job *const job = (struct repair_job *)(void *)userdata;

  job->total = total;
  report_step(job, stage, step + (job->wait_first ? 1 : 0));
}

/**
 * @brief Tell the window that a wait of the run is over
 *
 * Called from the thread that finished the wait, so it does no more than put a message up.
 *
 * @param userdata the run going on
 */
static void wake_repair(void *const userdata) {
  struct repair_job *const job = (struct repair_job *)(void *)userdata;

  post_to(job->hwnd, WM_ADC_ASYNC, 0, 0);
}

/**
 * @brief Take the run forward when the reader asked it to stop
 *
 * The ask is an event the run reads between its steps, so the window of the run has to be
 * taken forward for the ask to be read at all: it is the message the waits of the run use.
 *
 * @param userdata the state of the window
 */
static void cancel_repair(void *const userdata) {
  struct ui_state *const st = (struct ui_state *)(void *)userdata;

  post_to(st->window, WM_ADC_ASYNC, 0, 0);
}

/**
 * @brief Take the run forward as far as it goes without waiting
 *
 * Runs on the thread of the window, so it never waits: a wait of the run is a message that
 * brings the window back here.
 *
 * @param st the window that carries the job
 * @param hwnd the window the job reports to
 */
static void step_repair(struct ui_state *const st, HWND const hwnd) {
  struct repair_job *const job = &st->job;

  if (job->run == NULL) {
    return; // the run is over and a wake that was already on its way came in
  }
  if (repair_step(job->run, &job->err) == REPAIR_STEP_DONE) {
    job->ok = repair_run_finish(&job->run, &job->err);
    OV_ERROR_REPORT(&job->err, NULL);
    logger_log_run_end(job->result.message);
    post_to(hwnd, WM_ADC_DONE, 0, 0);
  }
}

/**
 * @brief Look at the devices and take the run up
 *
 * The probe is a handful of questions and is answered here; from there on the run does its
 * waiting on threads of its own.
 *
 * @param st the window that carries the job
 * @param hwnd the window the job reports to
 */
static void begin_repair(struct ui_state *const st, HWND const hwnd) {
  struct repair_job *const job = &st->job;
  struct ov_error probe_err = {0};
  struct device_target *targets = NULL;
  enum device_state state = DEVICE_STATE_QUERY_FAILED; // the fallback while the probe could not answer
  bool faulted = false;

  logger_log_run_start();
  targets = device_probe_all(job->settings->ks_service, job->settings->devices, OV_ARRAY_LENGTH(job->settings->devices), &probe_err);
  if (targets != NULL) {
    state = targets_state(targets, OV_ARRAY_LENGTH(targets), &faulted);
  } else if (ov_error_is(&probe_err, ov_error_type_generic, ov_error_generic_not_found)) {
    // no device of the service is present, which is not a failure
    state = DEVICE_STATE_ABSENT;
    OV_ERROR_DESTROY(&probe_err);
  } else {
    // the devices could not be read at all, so there is nothing to work on: the log keeps why,
    // and the dialog the run would have ended with carries the answer to the reader
    OV_ERROR_REPORT(&probe_err, NULL);
    job->ok = false;
    job->result.outcome = REPAIR_OUTCOME_FAILED;
    job->result.failure = repair_failure_device_state;
    device_targets_release(&job->targets);
    post_to(hwnd, WM_ADC_DONE, 0, 0);
    return;
  }
  device_targets_release(&targets);
  // a run nobody is watching looks for the fault this tool cures: a device
  // that works next to one that is stuck is a run that has work to do
  if (ui_auto_run_action(job->wait_first, faulted ? DEVICE_STATE_FAULTED : state) == UI_AUTO_RUN_LEAVE) {
    job->leave = true;
    logger_log_run_end(ui_auto_run_sentence(state));
    post_to(hwnd, WM_ADC_DONE, 0, 0);
    return;
  }
  job->run = repair_run_begin(job->settings,
                              job->targets,
                              job->allow_mixer_close,
                              job->allow_audio_service_pause,
                              repair_progress,
                              job,
                              job->cancel,
                              wake_repair,
                              job,
                              &job->result,
                              &job->err);
  if (job->run == NULL) {
    OV_ERROR_REPORT(&job->err, NULL);
    device_targets_release(&job->targets);
    job->ok = false;
    post_to(hwnd, WM_ADC_DONE, 0, 0);
    return;
  }
  step_repair(st, hwnd);
}

/**
 * @brief The headline of the result dialog: the verdict of the run
 *
 * @param outcome what came of the run
 * @return the translated sentence, UTF-8
 */
static char const *result_headline(enum repair_outcome const outcome) {
  switch (outcome) {
  case REPAIR_OUTCOME_CLOSED:
    return gettext("Success.");
  case REPAIR_OUTCOME_ABSENT:
    return gettext("No repair is needed.");
  case REPAIR_OUTCOME_FAILED:
  case REPAIR_OUTCOME_VETOED:
  case REPAIR_OUTCOME_OTHER_PROBLEM:
  case REPAIR_OUTCOME_ABORTED:
  case REPAIR_OUTCOME_UNKNOWN:
    break;
  }
  return gettext("Failed.");
}

/**
 * @brief Does the window close itself when the run is done
 *
 * @param auto_run the scheduler started this run
 * @param outcome what came of the run
 * @return true when there is nothing left for a person to do here
 */
static bool ui_closes_itself_when_done(bool const auto_run, enum repair_outcome const outcome) {
  return auto_run && (outcome == REPAIR_OUTCOME_CLOSED);
}

/**
 * @brief Does the run end in a dialog
 *
 * @param auto_run the scheduler started this run
 * @param outcome what came of the run
 * @return true when the window stays open and the outcome is worth a dialog
 */
static bool ui_shows_result_dialog(bool const auto_run, enum repair_outcome const outcome) {
  return !auto_run || (outcome != REPAIR_OUTCOME_CLOSED);
}

static enum ui_press ui_press_action(struct device_target const *const targets, size_t const target_count) {
  bool faulted = false;

  if (target_count == 0) {
    return UI_PRESS_NOT_REPAIRABLE; // nothing is connected, so the press has nothing to answer
  }
  switch (targets_state(targets, target_count, &faulted)) {
  case DEVICE_STATE_FAULTED:
    return UI_PRESS_REPAIR; // the fault this tool cures answers at once
  case DEVICE_STATE_PROBLEM:
  case DEVICE_STATE_NO_INTERFACE:
  case DEVICE_STATE_QUERY_FAILED:
    return UI_PRESS_NOT_REPAIRABLE; // not this tool's to cure, or no endpoint is there
  case DEVICE_STATE_HEALTHY:
  case DEVICE_STATE_ABSENT:
    break;
  }
  return UI_PRESS_ASK; // every device works: the reader names the one to release
}

/**
 * @brief The two lines of the result dialog
 *
 * The headline carries the verdict, the body the reason the run worked out.  Both are
 * sentences of the translation; what a run leaves in repair_result.message goes to the log.
 *
 * @param outcome what came of the run
 * @param removal_error why the removal was refused, 0 when it was for another reason
 * @param failure which step of the repair could not be taken, 0 when the outcome says it
 * @param mixer_name the process that could not be closed, NULL when none is to be named
 * @param headline receives the verdict, UTF-8
 * @param headline_size room of headline
 * @param body receives what happened, UTF-8
 * @param body_size room of body
 */
static void ui_result_dialog_text(enum repair_outcome const outcome,
                                  enum app_error_code const removal_error,
                                  enum repair_failure const failure,
                                  wchar_t const *const mixer_name,
                                  char *const headline,
                                  size_t const headline_size,
                                  char *const body,
                                  size_t const body_size) {
  char const *text = gettext("The device could not be closed."); // the switches below name the answer
  char named[256];

  if ((headline == NULL) || (headline_size == 0) || (body == NULL) || (body_size == 0)) {
    return;
  }
  if ((outcome == REPAIR_OUTCOME_FAILED) && (failure == repair_failure_process_close) && (mixer_name != NULL)) {
    // the process that could not be closed is named: the sentence says nothing without it
    char const *const format = gettext("%1$hs could not be closed.");
    char app[128];

    app[0] = '\0';
    ov_wchar_to_utf8(mixer_name, wcslen(mixer_name), app, sizeof(app), NULL);
    OV_SNPRINTF(named, sizeof(named), "%1$hs could not be closed.", format, app);
    text = named;
  } else if (outcome == REPAIR_OUTCOME_FAILED) {
    switch (failure) {
    case repair_failure_audio_service_start:
      text = gettext("The Windows Audio service could not be started again.");
      break;
    case repair_failure_audio_service_pause:
      text = gettext("The Windows Audio service could not be stopped.");
      break;
    case repair_failure_device_state:
      text = gettext("The state of the device could not be determined.");
      break;
    case repair_failure_record:
      text = gettext("The record of what is using the device could not be read.");
      break;
    case repair_failure_blockers:
      text = gettext("The process that is using the device could not be found.");
      break;
    case repair_failure_process_close:
      text = gettext("A process that is using the device could not be closed.");
      break;
    case repair_failure_not_recovered:
      text = gettext("The device was released, but it did not come back as an audio device.");
      break;
    case repair_failure_not_confirmed:
      text = gettext("The device was released, but whether it came back could not be checked.");
      break;
    case repair_failure_none:
      break;
    }
    if (failure == repair_failure_none) {
      switch (removal_error) {
      case removal_error_access_denied:
        text = gettext("This program is not allowed to close the device.\n\n"
                       "Run it as an administrator, then close the device again.");
        break;
      case removal_error_device_gone:
        text = gettext("The device was already gone.");
        break;
      case removal_error_veto:
      case removal_error_call_failed:
      case removal_error_refused:
      case removal_error_none:
      case task_error_not_installed:
        break;
      }
    }
  } else {
    switch (outcome) {
    case REPAIR_OUTCOME_CLOSED:
      text = gettext("The device was released.");
      break;
    case REPAIR_OUTCOME_VETOED:
      text = gettext("A process is still using the device, so it could not be released.");
      break;
    case REPAIR_OUTCOME_ABSENT:
      text = gettext("No Audient device was found.");
      break;
    case REPAIR_OUTCOME_OTHER_PROBLEM:
      text = gettext("The device has a problem that this tool cannot solve.");
      break;
    case REPAIR_OUTCOME_ABORTED:
      text = gettext("Aborted by the user.");
      break;
    case REPAIR_OUTCOME_FAILED:
    case REPAIR_OUTCOME_UNKNOWN:
      break;
    }
  }
  strncpy(body, text, body_size - 1);
  body[body_size - 1] = '\0';
  strncpy(headline, result_headline(outcome), headline_size - 1);
  headline[headline_size - 1] = '\0';
}
/**
 * @brief Show the result of the run in a dialog
 *
 * @param hwnd the window that is asking
 * @param s the settings the run worked for, which name the mixers
 * @param r what the run left
 */
static void show_result_dialog(HWND const hwnd, struct settings const *const s, struct repair_result const *const r) {
  wchar_t wheadline[256];
  wchar_t wbody[1600];
  char headline[256];
  char body[1600];
  wchar_t const *mixer_name = NULL;

  if (r->failure == repair_failure_process_close) {
    wchar_t const *const *const names = settings_mixer_names(s);
    size_t const count = settings_mixer_name_count(s);

    if ((names != NULL) && (r->failed_mixer < count)) {
      mixer_name = names[r->failed_mixer];
    }
  }
  ui_result_dialog_text(r->outcome, r->removal_error, r->failure, mixer_name, headline, sizeof(headline), body, sizeof(body));
  if (headline[0] == '\0') {
    return;
  }
  if (OV_SNPRINTF(wheadline, sizeof(wheadline) / sizeof(wheadline[0]), ph, ph, headline) < 0) {
    return;
  }
  if (OV_SNPRINTF(wbody, sizeof(wbody) / sizeof(wbody[0]), ph, ph, body) < 0) {
    return;
  }
  report_dialog(hwnd, wheadline, wbody, r->outcome != REPAIR_OUTCOME_CLOSED);
}

/**
 * @brief Answer a press on a device the tool cannot handle with its dialog
 *
 * The machine is asked for the sentence it has for the problem it named, and the answer is
 * turned into the sentence of the reader's language here.
 *
 * @param hwnd the window that is asking
 * @param problem_code the problem the machine named, DEVICE_PROBLEM_NO_REPORT when none was read
 */
static void report_not_repairable(HWND const hwnd, uint32_t const problem_code) {
  wchar_t title[128];
  wchar_t text[512];
  char const *const id = device_manager()->problem_id(problem_code);

  OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), ph, ph, gettext("Failed."));
  OV_SNPRINTF(
      text, sizeof(text) / sizeof(text[0]), ph, ph, gettext((id != NULL) ? id : "This tool cannot handle the problem that was found."));
  report_dialog(hwnd, title, text, TRUE);
}

/**
 * @brief Answer a press that found no device with its dialog
 *
 * @param hwnd the window that is asking
 */
static void report_no_device(HWND const hwnd) {
  wchar_t title[128];
  wchar_t text[512];

  OV_SNPRINTF(title, sizeof(title) / sizeof(title[0]), ph, ph, gettext("No repair is needed."));
  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("No Audient device was found."));
  report_dialog(hwnd, title, text, FALSE);
}

/**
 * @brief Give back what the job of the window holds
 *
 * The window is taken down by more than the end of its own run -- a quit from elsewhere
 * ends the loop the run was taken forward from -- so what the job holds is given back
 * here, from wherever the window is destroyed, rather than only from the end of a run.
 *
 * @param st the window that carries the job
 */
static void release_job(struct ui_state *const st) {
  struct repair_job *const job = &st->job;

  repair_result_release(&job->result);
  if (job->cancel != NULL) {
    CloseHandle(job->cancel);
    job->cancel = NULL;
  }
  if (job->targets != NULL) {
    device_targets_release(&job->targets);
  }
}

/**
 * @brief Bring the window onto what the run left behind
 *
 * @param st the window that ran the job
 * @param hwnd the window to redraw
 */
static void finish_repair(struct ui_state *const st, HWND const hwnd) {
  struct repair_job *const job = &st->job;
  bool post_close = false;

  st->busy = false;
  {
    struct ov_error err = {0};

    if (!ui_progress_window_destroy(&st->progress, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  }
  if (job->leave) {
    post_close = true;
    goto cleanup;
  }
  enable_actions(st, true); // a run that leaves the window open leaves it usable
  if (!job->ok) {
    show_result_dialog(hwnd, st->settings, &job->result);
    goto cleanup;
  }
  post_close = ui_closes_itself_when_done(st->options->auto_run, job->result.outcome);
  if (job->result.outcome == REPAIR_OUTCOME_VETOED) {
    struct ov_error berr = {0};

    if (!ui_blockers_window_show(hwnd, job->result.events, job->result.closed, OV_ARRAY_LENGTH(job->result.closed), &berr)) {
      OV_ERROR_REPORT(&berr, NULL);
      show_result_dialog(hwnd, st->settings, &job->result);
    }
    goto cleanup;
  }
  if (ui_shows_result_dialog(st->options->auto_run, job->result.outcome)) {
    show_result_dialog(hwnd, st->settings, &job->result);
  }

cleanup:
  release_job(st);
  if (post_close || st->close_when_done) {
    post_to(hwnd, WM_CLOSE, 0, 0);
  }
}

/**
 * @brief Start the work of one button press
 *
 * @param st the window that carries the job
 * @param hwnd the window the job reports to
 * @param wait_first the run waits for the devices before it looks at them
 */
static void do_repair(struct ui_state *const st, HWND const hwnd, bool const wait_first, struct device_target *const targets) {
  memset(&st->job, 0, sizeof(st->job));
  st->job.hwnd = hwnd;
  st->job.settings = st->settings;
  st->job.targets = targets;
  st->job.wait_first = wait_first;
  st->job.allow_mixer_close = (SendMessageW(st->chk_mixer, BM_GETCHECK, 0, 0) == BST_CHECKED);
  st->job.allow_audio_service_pause = (SendMessageW(st->chk_audio_service, BM_GETCHECK, 0, 0) == BST_CHECKED);
  st->busy = true;
  enable_actions(st, false);
  {
    struct ov_error cerr = {0};

    st->job.cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (st->job.cancel == NULL) {
      OV_ERROR_SET_HRESULT(&cerr, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&cerr, NULL);
      st->busy = false;
      enable_actions(st, true);
      device_targets_release(&st->job.targets);
      memset(&st->job, 0, sizeof(st->job));
      return;
    }
  }
  {
    struct ov_error perr = {0};
    char wait_buf[256];
    char widest_buf[256];
    bool made = false;

    ui_progress_window_line(REPAIR_STAGE_WAIT, st->settings, wait_buf, sizeof(wait_buf));
    if (!widest_stage_text(st, hwnd, widest_buf, sizeof(widest_buf), &perr)) {
      OV_ERROR_ADD_TRACE(&perr);
    } else if (!ui_progress_window_create(hwnd,
                                          st->dpi,
                                          ui_progress_window_steps(st->job.wait_first, 1),
                                          st->job.cancel,
                                          cancel_repair,
                                          st,
                                          wait_first ? wait_buf : ui_progress_window_default_line(),
                                          widest_buf,
                                          &st->progress,
                                          &perr)) {
      OV_ERROR_ADD_TRACE(&perr);
    } else {
      made = true;
    }
    if (!made) {
      OV_ERROR_REPORT(&perr, NULL);
      st->busy = false;
      enable_actions(st, true);
      return;
    }
    if (!UpdateWindow(hwnd)) {
      OV_ERROR_SET_HRESULT(&perr, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&perr, NULL);
    }
    if (st->job.wait_first) {
      report_step(&st->job, REPAIR_STAGE_WAIT, 1);
      if (SetTimer(hwnd, IDT_AUTORUN, (UINT)st->job.settings->auto_run_delay_ms, NULL) == 0) {
        OV_ERROR_SET_HRESULT(&perr, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&perr, NULL);
      }
      return;
    }
  }
  begin_repair(st, hwnd);
}

/**
 * @brief Ask which device to release when none of them is faulted
 *
 * None of the connected devices is in the state this tool
 * cures, so the press has nothing to do on its own: the
 * reader names the device to release, and the run repairs
 * that one.  A window that could not be made is written to
 * the log only: the press has no other answer to give.
 *
 * @param st the state of the window
 * @param hwnd the window that asks
 * @param targets the devices that are connected, the press releases the list
 */
static void ask_which_device(struct ui_state *const st, HWND const hwnd, struct device_target *targets) {
  struct device_target *picked = NULL;
  struct ov_error perr = {0};

  if (!ui_device_picker_show(hwnd, st->settings, targets, &picked, &perr)) {
    OV_ERROR_REPORT(&perr, NULL);
    goto cleanup;
  }
  if (picked != NULL) {
    do_repair(st, hwnd, false, picked);
  }

cleanup:
  device_targets_release(&targets);
}

/**
 * @brief Paint what no theme draws: the group frames and the background
 *
 * @param hwnd the window to paint
 * @param st the window that holds the layout
 * @param err receives the failure of the painting
 * @return false when the window could not be painted
 */
static NODISCARD bool window_paint(HWND const hwnd, struct ui_state *const st, struct ov_error *const err) {
  PAINTSTRUCT ps;
  HDC dc = NULL;
  bool success = false;

  dc = BeginPaint(hwnd, &ps);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (st->settings->use_theme) {
    /* the frames of the groups are painted here only while the theme is on:
       without it, the group boxes of the system draw their own */
    if (st->group_settings != NULL) {
      if (!theme_frame(dc, &st->settings_frame_rc, &st->settings_caption_rc, st->dpi, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
    if (st->group_install != NULL) {
      if (!theme_frame(dc, &st->install_frame_rc, &st->install_caption_rc, st->dpi, err)) {
        OV_ERROR_ADD_TRACE(err);
        goto cleanup;
      }
    }
  }
  success = true;

cleanup:
  if (dc != NULL) {
    if (!EndPaint(hwnd, &ps)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      success = false;
    }
    dc = NULL;
  }
  return success;
}

/**
 * @brief Make the controls of the window and start what it does by itself
 *
 * @param st the state of the window
 * @param hwnd the window to fill in
 * @param err receives the failure of the controls or of their attach
 * @return false when the window could not be filled in
 */
static NODISCARD bool build(struct ui_state *const st, HWND const hwnd, struct ov_error *const err) {
  bool success = false;
  struct {
    wchar_t title[128];
    wchar_t button[256];
    wchar_t check_mixer[256];
    wchar_t check_audio_service[256];
    wchar_t group_settings[128];
    wchar_t group_install[128];
    wchar_t task_install[256];
    wchar_t task_remove[256];
  } text = {0};
  UINT dpi = 0;

  if (!theme_attach(hwnd, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  OV_SNPRINTF(text.title, sizeof(text.title) / sizeof(text.title[0]), ph, ph, gettext("Audient Device Closer"));
  if (!SetWindowTextW(hwnd, text.title)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  st->dpi = (int)dpi;
  OV_SNPRINTF(text.button, sizeof(text.button) / sizeof(text.button[0]), ph, ph, gettext("Close the Audient device"));
  {
    char mixer_label[256];

    repair_option_mixer_label(st->settings, mixer_label, sizeof(mixer_label));
    OV_SNPRINTF(text.check_mixer, sizeof(text.check_mixer) / sizeof(text.check_mixer[0]), ph, ph, mixer_label);
  }
  OV_SNPRINTF(text.check_audio_service,
              sizeof(text.check_audio_service) / sizeof(text.check_audio_service[0]),
              ph,
              ph,
              repair_option_audio_service_label());
  OV_SNPRINTF(text.group_settings, sizeof(text.group_settings) / sizeof(text.group_settings[0]), ph, ph, gettext("Behaviour"));
  OV_SNPRINTF(text.group_install, sizeof(text.group_install) / sizeof(text.group_install[0]), ph, ph, gettext("Install"));
  OV_SNPRINTF(text.task_install, sizeof(text.task_install) / sizeof(text.task_install[0]), ph, ph, gettext("Register the scheduled task"));
  OV_SNPRINTF(text.task_remove, sizeof(text.task_remove) / sizeof(text.task_remove[0]), ph, ph, gettext("Remove the scheduled task"));

  if (st->settings->use_theme) {
    /* the caption is a control of its own and the frame is painted by the
       window, so the theme can colour both */
    st->group_settings = child(st, hwnd, L"STATIC", text.group_settings, SS_LEFT, 0, 0, 0, 0, IDC_GROUP_SETTINGS, err);
    st->group_install = child(st, hwnd, L"STATIC", text.group_install, SS_LEFT, 0, 0, 0, 0, IDC_GROUP_INSTALL, err);
  } else {
    /* without the theme the group is the box of the system: comctl32 draws
       its frame and its caption in the colours of the system, which is what
       a window that asks the system for everything looks like.  The box is
       made before the controls it holds, so it sits under them. */
    st->group_settings = child(st, hwnd, L"BUTTON", text.group_settings, BS_GROUPBOX, 0, 0, 0, 0, IDC_GROUP_SETTINGS, err);
    st->group_install = child(st, hwnd, L"BUTTON", text.group_install, BS_GROUPBOX, 0, 0, 0, 0, IDC_GROUP_INSTALL, err);
  }
  st->close_btn = child(st, hwnd, L"BUTTON", text.button, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_CLOSE_BTN, err);
  st->chk_mixer = child(st, hwnd, L"BUTTON", text.check_mixer, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, IDC_CHK_MIXER, err);
  st->chk_audio_service =
      child(st, hwnd, L"BUTTON", text.check_audio_service, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, IDC_CHK_AUDIO_SERVICE, err);
  st->task_install = child(st, hwnd, L"BUTTON", text.task_install, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_TASK_INSTALL, err);
  st->task_remove = child(st, hwnd, L"BUTTON", text.task_remove, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_TASK_REMOVE, err);
  if ((st->group_settings == NULL) || (st->group_install == NULL) || (st->close_btn == NULL) || (st->chk_mixer == NULL) ||
      (st->chk_audio_service == NULL) || (st->task_install == NULL) || (st->task_remove == NULL)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  // the choice to pause the audio service takes the sound away from the whole machine,
  // so a build that ships only offers it when the settings say so in as many words
  if (!repair_can_pause_audio_service(st->settings)) {
    if (!ShowWindow(st->chk_audio_service, SW_HIDE)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  if (!apply_dpi(st, hwnd, st->dpi, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  SendMessageW(st->chk_mixer, BM_SETCHECK, st->options->close_mixer ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(st->chk_audio_service, BM_SETCHECK, st->options->allow_audio_service_pause ? BST_CHECKED : BST_UNCHECKED, 0);
  success = true;

cleanup:
  return success;
}

/**
 * @brief The window procedure of the main window
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @return the answer the message carries
 */
static LRESULT CALLBACK wnd_proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct ui_state *st = (struct ui_state *)GetPropW(hwnd, state_prop);

  switch (msg) {
  case WM_NCCREATE:
    if (SetPropW(hwnd, state_prop, (HANDLE)((CREATESTRUCTW const *)lparam)->lpCreateParams) == 0) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
      return FALSE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  default:
    break;
  }
  if (st == NULL) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  {
    LRESULT handled_result = 0;
    struct ov_error err = {0};
    ov_tribool const handled = theme_message(hwnd, msg, wparam, lparam, &handled_result, &err);

    if (handled == ov_indeterminate) {
      OV_ERROR_REPORT(&err, NULL);
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (handled == ov_true) {
      return handled_result;
    }
  }

  switch (msg) {
  case WM_CREATE: {
    struct ov_error berr = {0};

    if (!build(st, hwnd, &berr)) {
      OV_ERROR_REPORT(&berr, NULL);
      return -1; // the window is taken down and the start-up says why
    }
    if (st->options->auto_run) {
      do_repair(st, hwnd, true, NULL);
    }
    return 0;
  }
  case WM_SIZE: {
    RECT rc;

    if (!GetClientRect(hwnd, &rc)) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
      return 0;
    }
    if ((rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0) {
      struct ov_error err = {0};
      size_t const settings_w = settings_group_width(st, hwnd, &err);

      if (settings_w == SIZE_MAX) {
        OV_ERROR_REPORT(&err, NULL);
      } else if (!layout(st, hwnd, &rc, (int)settings_w, &err)) {
        OV_ERROR_REPORT(&err, NULL);
      }
    }
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;
    int const dpi = (int)LOWORD(wparam);
    struct ov_error err = {0};
    bool ok = false;

    st->dpi = dpi;
    /* the monitor suggests where the window goes; how big it is, the layout says */
    if (!SetWindowPos(hwnd, NULL, rc->left, rc->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    } else if (!apply_dpi(st, hwnd, dpi, &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else {
      ok = true;
    }
    if (!ok) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_COMMAND:
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_CLOSE_BTN) && !st->busy) {
      // the press looks at the devices for itself: the answer of an earlier run
      // says nothing about the state they are in now
      struct device_target *targets = NULL;
      struct ov_error perr = {0};

      targets = device_probe_all(st->settings->ks_service, st->settings->devices, OV_ARRAY_LENGTH(st->settings->devices), &perr);
      if (targets == NULL) {
        if (!ov_error_is(&perr, ov_error_type_generic, ov_error_generic_not_found)) {
          OV_ERROR_REPORT(&perr, NULL);
          // the devices could not be read, so the machine named no problem of its own
          report_not_repairable(hwnd, DEVICE_PROBLEM_NO_REPORT);
          return 0;
        }
        // no device of the service is present, which the press answers as there being nothing
        // to repair rather than as a problem
        OV_ERROR_DESTROY(&perr);
        report_no_device(hwnd);
        return 0;
      }
      switch (ui_press_action(targets, OV_ARRAY_LENGTH(targets))) {
      case UI_PRESS_NOT_REPAIRABLE: {
        // the state the press answers from is the worst one of the list, so the sentence is
        // the one of the first device that named a problem of its own
        uint32_t problem_code = DEVICE_PROBLEM_NO_REPORT;

        for (size_t i = 0; i < OV_ARRAY_LENGTH(targets); i++) {
          if (targets[i].state == DEVICE_STATE_PROBLEM) {
            problem_code = targets[i].problem_code;
            break;
          }
        }
        report_not_repairable(hwnd, problem_code);
        break;
      }
      case UI_PRESS_REPAIR:
        device_targets_keep_faulted(&targets);
        do_repair(st, hwnd, false, targets);
        break;
      case UI_PRESS_ASK:
        // none of the connected devices is faulted, so the press has nothing
        // to do on its own: the reader names the device to release
        ask_which_device(st, hwnd, targets);
        break;
      }
    }
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_TASK_INSTALL)) {
      install_task(st, hwnd);
    }
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_TASK_REMOVE)) {
      remove_task(st, hwnd);
    }
    return 0;
  case WM_ADC_STAGE:
    if (st->busy) {
      char stage_buf[256];
      size_t const steps =
          (st->job.total != 0) ? (st->job.total + (st->job.wait_first ? 1 : 0)) : ui_progress_window_steps(st->job.wait_first, 1);
      struct ov_error err = {0};

      ui_progress_window_line((enum repair_stage)wparam, st->settings, stage_buf, sizeof(stage_buf));
      if (!ui_progress_window_report(st->progress, (size_t)lparam, stage_buf, steps, &err)) {
        OV_ERROR_REPORT(&err, NULL);
      }
    }
    return 0;
  case WM_TIMER:
    if (wparam == IDT_AUTORUN) {
      if (!KillTimer(hwnd, IDT_AUTORUN)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      if (st->busy) {
        begin_repair(st, hwnd);
      }
    }
    return 0;
  case WM_ADC_ASYNC:
    if (st->busy) {
      step_repair(st, hwnd);
    }
    return 0;
  case WM_ADC_DONE:
    finish_repair(st, hwnd);
    return 0;
  case WM_CLOSE:
    if (st->busy) {
      st->close_when_done = true;
      return 0;
    }
    break;
  case WM_QUERYENDSESSION:
    // the answer of DefWindowProcW is TRUE, which is the one to give: this tool has no work
    // that a logoff or a shutdown should wait for
    break;
  case WM_ENDSESSION:
    // WM_ENDSESSION gives no leave: the session ends and the process goes with it, so
    // nothing of the run can be settled here without waiting on a removal that takes
    // seconds.  The line is what is left to say how far the run had come; the log is
    // flushed as it is written, so it is on disk even though logger_close never runs
    if ((wparam != 0) && st->busy) {
      logger_notef("run", "WM_ENDSESSION ended the session with the run still going");
    }
    return 0;
  case WM_PAINT: {
    struct ov_error err = {0};

    if (!window_paint(hwnd, st, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  }
    return 0;

  case WM_DESTROY:
    // the window goes away with the run still in flight, so the reason it stopped has no
    // reader left to be shown to; the log takes it.  Nothing above this can be handed a
    // failure, so here is where it is let go
    if (st->job.run != NULL) {
      struct ov_error derr = {0};

      if (!repair_run_finish(&st->job.run, &derr)) {
        OV_ERROR_REPORT(&derr, NULL);
      }
    }
    release_job(st);
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief Bring the window of a class to the front
 *
 * The window is looked for by its class, in the whole desktop: the window at the screen is
 * made by another process, and this is exactly the one a second start has to reach.
 *
 * @param class_name the class the window to reach carries
 * @param err receives the failure of the search
 * @return false when no window of the class was found
 */
static NODISCARD bool activate_window_of_class(wchar_t const *const class_name, struct ov_error *const err) {
  bool success = false;
  HWND hwnd = NULL;

  hwnd = FindWindowW(class_name, NULL);
  if (hwnd == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  if (IsIconic(hwnd)) {
    if (!ShowWindowAsync(hwnd, SW_RESTORE)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  if (!BringWindowToTop(hwnd)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // SetForegroundWindow is allowed to fail: Windows keeps a program from taking the
  // foreground away from the one the person is working in.  The window is brought to the
  // front as far as this program may, and failing the rest is not worth a report.
  SetForegroundWindow(hwnd);
  success = true;

cleanup:
  return success;
}

/**
 * @brief Bring the window of the copy that is already running to the front
 *
 * @param err receives the failure of the search
 * @return false when no window of this program was found
 */
NODISCARD bool ui_activate_running_window(struct ov_error *const err) { return activate_window_of_class(main_window_class, err); }

/**
 * @brief Take the window down and give back what it held
 *
 * What it held is given back whether or not the window could be taken down.
 *
 * @param st the state of the window, set to NULL afterwards, may be NULL
 * @param err receives the failure of the teardown, NULL is allowed
 * @return false when the window or its class could not be taken down
 */
static NODISCARD bool ui_main_window_destroy(struct ui_state **const st, struct ov_error *const err) {
  bool success = true;

  if (st == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (*st != NULL) {
    if (((*st)->window != NULL) && IsWindow((*st)->window) && !DestroyWindow((*st)->window)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      success = false;
    }
    (*st)->window = NULL;
    if ((*st)->atom != 0) {
      HMODULE const module = GetModuleHandleW(NULL);

      if (module == NULL) {
        if (success) {
          OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        } else {
          OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
        }
        success = false;
      } else if (!UnregisterClassW(main_window_class, module)) {
        if (success) {
          OV_ERROR_SETF(err,
                        ov_error_type_hresult,
                        HRESULT_FROM_WIN32(GetLastError()),
                        "%1$ls",
                        gettext("UnregisterClassW failed for %1$ls"),
                        main_window_class);
        } else {
          OV_ERROR_PUSHF(err,
                         ov_error_type_hresult,
                         HRESULT_FROM_WIN32(GetLastError()),
                         gettext("UnregisterClassW failed for %1$ls"),
                         gettext("UnregisterClassW failed for %1$ls"),
                         main_window_class);
        }
        success = false;
      }
      (*st)->atom = 0;
    }
    if (((*st)->font != NULL) && !DeleteObject((*st)->font)) {
      if (success) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      } else {
        OV_ERROR_PUSH(err, ov_error_type_generic, ov_error_generic_fail, "the font of the window could not be deleted");
      }
      success = false;
    }
    (*st)->font = NULL;
    OV_FREE(st);
  }
  if (!theme_release(err)) {
    OV_ERROR_ADD_TRACE(err);
    success = false;
  }
  return success;
}

/**
 * @brief Make the window of the program, show it and give it back
 *
 * The window is the one a run reports into.  The caller runs the message loop of the window
 * and gives the state back to ui_main_window_destroy; ui_run is both of them together, and a
 * caller that needs the window while it is up -- a test that drives it, say -- takes the
 * steps apart the same way.
 *
 * @param options what this run was asked to do
 * @param settings the settings this run works for
 * @param err receives the failure of the start-up
 * @return the state of the window, NULL when the window could not be made; hand it back
 *         to ui_main_window_destroy
 */
static NODISCARD struct ui_state *
ui_main_window_create(struct options const *const options, struct settings const *const settings, struct ov_error *const err) {
  struct ui_state *st = NULL;
  WNDCLASSEXW wc;
  INITCOMMONCONTROLSEX icc;
  bool success = false;

  if ((options == NULL) || (settings == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!OV_REALLOC(&st, 1, sizeof(*st))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  *st = (struct ui_state){0};
  st->settings = settings;
  st->options = options;

  if (!theme_init(settings->use_theme, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  memset(&icc, 0, sizeof(icc));
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
  if (!InitCommonControlsEx(&icc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(err);
  if (wc.hbrBackground == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  wc.lpszClassName = main_window_class;
  wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  st->atom = RegisterClassExW(&wc);
  if (st->atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  {
    RECT outer;

    /* no dpi and no captions before the window exists: WM_CREATE corrects the size */
    layout_window_rect(layout_scale(ADC_MAIN_WINDOW_W, GetDpiForSystem()),
                       layout_scale(ADC_MAIN_WINDOW_H, GetDpiForSystem()),
                       GetDpiForSystem(),
                       main_window_style,
                       0,
                       &outer);
    st->window = CreateWindowExW(0,
                                 wc.lpszClassName,
                                 L"",
                                 main_window_style,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 outer.right - outer.left,
                                 outer.bottom - outer.top,
                                 NULL,
                                 NULL,
                                 wc.hInstance,
                                 st);
    if (st->window == NULL) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    // ShowWindow answers the state the window was in and does not set the last error, so
    // there is nothing to read from it; a window that cannot be shown is still a window
    ShowWindow(st->window, SW_SHOW);
    if (!UpdateWindow(st->window)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    success = true;
  }

cleanup:
  if (!success) {
    if (!ui_main_window_destroy(&st, err)) {
      OV_ERROR_ADD_TRACE(err);
    }
  }
  return success ? st : NULL;
}

/**
 * @brief Make the window of the program, run it and take it down again
 *
 * @param options what this run was asked to do
 * @param settings the settings this run works for
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
NODISCARD bool ui_run(struct options const *const options, struct settings const *const settings, struct ov_error *const err) {
  struct ui_state *st = NULL;
  bool success = false;

  st = ui_main_window_create(options, settings, err);
  if (st == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    MSG msg;
    BOOL answer = 1;

    while ((answer = GetMessageW(&msg, NULL, 0, 0)) > 0) {
      if (!IsDialogMessageW(st->window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (answer == -1) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (!ui_main_window_destroy(&st, err)) {
    OV_ERROR_ADD_TRACE(err);
    success = false;
  }
  return success;
}
