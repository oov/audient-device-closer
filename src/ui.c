#include "ui.h"

#include "task.h"
#include "theme.h"
#include "ui_blockers_window.h"
#include "ui_progress_window.h"

#include <windows.h>

#include <commctrl.h>

#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovthreads.h>

#include "culprit.h"
#include "device.h"
#include "layout.h"
#include "logger.h"
#include "options.h"
#include "repair.h"
#include "settings.h"

#define IDC_CLOSE_BTN 2
#define IDC_CHK_MIXER 5
#define IDC_CHK_AUDIODG 10
#define IDC_GROUP_SETTINGS 11
#define IDC_GROUP_INSTALL 7
#define IDC_TASK_INSTALL 8
#define IDC_TASK_REMOVE 9

#define WM_ADC_STAGE (WM_APP + 1)
#define WM_ADC_DONE (WM_APP + 2)

struct repair_job {
  HWND hwnd;
  struct settings const *settings;
  bool allow_mixer_close;
  bool allow_audiodg_close;
  bool wait_first; // the run of the scheduler: it waits for the devices before it looks
  bool cancel;     // the run quits between its steps when this is up; written by the UI thread only
  bool ok;
  bool leave; // nothing to fix: the run leaves the window behind
  struct repair_result result;
  struct ov_error err;
};

/**
 * @brief The style of the window of the application
 *
 * Without a thick frame: the window keeps the size of its layout.
 */
static DWORD const main_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

struct ui_state {
  struct settings const *settings;
  struct options const *options;
  HFONT font;
  HWND close_btn;
  HWND chk_mixer;
  HWND chk_audiodg;
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
  bool faulted;
  enum device_state state;
  bool close_when_done;
  HWND progress;
  struct repair_job job;
};

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
                  int const id) {
  HWND const hwnd = CreateWindowExW(
      0, cls, text, WS_CHILD | WS_VISIBLE | style, px(st, x), px(st, y), px(st, w), px(st, h), parent, (HMENU)(INT_PTR)id, NULL, NULL);
  if (hwnd != NULL) {
    theme_attach(hwnd);
  }
  return hwnd;
}

static wchar_t const ph[] = L"%1$s";

/**
 * @brief Switch the controls a press can act on
 *
 * @param st the window that carries the controls
 * @param enabled FALSE while the run owns the screen
 */
static void enable_actions(struct ui_state *const st, bool const enabled) {
  EnableWindow(st->close_btn, enabled);
  EnableWindow(st->chk_mixer, enabled);
  EnableWindow(st->chk_audiodg, enabled);
  EnableWindow(st->task_install, enabled);
  EnableWindow(st->task_remove, enabled);
}

/**
 * @brief The line the window shows when a stage has no text of its own
 *
 * @return the translated text, UTF-8
 */
static char const *default_stage_text(void) { return gettext("Closing the device..."); }

/**
 * @brief The text of one stage of the run
 *
 * gettext() has to see the literal at the call site, that is what src/i18n/start.bash
 * collects, so the mapping of the stages to the texts lives here and not with the repair.
 *
 * @param stage the stage to name
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the translated text, UTF-8, always terminated
 * @param out_size room of out
 */
static void stage_text(enum repair_stage const stage, struct settings const *const s, char *const out, size_t const out_size) {
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  out[0] = '\0';
  switch (stage) {
  case REPAIR_STAGE_WAIT:
    strncpy(out, gettext("Waiting..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_PROBE:
    strncpy(out, gettext("Checking the device state..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_VETO_RECORD:
    strncpy(out, gettext("Checking what is blocking the close..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_MIXER: {
    char const *const display = (s != NULL && s->mixer_display_name != NULL) ? s->mixer_display_name : "iD Mixer";
    char const *const exe = (s != NULL && s->mixer_process_name != NULL) ? s->mixer_process_name : "iD.exe";
    OV_SNPRINTF(out, out_size, "%1$s%2$s", gettext("Closing %1$s (%2$s)..."), display, exe);
    return;
  }
  case REPAIR_STAGE_AUDIODG:
    strncpy(out, gettext("Closing audiodg (audiodg.exe)..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_REMOVE:
    break;
  }
  strncpy(out, default_stage_text(), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief Show one answer of the program in a task dialog
 *
 * The outcome of a repair goes into a task dialog, which is what the UI description asks
 * for when the tool stays open.  The two buttons of the install group report through the
 * same dialog, so a reader sees one kind of answer for one window.
 *
 * @param owner the window that is asking
 * @param title the headline of the dialog, UTF-16
 * @param text the body of the dialog, UTF-16
 * @param error TRUE shows the answer as a warning
 */
static void report_dialog(HWND const owner, wchar_t const *const title, wchar_t const *const text, BOOL const error) {
  TASKDIALOGCONFIG cfg;
  wchar_t app[64];

  memset(&cfg, 0, sizeof(cfg));
  OV_SNPRINTF(app, sizeof(app) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));
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
 * @brief Report what a press of an install button did
 *
 * The two functions report what they did -- or that they could not -- with the same dialog
 * the result of a run uses, and the detail of a refusal stays in the log.  Registering and
 * removing are idempotent, so pressing the button twice is not an error.
 *
 * @param owner the window that is asking
 * @param ok what the step answered
 * @param done_id the message identifier of the answer when it worked
 * @param failed_id the message identifier of the answer when it did not
 */
static void report_task_step(HWND const owner, bool const ok, char const *const done_id, char const *const failed_id) {
  wchar_t title[128];
  wchar_t text[512];

  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));
  OV_SNPRINTF(text, sizeof(text) / sizeof(WCHAR), ph, ph, ok ? done_id : failed_id);
  report_dialog(owner, title, text, ok ? FALSE : TRUE);
}

/**
 * @brief Register the scheduled task when the button says so
 *
 * @param st the window that holds the settings
 * @param hwnd the window that is asking
 */
static void install_task(struct ui_state *const st, HWND const hwnd) {
  struct ov_error err = {0};
  bool const close_mixer = (SendMessageW(st->chk_mixer, BM_GETCHECK, 0, 0) == BST_CHECKED);
  bool const close_audiodg = (SendMessageW(st->chk_audiodg, BM_GETCHECK, 0, 0) == BST_CHECKED);
  bool const ok = task_install(st->settings, close_mixer, close_audiodg, &err);

  OV_ERROR_REPORT(&err, NULL);
  report_task_step(hwnd, ok, gettext("The scheduled task was registered."), gettext("The scheduled task could not be registered."));
}

/**
 * @brief Remove the scheduled task when the button says so
 *
 * @param st the window that holds the settings
 * @param hwnd the window that is asking
 */
static void remove_task(struct ui_state *const st, HWND const hwnd) {
  struct ov_error err = {0};
  bool const ok = task_uninstall(st->settings, &err);

  OV_ERROR_REPORT(&err, NULL);
  report_task_step(hwnd, ok, gettext("The scheduled task was removed."), gettext("The scheduled task could not be removed."));
}

/**
 * @brief Ask before the close when the device does not need it
 *
 * A healthy device does not need the close at all and the close of one that works is
 * likely to be refused, so that press is worth a question; the faulty one is the case this
 * tool exists for and starts at once.  The answer comes back as yes or no, and no leaves
 * the window as it was.
 *
 * @param owner the window that is asking
 * @return false when the person said no
 */
static bool confirm_close(HWND const owner) {
  TASKDIALOGCONFIG cfg;
  wchar_t app[64];
  wchar_t wheadline[256];
  wchar_t wbody[512];
  char headline[256];
  char body[512];
  int button = 0;

  ui_confirm_dialog_text(headline, sizeof(headline), body, sizeof(body));
  if ((headline[0] == '\0') || (body[0] == '\0')) {
    return true;
  }
  if (OV_SNPRINTF(wheadline, sizeof(wheadline) / sizeof(WCHAR), ph, ph, headline) < 0) {
    return true;
  }
  if (OV_SNPRINTF(wbody, sizeof(wbody) / sizeof(WCHAR), ph, ph, body) < 0) {
    return true;
  }
  memset(&cfg, 0, sizeof(cfg));
  OV_SNPRINTF(app, sizeof(app) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));
  cfg.cbSize = sizeof(cfg);
  cfg.hwndParent = owner;
  cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  cfg.pszWindowTitle = app;
  cfg.pszMainInstruction = wheadline;
  cfg.pszContent = wbody;
  cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
  cfg.nDefaultButton = IDNO; // a press of Enter follows the answer the reader likely meant
  cfg.pszMainIcon = TD_WARNING_ICON;
  if (TaskDialogIndirect(&cfg, &button, NULL, NULL) != S_OK) {
    return MessageBoxW(owner, wbody, wheadline, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
  }
  return button == IDYES;
}

enum { ADC_FONT_CHILD_COUNT = 7 };

/**
 * @brief Every control that carries the font of the window
 *
 * The buffer of the caller is sized from ADC_FONT_CHILD_COUNT: a ninth control makes the
 * compiler complain about the extra initializer instead of silently dropping the last
 * entry at run time, which is how the remove button of the install group lost its font.
 *
 * @param st the window that holds the controls
 * @param out receives the controls
 * @param max room of out
 * @return how many controls were written
 */
static size_t ui_children(struct ui_state *const st, HWND *const out, size_t const max) {
  HWND const kids[ADC_FONT_CHILD_COUNT] = {
      st->close_btn, st->chk_mixer, st->chk_audiodg, st->group_settings, st->group_install, st->task_install, st->task_remove};
  size_t n = 0;
  size_t i = 0;

  for (i = 0; i < (sizeof(kids) / sizeof(kids[0])); i++) {
    if (kids[i] != NULL && n < max) {
      out[n++] = kids[i];
    }
  }
  return n;
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
  OV_SNPRINTF(wide, sizeof(wide) / sizeof(WCHAR), ph, ph, text);
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
    REPAIR_STAGE_AUDIODG,
    REPAIR_STAGE_VETO_RECORD,
};

/**
 * @brief The stage text that needs the most room
 *
 * The window shows the step of the run behind the stage, and the step is the same for every
 * stage at the widest line: the text that is widest on its own is widest with the step too.
 *
 * @param st the window the run reports into, which knows the font to measure with
 * @param hwnd the window to measure on
 * @return the text, UTF-8
 */
static char const *widest_stage_text(struct ui_state const *const st, HWND const hwnd) {
  char const *widest = default_stage_text();
  HDC dc = GetDC(hwnd);
  HFONT old = NULL;
  int room = 0;
  size_t i = 0;

  if (dc == NULL) {
    return widest;
  }
  if (st->font != NULL) {
    old = (HFONT)SelectObject(dc, st->font);
  }
  room = text_width(dc, widest);
  for (i = 0; i < (sizeof(report_stages) / sizeof(report_stages[0])); i++) {
    char text_buf[256];
    stage_text(report_stages[i], st->settings, text_buf, sizeof(text_buf));
    char const *const text = text_buf;
    int const width = text_width(dc, text);

    if (width > room) {
      room = width;
      widest = text;
    }
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(hwnd, dc);
  return widest;
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
 * @return the width in pixels, 0 when the captions could not be read
 */
static int settings_group_width(struct ui_state *const st, HWND const hwnd) {
  char mixer_label[256];
  char const *const captions[] = {
      mixer_label,
      repair_option_audiodg_label(),
  };
  repair_option_mixer_label(st->settings, mixer_label, sizeof(mixer_label));
  int widest = 0;
  HDC dc = NULL;
  HFONT old = NULL;
  size_t i = 0;

  dc = GetDC(hwnd);
  if (dc == NULL) {
    return layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, (UINT)st->dpi);
  }
  if (st->font != NULL) {
    old = (HFONT)SelectObject(dc, st->font);
  }
  for (i = 0; i < (sizeof(captions) / sizeof(captions[0])); i++) {
    int const width = text_width(dc, captions[i]);

    if (width > widest) {
      widest = width;
    }
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(hwnd, dc);
  widest += layout_scale(ADC_LAYOUT_CHECK_BOX_W + ADC_LAYOUT_CHECK_BOX_GAP + 2 * ADC_LAYOUT_GROUP_INSET, (UINT)st->dpi);
  if (widest < layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, (UINT)st->dpi)) {
    widest = layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, (UINT)st->dpi);
  }
  return widest;
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
 * @return the width in pixels, 0 when the caption could not be read
 */
static int caption_width(struct ui_state *const st, HWND const hwnd, HWND const caption) {
  HDC dc = NULL;
  HFONT old = NULL;
  wchar_t text[64];
  SIZE size = {0, 0};
  int width = 0;

  if (caption == NULL) {
    return px(st, ADC_LAYOUT_CAPTION_PAD);
  }
  memset(text, 0, sizeof(text));
  GetWindowTextW(caption, text, (int)(sizeof(text) / sizeof(text[0])));
  dc = GetDC(hwnd);
  if (dc == NULL) {
    width = px(st, ADC_LAYOUT_CAPTION_PAD);
    goto cleanup;
  }
  old = (HFONT)SelectObject(dc, st->font);
  GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
  width = size.cx + px(st, ADC_LAYOUT_CAPTION_PAD);
  if (width <= 0) {
    width = px(st, ADC_LAYOUT_CAPTION_PAD);
  }

cleanup:
  if (dc != NULL) {
    if (old != NULL) {
      SelectObject(dc, old);
    }
    ReleaseDC(hwnd, dc);
    dc = NULL;
  }
  return width;
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
 * @return the height in pixels, 0 when the font could not be asked
 */
static int caption_height(struct ui_state *const st, HWND const hwnd) {
  HDC dc = NULL;
  HFONT old = NULL;
  TEXTMETRICW tm;
  int height = 0;

  memset(&tm, 0, sizeof(tm));
  dc = GetDC(hwnd);
  if (dc == NULL) {
    goto cleanup;
  }
  old = (HFONT)SelectObject(dc, st->font);
  if (!GetTextMetricsW(dc, &tm)) {
    goto cleanup;
  }
  height = tm.tmHeight;

cleanup:
  if (dc != NULL) {
    if (old != NULL) {
      SelectObject(dc, old);
    }
    ReleaseDC(hwnd, dc);
    dc = NULL;
  }
  return height;
}

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

/**
 * @brief Place every control of the window
 *
 * The geometry itself lives in layout_compute(), this only hands the rectangles to the
 * window manager.  Keeping the arithmetic out of this file is what makes it testable.
 *
 * @param st the window that holds the controls
 * @param hwnd the window to place them in
 * @param client the client rectangle of the window
 * @param settings_min_w the width the settings group needs, measured for the font of the window
 */
static void layout(struct ui_state *const st, HWND const hwnd, RECT const *const client, int const settings_min_w) {
  struct layout g;
  int const height = caption_height(st, hwnd);

  layout_compute(client,
                 (UINT)st->dpi,
                 settings_min_w,
                 caption_width(st, hwnd, st->group_settings),
                 caption_width(st, hwnd, st->group_install),
                 height,
                 &g);

  MoveWindow(st->close_btn, LX(g.button), LY(g.button), LW(g.button), LH(g.button), TRUE);
  MoveWindow(st->chk_mixer, LX(g.check_mixer), LY(g.check_mixer), LW(g.check_mixer), LH(g.check_mixer), TRUE);
  MoveWindow(st->chk_audiodg, LX(g.check_audiodg), LY(g.check_audiodg), LW(g.check_audiodg), LH(g.check_audiodg), TRUE);
  if (st->group_settings != NULL) {
    st->settings_frame_rc = g.settings_frame;
    st->settings_caption_rc = g.settings_caption;
    if (st->settings->use_theme) {
      MoveWindow(st->group_settings, LX(g.settings_caption), LY(g.settings_caption), LW(g.settings_caption), LH(g.settings_caption), TRUE);
    } else {
      /* the box of the system draws its caption from the top of the control and
         its top line at half the height of the caption, so the control spans the
         caption and the frame: both modes put the line through the middle of the
         same text */
      MoveWindow(st->group_settings,
                 LX(g.settings_frame),
                 LY(g.settings_caption),
                 LW(g.settings_frame),
                 g.settings_frame.bottom - g.settings_caption.top,
                 TRUE);
    }
  }
  if (st->group_install != NULL) {
    st->install_frame_rc = g.install_frame;
    st->install_caption_rc = g.install_caption;
    if (st->settings->use_theme) {
      MoveWindow(st->group_install, LX(g.install_caption), LY(g.install_caption), LW(g.install_caption), LH(g.install_caption), TRUE);
    } else {
      MoveWindow(st->group_install,
                 LX(g.install_frame),
                 LY(g.install_caption),
                 LW(g.install_frame),
                 g.install_frame.bottom - g.install_caption.top,
                 TRUE);
    }
    MoveWindow(st->task_install, LX(g.task_install), LY(g.task_install), LW(g.task_install), LH(g.task_install), TRUE);
    MoveWindow(st->task_remove, LX(g.task_remove), LY(g.task_remove), LW(g.task_remove), LH(g.task_remove), TRUE);
    InvalidateRect(hwnd, NULL, TRUE);
  }
}

/**
 * @brief Put the window at the size its layout asks for
 *
 * The only place that changes the size of the window.
 *
 * @param st             the window state, which knows the dpi the size is worked out for
 * @param hwnd           the window to put at that size
 * @param settings_min_w the width the settings group needs, in pixels
 */
static void apply_window_size(struct ui_state const *const st, HWND const hwnd, int const settings_min_w) {
  RECT outer;
  RECT now;

  layout_window_rect(layout_min_client_width(settings_min_w, (UINT)st->dpi),
                     layout_scale(ADC_LAYOUT_WINDOW_H, (UINT)st->dpi),
                     (UINT)st->dpi,
                     main_window_style,
                     0,
                     &outer);
  GetWindowRect(hwnd, &now);
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return;
  }
  SetWindowPos(hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
 */
static void apply_dpi(struct ui_state *const st, HWND const hwnd, int const dpi) {
  HFONT stale = NULL;
  HWND kids[ADC_FONT_CHILD_COUNT];
  size_t n = 0;
  size_t i = 0;
  RECT rc;
  int settings_w = 0;

  if (hwnd == NULL || dpi <= 0) {
    return;
  }
  stale = st->font;
  st->font = layout_make_font((UINT)dpi);
  if (st->font == NULL) {
    st->font = stale;
    stale = NULL;
    goto cleanup;
  }
  st->dpi = dpi;
  n = ui_children(st, kids, (sizeof(kids) / sizeof(kids[0])));
  for (i = 0; i < n; i++) {
    SendMessageW(kids[i], WM_SETFONT, (WPARAM)st->font, TRUE);
  }
  settings_w = settings_group_width(st, hwnd);
  apply_window_size(st, hwnd, settings_w);
  GetClientRect(hwnd, &rc);
  layout(st, hwnd, &rc, settings_w);
  InvalidateRect(hwnd, NULL, TRUE);

cleanup:
  if (stale != NULL) {
    DeleteObject(stale);
    stale = NULL;
  }
}

/**
 * @brief Look at the device and set what the next press answers from
 *
 * Nothing on the screen shows the state; the button is what reads it.
 *
 * @param st the window that carries the state
 * @return the state the device is in now
 */
static enum device_state refresh(struct ui_state *const st) {
  enum device_state state = DEVICE_STATE_UNKNOWN;
  struct ov_error err = {0};

  if (st->busy) {
    return DEVICE_STATE_UNKNOWN; // the run owns the screen while it acts by itself
  }
  state = device_probe(st->settings->ks_service, st->settings->ks_instance_prefix, &err);
  st->state = state;
  st->faulted = (state == DEVICE_STATE_FAULTED);
  switch (state) {
  case DEVICE_STATE_FAULTED:
  case DEVICE_STATE_HEALTHY:
  case DEVICE_STATE_PROBLEM:
  case DEVICE_STATE_ABSENT:
  case DEVICE_STATE_UNKNOWN:
    break;
  }
  EnableWindow(st->close_btn, TRUE);
  if ((state != DEVICE_STATE_FAULTED) && (state != DEVICE_STATE_HEALTHY)) {
    OV_ERROR_REPORT(&err, NULL);
  }
  return state;
}

/**
 * @brief Show the step the run is at
 *
 * @param job the run that is reporting
 * @param stage what the run is waiting for
 * @param step the step of the run
 */
static void report_step(struct repair_job *const job, enum repair_stage const stage, size_t const step) {
  PostMessageW(job->hwnd, WM_ADC_STAGE, (WPARAM)stage, (LPARAM)step);
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

  (void)total;
  report_step(job, stage, step + (job->wait_first ? 1 : 0));
}

/**
 * @brief The work of one button press, on a thread of its own
 *
 * It lives inside the ui state and the window refuses to close while the thread is still
 * writing to it.
 *
 * @param userdata the run to carry out
 * @return 0 when the run is over
 */
static int repair_thread(void *const userdata) {
  struct repair_job *const job = (struct repair_job *)(void *)userdata;
  struct ov_error probe_err = {0};
  struct ov_error log_err = {0};
  enum device_state state = DEVICE_STATE_UNKNOWN;

  if (!logger_log_run_start(&log_err)) {
    OV_ERROR_REPORT(&log_err, NULL);
  }
  if (job->wait_first) {
    report_step(job, REPAIR_STAGE_WAIT, 1);
    if (job->settings) {
      Sleep((DWORD)job->settings->auto_run_delay_ms);
    } else {
      Sleep(5000);
    }
  }
  state = device_probe(job->settings->ks_service, job->settings->ks_instance_prefix, &probe_err);
  OV_ERROR_REPORT(&probe_err, NULL);
  if (ui_auto_run_action(job->wait_first, state) == UI_AUTO_RUN_LEAVE) {
    job->leave = true;
    if (!logger_log_run_end(ui_auto_run_sentence(state), &log_err)) {
      OV_ERROR_REPORT(&log_err, NULL);
    }
  } else {
    job->ok = repair_run(
        job->settings, job->allow_mixer_close, job->allow_audiodg_close, repair_progress, job, &job->cancel, &job->result, &job->err);
    OV_ERROR_REPORT(&job->err, NULL);
    if (!logger_log_run_end(job->result.message, &log_err)) {
      OV_ERROR_REPORT(&log_err, NULL);
    }
  }
  PostMessageW(job->hwnd, WM_ADC_DONE, 0, 0);
  return 0;
}

/**
 * @brief The headline of the result dialog: the verdict of the run
 *
 * @param outcome what came of the run
 * @return the translated sentence, UTF-8
 */
char const *ui_result_headline(enum repair_outcome const outcome) {
  if (outcome == REPAIR_OUTCOME_CLOSED) {
    return gettext("Success.");
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
bool ui_closes_itself_when_done(bool const auto_run, enum repair_outcome const outcome) {
  return auto_run && (outcome == REPAIR_OUTCOME_CLOSED);
}

/**
 * @brief Does the run end in a dialog
 *
 * @param auto_run the scheduler started this run
 * @param outcome what came of the run
 * @return true when the window stays open and the outcome is worth a dialog
 */
bool ui_shows_result_dialog(bool const auto_run, enum repair_outcome const outcome) {
  return !auto_run || (outcome != REPAIR_OUTCOME_CLOSED);
}

/**
 * @brief What a run nobody is watching does with the state it finds
 *
 * @param auto_run the scheduler started this run
 * @param state the state of the device
 * @return what the run should do next
 */
enum ui_auto_run ui_auto_run_action(bool const auto_run, enum device_state const state) {
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
char const *ui_auto_run_sentence(enum device_state const state) {
  switch (state) {
  case DEVICE_STATE_HEALTHY:
    return "The device is working.";
  case DEVICE_STATE_ABSENT:
    return "No Audient device was found.";
  case DEVICE_STATE_PROBLEM:
    return "The device has a problem that this tool cannot solve.";
  case DEVICE_STATE_UNKNOWN:
    return "The device state could not be determined.";
  case DEVICE_STATE_FAULTED:
    break;
  }
  return NULL;
}

/**
 * @brief Does a press of the button have to ask first
 *
 * @param faulted the device is in the state this tool is for
 * @return true when the person should be asked first
 */
bool ui_asks_before_close(bool const faulted) { return !faulted; }

/**
 * @brief Does a press of the button find a device the tool cannot handle
 *
 * @param state the state the last refresh saw
 * @return true when the press is answered with a dialog instead of a run
 */
bool ui_state_is_not_repairable(enum device_state const state) {
  return (state == DEVICE_STATE_PROBLEM) || (state == DEVICE_STATE_ABSENT) || (state == DEVICE_STATE_UNKNOWN);
}

/**
 * @brief The two lines of the question in front of a close the device does not need
 *
 * What the reader is asked is whether to go on, so the headline carries the question and
 * the body carries what the answer is about: what was found, and what follows from it.
 * The detail stays in the log.
 *
 * @param headline receives the question, UTF-8
 * @param headline_size room of headline
 * @param body receives what the answer is about, UTF-8
 * @param body_size room of body
 */
void ui_confirm_dialog_text(char *const headline, size_t const headline_size, char *const body, size_t const body_size) {
  if ((headline == NULL) || (headline_size == 0) || (body == NULL) || (body_size == 0)) {
    return;
  }
  strncpy(headline, gettext("Do you really want to continue?"), headline_size - 1);
  headline[headline_size - 1] = '\0';
  strncpy(body,
          gettext("No faulty Audient device was found.\n\n"
                  "A device that is working normally is likely to fail to be closed."),
          body_size - 1);
  body[body_size - 1] = '\0';
}

/**
 * @brief The two lines of the result dialog
 *
 * The headline carries the verdict, the body the reason the run worked out.  Both are
 * sentences of the translation; what a run leaves in repair_result.message goes to the log.
 *
 * @param outcome what came of the run
 * @param reason why the removal was refused
 * @param headline receives the verdict, UTF-8
 * @param headline_size room of headline
 * @param body receives what happened, UTF-8
 * @param body_size room of body
 */
void ui_result_dialog_text(enum repair_outcome const outcome,
                           enum removal_reason const reason,
                           char *const headline,
                           size_t const headline_size,
                           char *const body,
                           size_t const body_size) {
  char const *text = gettext("The device could not be closed."); // the switches below name the answer

  if ((headline == NULL) || (headline_size == 0) || (body == NULL) || (body_size == 0)) {
    return;
  }
  if (outcome == REPAIR_OUTCOME_FAILED) {
    switch (reason) {
    case REMOVAL_REASON_ACCESS_DENIED:
      text = gettext("This program is not allowed to close the device.\n\n"
                     "Run it as an administrator, then close the device again.");
      break;
    case REMOVAL_REASON_DEVICE_GONE:
      text = gettext("The device was already gone.");
      break;
    case REMOVAL_REASON_CALL_FAILED:
    case REMOVAL_REASON_FAILED:
    case REMOVAL_REASON_NONE:
      break;
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
  strncpy(headline, ui_result_headline(outcome), headline_size - 1);
  headline[headline_size - 1] = '\0';
}
/**
 * @brief Show the result of the run in a dialog
 *
 * @param hwnd the window that is asking
 * @param outcome what came of the run
 * @param reason why the removal was refused
 */
static void show_result_dialog(HWND const hwnd, enum repair_outcome const outcome, enum removal_reason const reason) {
  wchar_t wheadline[256];
  wchar_t wbody[1600];
  char headline[256];
  char body[1600];

  ui_result_dialog_text(outcome, reason, headline, sizeof(headline), body, sizeof(body));
  if (headline[0] == '\0') {
    return;
  }
  if (OV_SNPRINTF(wheadline, sizeof(wheadline) / sizeof(WCHAR), ph, ph, headline) < 0) {
    return;
  }
  if (OV_SNPRINTF(wbody, sizeof(wbody) / sizeof(WCHAR), ph, ph, body) < 0) {
    return;
  }
  report_dialog(hwnd, wheadline, wbody, outcome != REPAIR_OUTCOME_CLOSED);
}

/**
 * @brief Answer a press on a device the tool cannot handle with its dialog
 *
 * @param hwnd the window that is asking
 */
static void report_not_repairable(HWND const hwnd) {
  wchar_t title[128];
  wchar_t text[512];

  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));
  OV_SNPRINTF(text, sizeof(text) / sizeof(WCHAR), ph, ph, gettext("This tool cannot handle the problem that was found."));
  report_dialog(hwnd, title, text, TRUE);
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
  ui_progress_window_destroy(&st->progress);
  if (job->leave) {
    post_close = true;
    goto cleanup;
  }
  enable_actions(st, true); // a run that leaves the window open leaves it usable
  if (!job->ok) {
    show_result_dialog(hwnd, REPAIR_OUTCOME_FAILED, REMOVAL_REASON_NONE);
    goto cleanup;
  }
  post_close = ui_closes_itself_when_done(st->options->auto_run, job->result.outcome);
  if (job->result.outcome == REPAIR_OUTCOME_VETOED) {
    struct ov_error berr = {0};

    if (!ui_blockers_window_show(hwnd, job->result.culprits, &berr)) {
      OV_ERROR_REPORT(&berr, NULL);
      show_result_dialog(hwnd, job->result.outcome, job->result.removal_reason);
    }
    refresh(st);
    goto cleanup;
  }
  if (ui_shows_result_dialog(st->options->auto_run, job->result.outcome)) {
    show_result_dialog(hwnd, job->result.outcome, job->result.removal_reason);
  }

cleanup:
  repair_result_release(&job->result);
  if (post_close || st->close_when_done) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

/**
 * @brief Start the work of one button press
 *
 * @param st the window that carries the job
 * @param hwnd the window the job reports to
 * @param wait_first the run waits for the devices before it looks at them
 */
static void do_repair(struct ui_state *const st, HWND const hwnd, bool const wait_first) {
  thrd_t thread;

  memset(&st->job, 0, sizeof(st->job));
  st->job.hwnd = hwnd;
  st->job.settings = st->settings;
  st->job.wait_first = wait_first;
  st->job.allow_mixer_close = (SendMessageW(st->chk_mixer, BM_GETCHECK, 0, 0) == BST_CHECKED);
  st->job.allow_audiodg_close = (SendMessageW(st->chk_audiodg, BM_GETCHECK, 0, 0) == BST_CHECKED);
  st->busy = true;
  enable_actions(st, false);
  {
    struct ov_error perr = {0};
    char wait_buf[256];
    stage_text(REPAIR_STAGE_WAIT, st->settings, wait_buf, sizeof(wait_buf));
    if (!ui_progress_window_create(hwnd,
                                   st->dpi,
                                   ui_progress_window_steps(st->job.wait_first),
                                   &st->job.cancel,
                                   wait_first ? wait_buf : default_stage_text(),
                                   widest_stage_text(st, hwnd),
                                   &st->progress,
                                   &perr)) {
      OV_ERROR_REPORT(&perr, NULL);
      st->busy = false;
      enable_actions(st, true);
      return;
    }
  }
  UpdateWindow(hwnd);

  if (thrd_create(&thread, repair_thread, &st->job) != thrd_success) {
    repair_thread(&st->job);
    return;
  }
  thrd_detach(thread);
}

/**
 * @brief Paint what no theme draws: the group frames and the background
 *
 * @param hwnd the window to paint
 * @param st the window that holds the layout
 */
static void window_paint(HWND const hwnd, struct ui_state *const st) {
  PAINTSTRUCT ps;
  HDC dc = NULL;

  dc = BeginPaint(hwnd, &ps);
  if (dc == NULL) {
    goto cleanup;
  }
  if (st->settings->use_theme) {
    /* the frames of the groups are painted here only while the theme is on:
       without it, the group boxes of the system draw their own */
    if (st->group_settings != NULL) {
      theme_frame(dc, &st->settings_frame_rc, &st->settings_caption_rc, st->dpi);
    }
    if (st->group_install != NULL) {
      theme_frame(dc, &st->install_frame_rc, &st->install_caption_rc, st->dpi);
    }
  }

cleanup:
  if (dc != NULL) {
    EndPaint(hwnd, &ps);
    dc = NULL;
  }
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
  LONG_PTR const stored = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  struct ui_state *st = (struct ui_state *)(void *)stored;
  CREATESTRUCTW const *cs = NULL;

  switch (msg) {
  case WM_NCCREATE:
    cs = (CREATESTRUCTW const *)lparam;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  default:
    break;
  }
  if (st == NULL) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  {
    LRESULT handled_result = 0;
    bool handled = false;

    if (!theme_message(hwnd, msg, wparam, lparam, &handled_result, &handled, NULL)) {
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (handled) {
      return handled_result;
    }
  }

  switch (msg) {
  case WM_CREATE:
    theme_attach(hwnd);
    {
      UINT const dpi = GetDpiForWindow(hwnd);
      st->dpi = (dpi != 0) ? (int)dpi : USER_DEFAULT_SCREEN_DPI;
    }
    {
      wchar_t button[256];
      wchar_t check_mixer[256];
      wchar_t check_audiodg[256];
      wchar_t group_settings[128];
      wchar_t group_install[128];
      wchar_t task_install[256];
      wchar_t task_remove[256];

      OV_SNPRINTF(button, sizeof(button) / sizeof(WCHAR), ph, ph, gettext("Close the Audient device"));
      {
        char mixer_label[256];
        repair_option_mixer_label(st->settings, mixer_label, sizeof(mixer_label));
        OV_SNPRINTF(check_mixer, sizeof(check_mixer) / sizeof(WCHAR), ph, ph, mixer_label);
      }
      OV_SNPRINTF(check_audiodg, sizeof(check_audiodg) / sizeof(WCHAR), ph, ph, repair_option_audiodg_label());
      OV_SNPRINTF(group_settings, sizeof(group_settings) / sizeof(WCHAR), ph, ph, gettext("Behaviour"));
      OV_SNPRINTF(group_install, sizeof(group_install) / sizeof(WCHAR), ph, ph, gettext("Install"));
      OV_SNPRINTF(task_install, sizeof(task_install) / sizeof(WCHAR), ph, ph, gettext("Register the scheduled task"));
      OV_SNPRINTF(task_remove, sizeof(task_remove) / sizeof(WCHAR), ph, ph, gettext("Remove the scheduled task"));

      if (st->settings->use_theme) {
        /* the caption is a control of its own and the frame is painted by the
           window, so the theme can colour both */
        st->group_settings = child(st, hwnd, L"STATIC", group_settings, SS_LEFT, 0, 0, 0, 0, IDC_GROUP_SETTINGS);
        st->group_install = child(st, hwnd, L"STATIC", group_install, SS_LEFT, 0, 0, 0, 0, IDC_GROUP_INSTALL);
      } else {
        /* without the theme the group is the box of the system: comctl32 draws
           its frame and its caption in the colours of the system, which is what
           a window that asks the system for everything looks like.  The box is
           made before the controls it holds, so it sits under them. */
        st->group_settings = child(st, hwnd, L"BUTTON", group_settings, BS_GROUPBOX, 0, 0, 0, 0, IDC_GROUP_SETTINGS);
        st->group_install = child(st, hwnd, L"BUTTON", group_install, BS_GROUPBOX, 0, 0, 0, 0, IDC_GROUP_INSTALL);
      }
      st->close_btn = child(st, hwnd, L"BUTTON", button, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_CLOSE_BTN);
      st->chk_mixer = child(st, hwnd, L"BUTTON", check_mixer, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, IDC_CHK_MIXER);
      st->chk_audiodg = child(st, hwnd, L"BUTTON", check_audiodg, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, IDC_CHK_AUDIODG);
      st->task_install = child(st, hwnd, L"BUTTON", task_install, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_TASK_INSTALL);
      st->task_remove = child(st, hwnd, L"BUTTON", task_remove, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, IDC_TASK_REMOVE);
      apply_dpi(st, hwnd, st->dpi);
    }
    SendMessageW(st->chk_mixer, BM_SETCHECK, st->options->close_mixer ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->chk_audiodg, BM_SETCHECK, st->options->close_audiodg ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh(st);
    if (st->options->auto_run) {
      do_repair(st, hwnd, true);
    }
    return 0;
  case WM_SIZE: {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if ((rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0) {
      layout(st, hwnd, &rc, settings_group_width(st, hwnd));
    }
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;
    int const dpi = (int)LOWORD(wparam);

    st->dpi = dpi;
    /* the monitor suggests where the window goes; how big it is, the layout says */
    SetWindowPos(hwnd, NULL, rc->left, rc->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    apply_dpi(st, hwnd, dpi);
    return 0;
  }
  case WM_COMMAND:
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_CLOSE_BTN) && !st->busy) {
      if (ui_state_is_not_repairable(st->state)) {
        report_not_repairable(hwnd);
      } else if (!ui_asks_before_close(st->faulted) || confirm_close(hwnd)) {
        do_repair(st, hwnd, false);
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
      stage_text((enum repair_stage)wparam, st->settings, stage_buf, sizeof(stage_buf));
      ui_progress_window_report(st->progress, (size_t)lparam, stage_buf, ui_progress_window_steps(st->job.wait_first));
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
  case WM_PAINT:
    window_paint(hwnd, st);
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief Make the window and run the message loop of the program
 *
 * @param options what this run was asked to do
 * @param settings the settings this run works for
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
bool ui_run(struct options const *const options, struct settings const *const settings, struct ov_error *const err) {
  struct ui_state st;
  WNDCLASSEXW wc;
  INITCOMMONCONTROLSEX icc;
  HWND hwnd = NULL;
  MSG msg;
  ATOM atom = 0;
  wchar_t title[128];
  bool success = false;

  if ((options == NULL) || (settings == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(&st, 0, sizeof(st));
  st.settings = settings;
  st.options = options;

  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));

  if (!theme_init(settings->use_theme, err)) {
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
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"audient_device_closer_main";
  wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  atom = RegisterClassExW(&wc);
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  {
    RECT outer;

    /* no dpi and no captions before the window exists: WM_CREATE corrects the size */
    layout_window_rect(layout_scale(ADC_LAYOUT_WINDOW_W, GetDpiForSystem()),
                       layout_scale(ADC_LAYOUT_WINDOW_H, GetDpiForSystem()),
                       GetDpiForSystem(),
                       main_window_style,
                       0,
                       &outer);
    hwnd = CreateWindowExW(0,
                           wc.lpszClassName,
                           title,
                           main_window_style,
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,
                           outer.right - outer.left,
                           outer.bottom - outer.top,
                           NULL,
                           NULL,
                           wc.hInstance,
                           &st);
  }
  if (hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  theme_attach(hwnd);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  success = true;

cleanup:
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    atom = 0;
  }
  if (st.font != NULL) {
    DeleteObject(st.font);
    st.font = NULL;
  }
  theme_release();
  return success;
}
