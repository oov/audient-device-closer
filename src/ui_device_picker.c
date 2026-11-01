/**
 * The window that asks which device to repair.
 *
 * None of the connected devices is in the state this tool cures, so the
 * press of the button has nothing to do on its own.  The reader may still
 * want the release, so this window names the connected devices and waits
 * for the one to repair.  The window is self-contained -- its geometry,
 * its class and its theme live here -- so a test can make it against any
 * window of its own and read the combo.
 */
#include "ui_device_picker.h"

#include "theme.h"

#include <commctrl.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "device_probe.h"
#include "layout.h"
#include "repair.h"
#include "settings.h"

#define IDC_PICKER_LABEL 16
#define IDC_PICKER_COMBO 17

/** @brief The room of one line of the combo */
enum { ADC_PICKER_TEXT_CHARS = 512 };

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

/** @brief The name the window keeps its state under */
static wchar_t const state_prop[] = L"audient_device_closer_picker_state";

/**
 * @brief The geometry of the window that asks which device to repair, in logical units
 *
 * The window holds the sentence that says why it asks, the combo of the connected devices
 * under it and the two buttons that answer the question.  Its least width is below; the
 * window grows to the widest line the combo holds, so no name is cut off.
 */
enum picker_layout_size {
  ADC_PICKER_W = 360,         /* the least width the sentences read in */
  ADC_PICKER_LABEL_H = 40,    /* the two sentences that ask the question */
  ADC_PICKER_COMBO_H = 26,    /* the combo of the connected devices */
  ADC_PICKER_COMBO_INSET = 8, /* the frame and the padding the combo draws its line away from */
  ADC_PICKER_BUTTON_H = 26,   /* the two buttons that answer it */
  ADC_PICKER_BUTTON_W = 120,
};

/**
 * @brief Every rectangle of the window that asks which device to repair
 *
 * All values are physical pixels in the client coordinates of the window.  The stack is
 * placed from the top edge the way the progress window is.
 */
struct picker_layout {
  RECT label;  /* the sentence that asks the question */
  RECT combo;  /* the connected devices, one line each */
  RECT ok;     /* the button that repairs the chosen device */
  RECT cancel; /* the button that answers nothing */
};

/**
 * @brief Compute the geometry of the window that asks which device to repair
 *
 * The stack is placed from the top edge: the sentence, the combo of the connected devices
 * under it and the two buttons that answer the question side by side under that.
 *
 * @param width  the client width of the window in pixels
 * @param dpi    the dpi of the window
 * @param out    receives the geometry
 */
static void picker_compute(int const width, UINT const dpi, struct picker_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const usable = (width > 2 * margin) ? (width - 2 * margin) : 0;
  int const combo_bottom = layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi) + layout_scale(ADC_PICKER_LABEL_H, dpi) +
                           layout_scale(ADC_LAYOUT_GAP, dpi) + layout_scale(ADC_PICKER_COMBO_H, dpi);
  int const button_w = layout_scale(ADC_PICKER_BUTTON_W, dpi);
  int const button_h = layout_scale(ADC_PICKER_BUTTON_H, dpi);
  int const button_row = combo_bottom + layout_scale(ADC_LAYOUT_GAP, dpi);
  int const right = margin + usable;

  memset(out, 0, sizeof(*out));
  out->label = layout_rect(margin, layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi), usable, layout_scale(ADC_PICKER_LABEL_H, dpi));
  out->combo = layout_rect(margin, out->label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi), usable, layout_scale(ADC_PICKER_COMBO_H, dpi));
  out->ok = layout_rect(right - 2 * button_w - layout_scale(ADC_LAYOUT_GAP, dpi), button_row, button_w, button_h);
  out->cancel = layout_rect(right - button_w, button_row, button_w, button_h);
}

/** @brief The styles of the window: a frame it is not resized with */
static DWORD const picker_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
static DWORD const picker_window_extra = WS_EX_DLGMODALFRAME;

/**
 * @brief The answer the reader gave
 *
 * The window writes it and the loop that made the window reads it once
 * the window is down: the two do not meet, so the window owns nothing
 * of it.
 */
struct picker_result {
  bool answered; // the button that repairs was pressed
  int selected;  // the row of the combo the reader picked, -1 when none
};

/**
 * @brief The window and the controls it is made of
 *
 * The window owns its font and deletes it with the window: the font of
 * the owner is deleted when the owner moves on to a monitor of its own
 * dpi.  The answer lives apart from the window, see picker_result.
 */
struct picker_window {
  HWND hwnd;
  HWND label;
  HWND combo;
  HWND ok_btn;
  HWND cancel_btn;
  int dpi;
  HFONT font;
  struct picker_result *result;        // borrowed
  struct settings const *settings;     // borrowed, names the devices the combo lists
  struct device_target const *targets; // borrowed, the units the combo lists
  struct ov_error *err;                // borrowed, why the window could not be made
  bool failed;                         // the controls could not be made
};

/**
 * @brief The line the combo holds: the name of the device and
 * the unit of it that is connected
 *
 * The driver package lists one name for more than one device, and
 * the same device can be connected more than once, so the line
 * carries the devnode of the unit: the reader sees which device,
 * and which unit of it, is which.
 *
 * @param entry the settings entry the devnode belongs to
 * @param devnode the devnode of the connected unit, may not be NULL
 * @param out receives the line, UTF-8
 * @param out_size room of out
 * @return false when the arguments name nothing
 */
static bool label(struct device_entry const *const entry, wchar_t const *const devnode, wchar_t *const out, size_t const out_size) {
  if ((entry == NULL) || (entry->name == NULL) || (devnode == NULL) || (devnode[0] == L'\0') || (out == NULL) || (out_size == 0)) {
    return false;
  }
  return OV_SNPRINTF(out, out_size, L"%1$ls (%2$ls)", L"%1$ls (%2$ls)", entry->name, devnode) >= 0;
}

/**
 * @brief Bring the picker window onto a font of one dpi
 *
 * @param pw the window to draw with
 * @param dpi the dpi to draw for
 */
static NODISCARD bool apply_font(struct picker_window *const pw, int const dpi, struct ov_error *const err) {
  HFONT stale = pw->font;
  HFONT font = layout_make_font((UINT)dpi);

  pw->dpi = dpi;
  if (font == NULL) {
    // the geometry follows the monitor, the window keeps the font it has
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    return false;
  }
  pw->font = font;
  SendMessageW(pw->label, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->combo, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->ok_btn, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->cancel_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if ((stale != NULL) && !DeleteObject(stale)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

/**
 * @brief The room a text takes in the font of the window
 *
 * @param pw the window that draws the text
 * @param text the text to measure
 * @param len how many characters of the text to measure
 * @param out receives the width in pixels
 * @param err receives the failure of the measurement
 * @return false when the text could not be measured
 */
static NODISCARD bool
text_width(struct picker_window const *const pw, wchar_t const *const text, size_t const len, int *const out, struct ov_error *const err) {
  HDC dc = NULL;
  HFONT old = NULL;
  SIZE size = {0, 0};
  bool success = false;

  *out = 0;
  dc = GetDC(pw->hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (pw->font != NULL) {
    old = (HFONT)SelectObject(dc, pw->font);
    if ((old == NULL) || (old == HGDI_ERROR)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      old = NULL;
      goto cleanup;
    }
  }
  if (!GetTextExtentPoint32W(dc, text, (int)len, &size)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  *out = size.cx;
  success = true;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    if (!ReleaseDC(pw->hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
  }
  return success;
}

/**
 * @brief The room the widest line of a text takes
 *
 * The lines of the text are the parts its line
 * breaks separate, and the window has to be wide
 * enough for the widest one of them.
 *
 * @param pw the window that draws the text
 * @param text the text to measure, its lines separated by line breaks
 * @param err receives the failure of the measurement
 * @param out receives the width of the widest line in pixels
 * @return false when the text could not be measured
 */
static NODISCARD bool
line_width(struct picker_window const *const pw, wchar_t const *const text, int *const out, struct ov_error *const err) {
  wchar_t const *line = text;
  int width = 0;

  *out = 0;
  while (line != NULL) {
    wchar_t const *const end = wcschr(line, L'\n');
    int room = 0;

    if (!text_width(pw, line, ((end != NULL) ? (size_t)(end - line) : wcslen(line)), &room, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    if (room > width) {
      width = room;
    }
    line = (end != NULL) ? (end + 1) : NULL;
  }
  *out = width;
  return true;
}

/**
 * @brief The room the combo keeps beside the line it shows
 *
 * The button that opens the list, the frame
 * around the line and the edge the line is
 * drawn away from take room of the combo,
 * so the window is made wider by their room
 * for the line to be read whole.
 *
 * @param pw the window that draws the combo
 * @return the width in pixels the combo keeps for itself
 */
static int combo_room(struct picker_window const *const pw) {
  COMBOBOXINFO info = {0};
  int button = 0;

  info.cbSize = sizeof(info);
  if (GetComboBoxInfo(pw->combo, &info) && ((info.rcButton.right - info.rcButton.left) > 0)) {
    button = info.rcButton.right - info.rcButton.left;
  } else {
    // the query can fail before the window is shown, and the
    // button of a combo is the width a scroll bar is
    button = GetSystemMetrics(SM_CXVSCROLL);
  }
  return button + (2 * GetSystemMetrics(SM_CXEDGE)) + layout_scale(ADC_PICKER_COMBO_INSET, (UINT)pw->dpi);
}

/**
 * @brief Make the window wide enough for the question it opens with
 *
 * The reader is not asked to resize the window, but the question and
 * the longest line of the combo have to be read whole from the first
 * moment, in every language and at every dpi.
 *
 * @param pw the window to widen
 * @param label_text the text of the sentences
 * @param widest the widest line the combo holds, NULL for the sentence
 * @param err receives the failure of the measurement or of the resize
 * @return false when the window could not be widened
 */
static NODISCARD bool
fit(struct picker_window *const pw, wchar_t const *const label_text, wchar_t const *const widest, struct ov_error *const err) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)pw->dpi);
  int const least = layout_scale(ADC_PICKER_W, (UINT)pw->dpi);
  int width = 0;
  int combo_width = 0;
  int needed = 0;
  int client = 0;
  RECT outer;
  RECT now;

  if (!line_width(pw, label_text, &width, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  if (widest != NULL) {
    int room = 0;

    if (!text_width(pw, widest, wcslen(widest), &room, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    combo_width = room + combo_room(pw);
  }
  if (combo_width > width) {
    width = combo_width;
  }
  needed = width + 2 * margin;
  client = (needed > least) ? needed : least;
  layout_window_rect(client,
                     layout_scale(ADC_LAYOUT_MARGIN_TOP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_LABEL_H, (UINT)pw->dpi) +
                         layout_scale(ADC_LAYOUT_GAP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_COMBO_H, (UINT)pw->dpi) +
                         layout_scale(ADC_LAYOUT_GAP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_BUTTON_H, (UINT)pw->dpi) +
                         layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, (UINT)pw->dpi),
                     (UINT)pw->dpi,
                     picker_window_style,
                     picker_window_extra,
                     &outer);
  if (!GetWindowRect(pw->hwnd, &now)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return true; // already wide enough
  }
  if (!SetWindowPos(pw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  SendMessageW(pw->combo, CB_SETDROPPEDWIDTH, (WPARAM)(client - 2 * margin), 0);
  return true;
}

/**
 * @brief Place the controls of the picker window
 *
 * The geometry itself lives in picker_compute(), this only hands
 * the rectangles to the window manager.
 *
 * @param pw the picker window to place
 * @param err receives the failure of the placement
 * @return false when a control could not be placed
 */
static NODISCARD bool layout(struct picker_window *const pw, struct ov_error *const err) {
  struct picker_layout g;
  RECT rc;

  if (!GetClientRect(pw->hwnd, &rc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  picker_compute(rc.right - rc.left, (UINT)pw->dpi, &g);
  if (!MoveWindow(pw->label, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE) ||
      !MoveWindow(pw->combo, LX(g.combo), LY(g.combo), LW(g.combo), LH(g.combo), TRUE) ||
      !MoveWindow(pw->ok_btn, LX(g.ok), LY(g.ok), LW(g.ok), LH(g.ok), TRUE) ||
      !MoveWindow(pw->cancel_btn, LX(g.cancel), LY(g.cancel), LW(g.cancel), LH(g.cancel), TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief Take the window down with the answer the reader gave
 *
 * @param pw the picker window that answers
 * @param answered the button that repairs was pressed
 * @param err receives the failure of the takedown
 * @return false when the window could not be taken down
 */
static NODISCARD bool answer(struct picker_window *const pw, bool const answered, struct ov_error *const err) {
  if (pw->result != NULL) {
    if (answered) {
      pw->result->selected = (int)SendMessageW(pw->combo, CB_GETCURSEL, 0, 0);
    }
    pw->result->answered = answered;
  }
  if (!DestroyWindow(pw->hwnd)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief Make the controls of the picker window and fill the combo
 *
 * Called from WM_CREATE.  The caller makes the window and shows it; the controls, the
 * font, the theme and the lines of the combo are in place by then.
 *
 * @param pw the window to fill, its hwnd, dpi, settings and targets are already set
 * @param err receives the reason a control could not be made or a device could not be named
 * @return false when the window could not be filled, with the reason in err
 */
static bool build(struct picker_window *const pw, struct ov_error *const err) {
  wchar_t label_text[ADC_PICKER_TEXT_CHARS] = {0};
  wchar_t text[128] = {0};
  wchar_t line[ADC_PICKER_TEXT_CHARS] = {0};
  wchar_t widest[ADC_PICKER_TEXT_CHARS] = {0};
  size_t widest_len = 0;
  bool success = false;

  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Audient Device Closer"));
  if (!SetWindowTextW(pw->hwnd, text)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  OV_SNPRINTF(label_text,
              sizeof(label_text) / sizeof(label_text[0]),
              ph,
              ph,
              gettext("None of the connected devices needs a repair.\nTo repair one anyway, choose it below."));
  pw->label = CreateWindowExW(
      0, L"STATIC", label_text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDC_PICKER_LABEL, NULL, NULL);
  pw->combo = CreateWindowExW(0,
                              WC_COMBOBOXW,
                              L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
                              0,
                              0,
                              0,
                              0,
                              pw->hwnd,
                              (HMENU)(INT_PTR)IDC_PICKER_COMBO,
                              NULL,
                              NULL);
  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Repair"));
  pw->ok_btn = CreateWindowExW(
      0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDOK, NULL, NULL);
  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Cancel"));
  pw->cancel_btn = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                   0,
                                   0,
                                   0,
                                   0,
                                   pw->hwnd,
                                   (HMENU)(INT_PTR)IDCANCEL,
                                   NULL,
                                   NULL);
  if ((pw->label == NULL) || (pw->combo == NULL) || (pw->ok_btn == NULL) || (pw->cancel_btn == NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!theme_attach(pw->hwnd, err) || !theme_attach(pw->label, err) || !theme_attach(pw->combo, err) || !theme_attach(pw->ok_btn, err) ||
      !theme_attach(pw->cancel_btn, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!apply_font(pw, pw->dpi, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  for (size_t i = 0; i < OV_ARRAY_LENGTH(pw->targets); i++) {
    size_t const index = device_find_entry(pw->targets[i].devnode, pw->settings->devices, OV_ARRAY_LENGTH(pw->settings->devices), err);

    if (index == SIZE_MAX) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!label(&pw->settings->devices[index], pw->targets[i].devnode, line, sizeof(line) / sizeof(line[0]))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      goto cleanup;
    }
    if (SendMessageW(pw->combo, CB_ADDSTRING, 0, (LPARAM)line) == (LRESULT)CB_ERR) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (wcslen(line) > widest_len) {
      widest_len = wcslen(line);
      memcpy(widest, line, (widest_len + 1) * sizeof(line[0]));
    }
  }
  SendMessageW(pw->combo, CB_SETCURSEL, 0, 0);
  if (!fit(pw, label_text, (widest[0] != L'\0') ? widest : NULL, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!layout(pw, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief The window procedure of the picker window
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @return the answer the message carries
 */
static LRESULT CALLBACK proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct picker_window *pw = (struct picker_window *)GetPropW(hwnd, state_prop);

  switch (msg) {
  case WM_NCCREATE:
    if (SetPropW(hwnd, state_prop, (HANDLE)((CREATESTRUCTW const *)lparam)->lpCreateParams) == 0) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
      return FALSE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  case WM_CREATE:
    if (pw != NULL) {
      pw->hwnd = hwnd;
      if (!build(pw, pw->err)) {
        pw->failed = true;
      }
    }
    return 0;
  case WM_DESTROY: {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the popup held goes back to the owner
    }
    if (pw != NULL) {
      if (pw->font != NULL) {
        if (!DeleteObject(pw->font)) {
          struct ov_error err = {0};

          OV_ERROR_SET_GENERIC(&err, ov_error_generic_fail);
          OV_ERROR_REPORT(&err, NULL);
        }
        pw->font = NULL;
      }
      if (!RemovePropW(hwnd, state_prop)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      OV_FREE(&pw);
    }
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;
    struct ov_error err = {0};
    bool ok = false;

    /* the reader chose this size: the monitor's suggestion of it is kept,
     * only the font and the geometry follow the new dpi */
    if (!SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    } else if (!apply_font(pw, (int)LOWORD(wparam), &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else if (!layout(pw, &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else {
      ok = true;
    }
    if (!ok) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_SIZE: {
    struct ov_error err = {0};

    if (!layout(pw, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_COMMAND: {
    struct ov_error err = {0};
    bool ok = true;

    if (LOWORD(wparam) == (WORD)IDCANCEL) {
      ok = answer(pw, false, &err); // IsDialogMessageW sends this, with no notification code, for ESC
    } else if ((LOWORD(wparam) == (WORD)IDOK) && (HIWORD(wparam) == BN_CLICKED)) {
      ok = answer(pw, true, &err);
    }
    if (!ok) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_CLOSE: {
    struct ov_error err = {0};

    if (!answer(pw, false, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  default:
    break;
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
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief Register the window class of the picker window
 *
 * The class is registered once per process, the way the main window does it.
 *
 * @param err receives the failure of the registration
 * @return false when the class could not be registered
 */
static NODISCARD bool register_class(struct ov_error *const err) {
  static ATOM atom = 0;

  if (atom != 0) {
    return true;
  }
  {
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    // the system deletes the background brush of a class when the class is unregistered,
    // so the class gets a brush of its own
    wc.hbrBackground = theme_class_brush(err);
    if (wc.hbrBackground == NULL) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    wc.lpszClassName = L"audient_device_closer_picker";
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

NODISCARD bool ui_device_picker_show(HWND const owner,
                                     struct settings const *const s,
                                     struct device_target const *const targets,
                                     struct device_target **const out_target,
                                     struct ov_error *const err) {
  struct picker_window *pw = NULL;
  struct picker_result result = {.selected = -1};
  struct device_target *picked = NULL;
  HWND window = NULL; // the handle of the window while the modal loop is on it
  bool success = false;

  if ((owner == NULL) || (s == NULL) || (targets == NULL) || (out_target == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_target = NULL;
  if (OV_ARRAY_LENGTH(targets) == 0) {
    success = true; // nothing is connected, so there is nothing to pick
    goto cleanup;
  }
  if (!register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!OV_REALLOC(&pw, 1, sizeof(*pw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *pw = (struct picker_window){0};
  pw->dpi = (int)GetDpiForWindow(owner);
  pw->result = &result;
  pw->settings = s;
  pw->targets = targets;
  pw->err = err;
  {
    RECT owner_rc = {0, 0, 0, 0};
    RECT outer;
    int width = 0;
    int height = 0;

    if (!GetWindowRect(owner, &owner_rc)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    layout_window_rect(layout_scale(ADC_PICKER_W, (UINT)pw->dpi),
                       layout_scale(ADC_LAYOUT_MARGIN_TOP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_LABEL_H, (UINT)pw->dpi) +
                           layout_scale(ADC_LAYOUT_GAP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_COMBO_H, (UINT)pw->dpi) +
                           layout_scale(ADC_LAYOUT_GAP, (UINT)pw->dpi) + layout_scale(ADC_PICKER_BUTTON_H, (UINT)pw->dpi) +
                           layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, (UINT)pw->dpi),
                       (UINT)pw->dpi,
                       picker_window_style,
                       picker_window_extra,
                       &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
    pw->hwnd = CreateWindowExW(picker_window_extra,
                               L"audient_device_closer_picker",
                               L"",
                               picker_window_style,
                               owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                               owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                               width,
                               height,
                               owner,
                               NULL,
                               GetModuleHandleW(NULL),
                               pw);
  }
  if (pw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (pw->failed) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  window = pw->hwnd;
  EnableWindow(owner, FALSE); // the dialog holds its owner until the reader is done
  // ShowWindow answers the state the window was in and does not set the last error, so
  // there is nothing to read from it
  ShowWindow(window, SW_SHOW);
  if (!UpdateWindow(window)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (SetFocus(pw->cancel_btn) == NULL) { // the window opens on the answer that repairs nothing
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  pw = NULL;
  {
    MSG msg;

    while (IsWindow(window) && GetMessageW(&msg, NULL, 0, 0) > 0) {
      if (!IsDialogMessageW(window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
  if (result.answered && (result.selected >= 0) && ((size_t)result.selected < OV_ARRAY_LENGTH(targets))) {
    struct device_target const *const row = &targets[(size_t)result.selected];
    size_t const chars = wcslen(row->devnode) + 1;

    if (!OV_ARRAY_GROW(&picked, 1)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    picked[0] = (struct device_target){0};
    if (!OV_ARRAY_GROW(&picked[0].devnode, chars)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(picked[0].devnode, chars - 1);
    wcscpy(picked[0].devnode, row->devnode);
    picked[0].state = row->state;
    picked[0].problem_code = row->problem_code;
    OV_ARRAY_SET_LENGTH(picked, 1);
    *out_target = picked;
    picked = NULL;
  }
  success = true;

cleanup:
  if (picked != NULL) {
    device_targets_release(&picked);
  }
  if (pw != NULL) {
    if (pw->hwnd != NULL) {
      // answers through WM_DESTROY, which frees the state
      if (!DestroyWindow(pw->hwnd)) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    } else {
      OV_FREE(&pw);
    }
  }
  return success;
}
