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

#include "device.h"
#include "layout.h"
#include "repair.h"
#include "settings.h"

#define IDC_PICKER_LABEL 16
#define IDC_PICKER_COMBO 17
#define IDC_PICKER_OK 18
#define IDC_PICKER_CANCEL 19

/** @brief The room of one line of the combo */
enum { ADC_PICKER_TEXT_CHARS = 512 };

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

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
  struct picker_result *result; // borrowed
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
 * @param entry the settings entry the devnode belongs to, NULL when
 *        no entry names it
 * @param devnode the devnode of the connected unit, may not be NULL
 * @param out receives the line, UTF-8
 * @param out_size room of out
 * @return false when the arguments name nothing
 */
static bool
ui_device_picker_label(struct device_entry const *const entry, char const *const devnode, char *const out, size_t const out_size) {
  char const *const name = (entry != NULL) ? entry->name : NULL;

  if ((devnode == NULL) || (devnode[0] == '\0') || (out == NULL) || (out_size == 0)) {
    return false;
  }
  if (name == NULL) {
    OV_SNPRINTF(out, out_size, "%1$s", "%1$s", devnode); // no entry names the unit: the devnode is the only name
  } else {
    OV_SNPRINTF(out, out_size, "%1$s (%2$s)", "%1$s (%2$s)", name, devnode);
  }
  return true;
}

/**
 * @brief Bring the picker window onto a font of one dpi
 *
 * @param pw the window to draw with
 * @param dpi the dpi to draw for
 */
static void picker_apply_font(struct picker_window *const pw, int const dpi) {
  HFONT stale = pw->font;
  HFONT font = layout_make_font((UINT)dpi);

  pw->dpi = dpi;
  if (font == NULL) {
    return; // the geometry follows the monitor, the window keeps the font it has
  }
  pw->font = font;
  SendMessageW(pw->label, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->combo, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->ok_btn, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->cancel_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if (stale != NULL) {
    DeleteObject(stale);
    stale = NULL;
  }
}

/**
 * @brief The room a text takes in the font of the window
 *
 * @param pw the window that draws the text
 * @param text the text to measure
 * @param len how many characters of the text to measure
 * @return the width in pixels, 0 when it could not be measured
 */
static int picker_text_width(struct picker_window const *const pw, wchar_t const *const text, size_t const len) {
  HDC dc = GetDC(pw->hwnd);
  HFONT old = NULL;
  SIZE size = {0, 0};
  int width = 0;

  if (dc == NULL) {
    return 0;
  }
  if (pw->font != NULL) {
    old = (HFONT)SelectObject(dc, pw->font);
  }
  if (GetTextExtentPoint32W(dc, text, (int)len, &size)) {
    width = size.cx;
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(pw->hwnd, dc);
  return width;
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
 * @return the width of the widest line in pixels, 0 when it could not be measured
 */
static int picker_line_width(struct picker_window const *const pw, wchar_t const *const text) {
  wchar_t const *line = text;
  int width = 0;

  while (line != NULL) {
    wchar_t const *const end = wcschr(line, L'\n');
    int const room = picker_text_width(pw, line, (end != NULL) ? (size_t)(end - line) : wcslen(line));
    if (room > width) {
      width = room;
    }
    line = (end != NULL) ? (end + 1) : NULL;
  }
  return width;
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
static int picker_combo_room(struct picker_window const *const pw) {
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
  return button + (2 * GetSystemMetrics(SM_CXEDGE)) + layout_scale(ADC_LAYOUT_PICKER_COMBO_INSET, (UINT)pw->dpi);
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
 */
static void picker_fit(struct picker_window *const pw, wchar_t const *const label_text, wchar_t const *const widest) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)pw->dpi);
  int const least = layout_scale(ADC_LAYOUT_PICKER_W, (UINT)pw->dpi);
  int width = picker_line_width(pw, label_text);
  int combo_width = 0;
  int needed = 0;
  int client = 0;
  RECT outer;
  RECT now;

  if (widest != NULL) {
    combo_width = picker_text_width(pw, widest, wcslen(widest)) + picker_combo_room(pw);
  }
  if (combo_width > width) {
    width = combo_width;
  }
  needed = width + 2 * margin;
  client = (needed > least) ? needed : least;
  layout_window_rect(client,
                     layout_scale(ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_PICKER_LABEL_H + ADC_LAYOUT_GAP + ADC_LAYOUT_PICKER_COMBO_H +
                                      ADC_LAYOUT_GAP + ADC_LAYOUT_PICKER_BUTTON_H + ADC_LAYOUT_MARGIN_BOTTOM,
                                  (UINT)pw->dpi),
                     (UINT)pw->dpi,
                     picker_window_style,
                     picker_window_extra,
                     &outer);
  GetWindowRect(pw->hwnd, &now);
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return;
  }
  SetWindowPos(pw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  SendMessageW(pw->combo, CB_SETDROPPEDWIDTH, (WPARAM)(client - 2 * margin), 0);
}

/**
 * @brief Place the controls of the picker window
 *
 * The geometry itself lives in layout_picker_compute(), this only hands
 * the rectangles to the window manager.
 *
 * @param pw the picker window to place
 */
static void picker_layout(struct picker_window *const pw) {
  struct picker_layout g;
  RECT rc;

  GetClientRect(pw->hwnd, &rc);
  layout_picker_compute(rc.right - rc.left, (UINT)pw->dpi, &g);
  MoveWindow(pw->label, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE);
  MoveWindow(pw->combo, LX(g.combo), LY(g.combo), LW(g.combo), LH(g.combo), TRUE);
  MoveWindow(pw->ok_btn, LX(g.ok), LY(g.ok), LW(g.ok), LH(g.ok), TRUE);
  MoveWindow(pw->cancel_btn, LX(g.cancel), LY(g.cancel), LW(g.cancel), LH(g.cancel), TRUE);
}

/**
 * @brief Take the window down with the answer the reader gave
 *
 * @param pw the picker window that answers
 * @param answered the button that repairs was pressed
 */
static void picker_answer(struct picker_window *const pw, bool const answered) {
  if (pw->result != NULL) {
    if (answered) {
      pw->result->selected = (int)SendMessageW(pw->combo, CB_GETCURSEL, 0, 0);
    }
    pw->result->answered = answered;
  }
  DestroyWindow(pw->hwnd);
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
static LRESULT CALLBACK picker_proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct picker_window *pw = (struct picker_window *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

  switch (msg) {
  case WM_NCCREATE:
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW const *)lparam)->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  case WM_DESTROY: {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the popup held goes back to the owner
    }
    if (pw != NULL) {
      if (pw->font != NULL) {
        DeleteObject(pw->font);
        pw->font = NULL;
      }
      OV_FREE(&pw);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;

    /* the reader chose this size: the monitor's suggestion of it is kept,
     * only the font and the geometry follow the new dpi */
    SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
    picker_apply_font(pw, (int)LOWORD(wparam));
    picker_layout(pw);
    return 0;
  }
  case WM_SIZE:
    picker_layout(pw);
    return 0;
  case WM_COMMAND:
    if (HIWORD(wparam) != BN_CLICKED) {
      break;
    }
    if (LOWORD(wparam) == (WORD)IDC_PICKER_OK) {
      picker_answer(pw, true);
    } else if (LOWORD(wparam) == (WORD)IDC_PICKER_CANCEL) {
      picker_answer(pw, false);
    }
    return 0;
  case WM_CLOSE:
    picker_answer(pw, false);
    return 0;
  default:
    break;
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
static bool picker_register_class(struct ov_error *const err) {
  static ATOM atom = 0;

  if (atom != 0) {
    return true;
  }
  {
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = picker_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = theme_class_brush();
    wc.lpszClassName = L"audient_device_closer_picker";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

bool ui_device_picker_show(HWND const owner,
                           struct settings const *const s,
                           struct device_target const *const targets,
                           struct device_target **const out_target,
                           struct ov_error *const err) {
  struct picker_window *pw = NULL;
  struct picker_result result;
  struct device_target *picked = NULL;
  wchar_t title[128];
  wchar_t label_text[512];
  wchar_t ok_text[64];
  wchar_t cancel_text[64];
  wchar_t widest[ADC_PICKER_TEXT_CHARS];
  size_t widest_len = 0;
  RECT owner_rc = {0, 0, 0, 0};
  int dpi = 0;
  int width = 0;
  int height = 0;
  size_t i = 0;
  HWND window = NULL; // the handle of the window while the modal loop is on it
  bool success = false;

  if ((owner == NULL) || (s == NULL) || (targets == NULL) || (out_target == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_target = NULL;
  memset(&result, 0, sizeof(result));
  result.selected = -1;
  if (OV_ARRAY_LENGTH(targets) == 0) {
    success = true; // nothing is connected, so there is nothing to pick
    goto cleanup;
  }
  if (!picker_register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  dpi = (int)GetDpiForWindow(owner);
  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("Audient Device Closer"));
  OV_SNPRINTF(label_text,
              sizeof(label_text) / sizeof(WCHAR),
              ph,
              ph,
              gettext("None of the connected devices needs a repair.\nTo repair one anyway, choose it below."));
  OV_SNPRINTF(ok_text, sizeof(ok_text) / sizeof(WCHAR), ph, ph, gettext("Repair"));
  OV_SNPRINTF(cancel_text, sizeof(cancel_text) / sizeof(WCHAR), ph, ph, gettext("Cancel"));
  if (!OV_REALLOC(&pw, 1, sizeof(*pw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(pw, 0, sizeof(*pw));
  pw->dpi = dpi;
  pw->result = &result;
  GetWindowRect(owner, &owner_rc);
  {
    RECT outer;

    layout_window_rect(layout_scale(ADC_LAYOUT_PICKER_W, (UINT)dpi),
                       layout_scale(ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_PICKER_LABEL_H + ADC_LAYOUT_GAP + ADC_LAYOUT_PICKER_COMBO_H +
                                        ADC_LAYOUT_GAP + ADC_LAYOUT_PICKER_BUTTON_H + ADC_LAYOUT_MARGIN_BOTTOM,
                                    (UINT)dpi),
                       (UINT)dpi,
                       picker_window_style,
                       picker_window_extra,
                       &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
  }
  pw->hwnd = CreateWindowExW(picker_window_extra,
                             L"audient_device_closer_picker",
                             title,
                             picker_window_style,
                             owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                             owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                             width,
                             height,
                             owner,
                             NULL,
                             GetModuleHandleW(NULL),
                             pw);
  if (pw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  pw->label = CreateWindowExW(0,
                              L"STATIC",
                              (wchar_t const *)label_text,
                              WS_CHILD | WS_VISIBLE | SS_LEFT,
                              0,
                              0,
                              0,
                              0,
                              pw->hwnd,
                              (HMENU)(INT_PTR)IDC_PICKER_LABEL,
                              NULL,
                              NULL);
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
  pw->ok_btn = CreateWindowExW(0,
                               L"BUTTON",
                               (wchar_t const *)ok_text,
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                               0,
                               0,
                               0,
                               0,
                               pw->hwnd,
                               (HMENU)(INT_PTR)IDC_PICKER_OK,
                               NULL,
                               NULL);
  pw->cancel_btn = CreateWindowExW(0,
                                   L"BUTTON",
                                   (wchar_t const *)cancel_text,
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                   0,
                                   0,
                                   0,
                                   0,
                                   pw->hwnd,
                                   (HMENU)(INT_PTR)IDC_PICKER_CANCEL,
                                   NULL,
                                   NULL);
  if ((pw->label == NULL) || (pw->combo == NULL) || (pw->ok_btn == NULL) || (pw->cancel_btn == NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  theme_attach(pw->hwnd);
  theme_attach(pw->label);
  theme_attach(pw->combo);
  theme_attach(pw->ok_btn);
  theme_attach(pw->cancel_btn);
  picker_apply_font(pw, dpi);
  widest[0] = L'\0';
  for (i = 0; i < OV_ARRAY_LENGTH(targets); i++) {
    char line[ADC_PICKER_TEXT_CHARS];
    wchar_t wline[ADC_PICKER_TEXT_CHARS];
    bool found = false;
    size_t index = 0;
    struct device_entry const *entry = NULL;
    char const *text = NULL;

    if (!device_find_entry(targets[i].devnode, s->devices, OV_ARRAY_LENGTH(s->devices), &found, &index, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    entry = found ? &s->devices[index] : NULL;
    if (!ui_device_picker_label(entry, targets[i].devnode, line, sizeof(line))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      goto cleanup;
    }
    text = line;
    if (OV_SNPRINTF(wline, sizeof(wline) / sizeof(WCHAR), ph, ph, text) < 0) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
      goto cleanup;
    }
    if (SendMessageW(pw->combo, CB_ADDSTRING, 0, (LPARAM)wline) == (LRESULT)CB_ERR) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (wcslen(wline) > widest_len) {
      widest_len = wcslen(wline);
      memcpy(widest, wline, (wcslen(wline) + 1) * sizeof(WCHAR));
    }
  }
  SendMessageW(pw->combo, CB_SETCURSEL, 0, 0);
  picker_fit(pw, label_text, widest[0] != L'\0' ? widest : NULL);
  picker_layout(pw);
  EnableWindow(owner, FALSE); // the dialog holds its owner until the reader is done
  ShowWindow(pw->hwnd, SW_SHOW);
  UpdateWindow(pw->hwnd);
  window = pw->hwnd;
  pw = NULL;
  success = true;

cleanup:
  if ((pw != NULL) && (pw->hwnd != NULL)) {
    DestroyWindow(pw->hwnd); // answers through WM_DESTROY, which frees the state
  } else if (pw != NULL) {
    if (pw->font != NULL) {
      DeleteObject(pw->font);
      pw->font = NULL;
    }
    OV_FREE(&pw);
  }
  if (!success) {
    return false;
  }
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
    struct device_target const *row = &targets[(size_t)result.selected];
    struct device_target t;
    char *devnode = NULL;

    if (!ov_sprintf_char(&devnode, err, NULL, "%s", row->devnode)) {
      OV_ERROR_ADD_TRACE(err);
      success = false;
      goto release;
    }
    memset(&t, 0, sizeof(t));
    t.devnode = devnode;
    t.state = row->state;
    if (!OV_ARRAY_PUSH(&picked, t)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      device_string_free(&devnode);
      success = false;
      goto release;
    }
    *out_target = picked;
  }
release:
  return success;
}
