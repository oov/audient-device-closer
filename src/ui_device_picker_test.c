#include <windows.h>

#include "test_suppress.h"

/**
 * @brief Put the hooks in place that keep the windows of this test out of the reader's way
 *
 * ovtest.h calls this before anything of the test runs, which is early enough for the hook
 * to catch the first window.
 */
static void adc_test_init(void) { test_suppress_install_quiet(); }

#define TEST_MY_INIT adc_test_init()
#define TEST_MY_FINI test_suppress_release()

#include "ui_device_picker.c"

#include <ovtest.h>

#include <string.h>

#include <commctrl.h>
#include <uxtheme.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "device_probe.h"
#include "layout.h"
#include "repair.h"
#include "settings.h"
#include "theme.h"

enum {
  ADC_PICKER_TEST_TIMER_ID = 2,
  ADC_PICKER_TEST_TIMER_MS = 100,
};

/**
 * @brief The height argument of CreateFontW, the same rule the layout uses
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
static int test_font_height(UINT const dpi) { return layout_font_height(dpi); }
/**
 * @brief Which palette the system asks this program to use
 *
 * The tests ask the system instead of the theme they check: the answer is the input both
 * work from, and a test that read it off the theme could only ever agree with the theme,
 * never tell that the theme took the wrong branch.
 *
 * @return true when the system is in the dark mode and no high contrast is on
 */
static bool theme_dark(void) {
  typedef BOOL(WINAPI * dark_mode_fn)(void);
  HMODULE const uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HIGHCONTRASTW hc;
  bool dark = false;

  if (uxtheme != NULL) {
    dark_mode_fn const should = (dark_mode_fn)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(132));

    dark = (should != NULL) && (should() != FALSE);
    FreeLibrary(uxtheme);
  }
  memset(&hc, 0, sizeof(hc));
  hc.cbSize = sizeof(hc);
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, FALSE) && ((hc.dwFlags & HCF_HIGHCONTRASTON) != 0)) {
    dark = false;
  }
  return dark;
}

/**
 * @brief Does one control draw with a font of the given dpi
 *
 * @param control the control to read the font of
 * @param dpi the dpi the font has to be made for
 * @return false when the control has no font, or a font that is not there any more
 */
static bool font_of_dpi(HWND const control, int const dpi) {
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(control, WM_GETFONT, 0, 0);
  LOGFONTW lf;

  memset(&lf, 0, sizeof(lf));
  if (font == NULL) {
    return false;
  }
  if (GetObjectW(font, sizeof(lf), &lf) == 0) {
    return false;
  }
  return lf.lfHeight == test_font_height((UINT)dpi);
}

/**
 * @brief Does every line of the text of one control fit the room it is given
 *
 * The lines of the text are the parts its line
 * breaks separate, and each of them has to fit
 * the room of the control on its own.
 *
 * @param control the control to read
 * @return false when a line is cut off, or nothing could be measured
 */
static bool control_lines_fit(HWND const control) {
  wchar_t text[512];
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(control, WM_GETFONT, 0, 0);
  HDC dc = GetDC(control);
  HFONT old = NULL;
  RECT rc;
  bool fits = true;

  if (dc == NULL) {
    return false;
  }
  memset(text, 0, sizeof(text));
  GetWindowTextW(control, text, (int)(sizeof(text) / sizeof(text[0])) - 1);
  old = (HFONT)SelectObject(dc, font);
  GetClientRect(control, &rc);
  for (wchar_t const *line = text; (line != NULL) && fits;) {
    wchar_t const *const end = wcschr(line, L'\n');
    int const len = (int)((end != NULL) ? (size_t)(end - line) : wcslen(line));
    SIZE size = {0, 0};

    if ((len > 0) && ((GetTextExtentPoint32W(dc, line, len, &size) == 0) || (size.cx > (rc.right - rc.left)))) {
      fits = false;
    }
    line = (end != NULL) ? (end + 1) : NULL;
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(control, dc);
  return fits;
}

/**
 * @brief Does the line the combo shows fit the room the combo keeps for it
 *
 * The button that opens the list, the frame
 * around the line and the padding the line is
 * drawn away from take room of the combo, so
 * the line has to fit in the room that is left.
 *
 * @param combo the combo to read
 * @return false when the line is cut off, or nothing could be measured
 */
static bool combo_line_fits(HWND const combo) {
  wchar_t text[512];
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(combo, WM_GETFONT, 0, 0);
  HDC dc = GetDC(combo);
  HFONT old = NULL;
  SIZE size = {0, 0};
  RECT rc;
  COMBOBOXINFO info = {0};
  int button = 0;
  bool measured = false;

  if (dc == NULL) {
    return false;
  }
  memset(text, 0, sizeof(text));
  SendMessageW(combo, WM_GETTEXT, (WPARAM)(sizeof(text) / sizeof(text[0])), (LPARAM)text);
  old = (HFONT)SelectObject(dc, font);
  measured = GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size) != FALSE;
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(combo, dc);
  if (!measured) {
    return false;
  }
  info.cbSize = sizeof(info);
  if (GetComboBoxInfo(combo, &info) && ((info.rcButton.right - info.rcButton.left) > 0)) {
    button = info.rcButton.right - info.rcButton.left;
  } else {
    button = GetSystemMetrics(SM_CXVSCROLL);
  }
  GetClientRect(combo, &rc);
  return (size.cx + button + (2 * GetSystemMetrics(SM_CXEDGE)) + layout_scale(ADC_PICKER_COMBO_INSET, GetDpiForWindow(combo))) <=
         (rc.right - rc.left);
}

/**
 * @brief Read the picker window while it is up, then answer it
 *
 * The units the combo holds are the two
 * the timer was made for, so each line has
 * to name its unit, and the window has to
 * be wide enough for the widest one to be
 * read whole.  The answer is the press of
 * the button that repairs.
 *
 * @param hwnd the owner the timer was made for
 * @param msg the message of the timer
 * @param id the identifier of the timer
 * @param tickcount the tick count when the timer fired
 */
static void CALLBACK picker_test_timer(HWND const hwnd, UINT const msg, UINT_PTR const id, DWORD const tickcount) {
  static wchar_t const first_unit[] = L"iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2)";
  static wchar_t const second_unit[] = L"iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&feedface&0&3)";
  HWND picker = NULL;
  HWND combo = NULL;
  HWND label = NULL;
  HWND ok_btn = NULL;
  int lines = 0;
  int i = 0;

  (void)msg;
  (void)id;
  (void)tickcount;
  KillTimer(hwnd, ADC_PICKER_TEST_TIMER_ID);
  picker = GetLastActivePopup(hwnd);
  if (!TEST_CHECK((picker != NULL) && (picker != hwnd))) {
    return;
  }
  TEST_CHECK(IsWindowEnabled(hwnd) == FALSE);
  TEST_MSG("want a locked owner while the picker is up");
  combo = FindWindowExW(picker, NULL, WC_COMBOBOXW, NULL);
  if (TEST_CHECK(combo != NULL)) {
    lines = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    TEST_CHECK_(lines == 2, "want the two units of the device, got %d", lines);
    for (i = 0; i < lines; i++) {
      wchar_t line[512];
      wchar_t const *const want = (i == 0) ? first_unit : second_unit;

      memset(line, 0, sizeof(line));
      SendMessageW(combo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)line);
      TEST_CHECK_(lstrcmpW(line, want) == 0, "unit %d: want [%ls], got [%ls]", i, want, line);
    }
    TEST_CHECK(combo_line_fits(combo));
    TEST_MSG("want the widest unit readable whole in the room the combo keeps");
    if (theme_dark()) {
      // the list of the drop down is a window of the system the control makes for itself:
      // the theme class of the palette gives it its dark look, and this component paints
      // the frame the theme of the system leaves in the light border of the classic
      // frame.  The check reads the frame back through WM_PRINT, which paints into a
      // context the test holds, so the reader never depends on the list being on the
      // screen, and the list never has to be dropped to be read
      COMBOBOXINFO info = {0};

      info.cbSize = sizeof(info);
      if (TEST_CHECK(GetComboBoxInfo(combo, &info) && (info.hwndList != NULL))) {
        HDC dc = GetDC(info.hwndList);
        HDC mem = CreateCompatibleDC(dc);
        RECT rc = {0, 0, 0, 0};
        HBITMAP bmp = NULL;
        HBITMAP old = NULL;
        COLORREF frame_pixel = 0;

        GetWindowRect(info.hwndList, &rc);
        bmp = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        if (TEST_CHECK((dc != NULL) && (mem != NULL) && (bmp != NULL))) {
          old = (HBITMAP)SelectObject(mem, bmp);
          SendMessageW(info.hwndList, WM_PRINT, (WPARAM)mem, PRF_NONCLIENT);
          frame_pixel = GetPixel(mem, 0, (rc.bottom - rc.top) / 2);
          SelectObject(mem, old);
          old = NULL;
        }
        if (bmp != NULL) {
          DeleteObject(bmp);
          bmp = NULL;
        }
        if (mem != NULL) {
          DeleteDC(mem);
          mem = NULL;
        }
        if (dc != NULL) {
          ReleaseDC(info.hwndList, dc);
          dc = NULL;
        }
        TEST_CHECK_(
            frame_pixel == RGB(0x39, 0x39, 0x39), "want the frame of the list in the colour of the dark palette, got %08lx", frame_pixel);
        // the rows of the list are painted here, and the reader reads them back
        // through WM_PRINTCLIENT, which paints into a context the test holds
        {
          HDC const row_dc = GetDC(info.hwndList);
          HDC row_mem = CreateCompatibleDC(row_dc);
          HBITMAP row_bmp = NULL;
          HBITMAP row_old = NULL;
          COLORREF row = 0;
          COLORREF chosen_row = 0;
          RECT cr = {0, 0, 0, 0};
          LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);

          GetClientRect(info.hwndList, &cr);
          row_bmp = CreateCompatibleBitmap(row_dc, cr.right - cr.left, cr.bottom - cr.top);
          if (TEST_CHECK((row_dc != NULL) && (row_mem != NULL) && (row_bmp != NULL))) {
            row_old = (HBITMAP)SelectObject(row_mem, row_bmp);
            SendMessageW(info.hwndList, WM_PRINTCLIENT, (WPARAM)row_mem, PRF_CLIENT);
            // the first row is the one the reader chose, the second one is not,
            // and the name of a unit starts a little into the row, so the edge
            // of the row is the face of the row
            row = GetPixel(row_mem, 0, item_height + 3);
            chosen_row = GetPixel(row_mem, 0, 3);
            SelectObject(row_mem, row_old);
            row_old = NULL;
          }
          if (row_bmp != NULL) {
            DeleteObject(row_bmp);
            row_bmp = NULL;
          }
          if (row_mem != NULL) {
            DeleteDC(row_mem);
            row_mem = NULL;
          }
          if (row_dc != NULL) {
            ReleaseDC(info.hwndList, row_dc);
          }
          TEST_CHECK_(row == RGB(0x2B, 0x2B, 0x2B), "want the face of a row of the list in the colour of the dark palette, got %08lx", row);
          TEST_CHECK_(chosen_row == GetSysColor(COLOR_HIGHLIGHT),
                      "want the chosen row in the colour of the selection of Windows, got %08lx",
                      chosen_row);
        }
      }
    }
    if (theme_dark()) {
      // The rows the reader sees are the ones the open list paints, so the check opens the
      // list and has it paint into a context the test holds.  The screen is not read: the
      // window of a test shows nothing, and what a window draws is what WM_PRINTCLIENT
      // answers, which is the painting itself and never a race against a repaint.
      COMBOBOXINFO dd_info;
      LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
      HDC dd_dc = NULL;
      HDC dd_mem = NULL;
      HBITMAP dd_bmp = NULL;
      HBITMAP dd_old = NULL;
      RECT dd_rc = {0, 0, 0, 0};
      COLORREF plain_px = 0;
      COLORREF chosen_px = 0;

      memset(&dd_info, 0, sizeof(dd_info));
      dd_info.cbSize = sizeof(dd_info);
      GetComboBoxInfo(combo, &dd_info);
      {
        // the reader opens the list the way a click on the arrow does
        LONG const arrow_x = (dd_info.rcButton.left + dd_info.rcButton.right) / 2;
        LONG const arrow_y = (dd_info.rcButton.top + dd_info.rcButton.bottom) / 2;
        SendMessageW(combo, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(arrow_x, arrow_y));
        SendMessageW(combo, WM_LBUTTONUP, 0, MAKELPARAM(arrow_x, arrow_y));
      }
      GetClientRect(dd_info.hwndList, &dd_rc);
      dd_dc = GetDC(dd_info.hwndList);
      dd_mem = CreateCompatibleDC(dd_dc);
      if (dd_mem != NULL) {
        dd_bmp = CreateCompatibleBitmap(dd_dc, dd_rc.right - dd_rc.left, dd_rc.bottom - dd_rc.top);
      }
      if (TEST_CHECK((dd_dc != NULL) && (dd_mem != NULL) && (dd_bmp != NULL))) {
        dd_old = (HBITMAP)SelectObject(dd_mem, dd_bmp);
        SendMessageW(dd_info.hwndList, WM_PRINTCLIENT, (WPARAM)dd_mem, PRF_CLIENT);
        plain_px = GetPixel(dd_mem, 0, item_height + 3);
        chosen_px = GetPixel(dd_mem, 0, 3);
        SelectObject(dd_mem, dd_old);
        dd_old = NULL;
      }
      if (dd_bmp != NULL) {
        DeleteObject(dd_bmp);
        dd_bmp = NULL;
      }
      if (dd_mem != NULL) {
        DeleteDC(dd_mem);
        dd_mem = NULL;
      }
      if (dd_dc != NULL) {
        ReleaseDC(dd_info.hwndList, dd_dc);
      }
      SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
      TEST_CHECK_(plain_px == RGB(0x2B, 0x2B, 0x2B),
                  "want the unchosen row of the open list in the colour of the dark palette, got %08lx",
                  plain_px);
      TEST_CHECK_(chosen_px == GetSysColor(COLOR_HIGHLIGHT),
                  "want the chosen row of the open list in the colour of the selection, got %08lx",
                  chosen_px);
    }
    if (theme_dark()) {
      // the list is erased before it is painted, and an animation of the drop shows the area
      // the erase covers first, so the erase has to fill it in the palette too
      HDC erase_dc = NULL;
      HDC erase_mem = NULL;
      HBITMAP erase_bmp = NULL;
      HBITMAP erase_old = NULL;
      RECT erase_rc = {0, 0, 64, 64};
      COLORREF erased = 0;
      COMBOBOXINFO erase_info;

      memset(&erase_info, 0, sizeof(erase_info));
      erase_info.cbSize = sizeof(erase_info);
      GetComboBoxInfo(combo, &erase_info);
      erase_dc = GetDC(erase_info.hwndList);
      erase_mem = CreateCompatibleDC(erase_dc);
      if (erase_mem != NULL) {
        erase_bmp = CreateCompatibleBitmap(erase_dc, 64, 64);
      }
      if ((erase_mem != NULL) && (erase_bmp != NULL)) {
        erase_old = (HBITMAP)SelectObject(erase_mem, erase_bmp);
        FillRect(erase_mem, &erase_rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SendMessageW(erase_info.hwndList, WM_ERASEBKGND, (WPARAM)erase_mem, 0);
        erased = GetPixel(erase_mem, 5, 5);
        SelectObject(erase_mem, erase_old);
      }
      if (erase_bmp != NULL) {
        DeleteObject(erase_bmp);
      }
      if (erase_mem != NULL) {
        DeleteDC(erase_mem);
      }
      if (erase_dc != NULL) {
        ReleaseDC(erase_info.hwndList, erase_dc);
      }
      TEST_CHECK_(erased == RGB(0x2B, 0x2B, 0x2B), "want the erase of the list in the colour of the dark palette, got %08lx", erased);
    }
    if (theme_dark()) {
      // The selection is moved with the arrow keys while the list is open, and the rows have
      // to follow the selection.  The painting is read back through WM_PRINTCLIENT, which is
      // the painting itself and not the screen.
      COMBOBOXINFO key_info;
      LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
      HDC key_dc = NULL;
      HDC key_mem = NULL;
      HBITMAP key_bmp = NULL;
      HBITMAP key_old = NULL;
      RECT key_rc = {0, 0, 0, 0};
      COLORREF first_px = 0;
      COLORREF second_px = 0;
      LRESULT moved = 0;

      memset(&key_info, 0, sizeof(key_info));
      key_info.cbSize = sizeof(key_info);
      GetComboBoxInfo(combo, &key_info);
      {
        LONG const arrow_x = (key_info.rcButton.left + key_info.rcButton.right) / 2;
        LONG const arrow_y = (key_info.rcButton.top + key_info.rcButton.bottom) / 2;
        SendMessageW(combo, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(arrow_x, arrow_y));
        SendMessageW(combo, WM_LBUTTONUP, 0, MAKELPARAM(arrow_x, arrow_y));
      }
      SendMessageW(key_info.hwndList, WM_KEYDOWN, VK_DOWN, 0);
      SendMessageW(key_info.hwndList, WM_KEYUP, VK_DOWN, 0);
      moved = SendMessageW(key_info.hwndList, LB_GETCURSEL, 0, 0);
      GetClientRect(key_info.hwndList, &key_rc);
      key_dc = GetDC(key_info.hwndList);
      key_mem = CreateCompatibleDC(key_dc);
      if (key_mem != NULL) {
        key_bmp = CreateCompatibleBitmap(key_dc, key_rc.right - key_rc.left, key_rc.bottom - key_rc.top);
      }
      if (TEST_CHECK((key_dc != NULL) && (key_mem != NULL) && (key_bmp != NULL))) {
        key_old = (HBITMAP)SelectObject(key_mem, key_bmp);
        SendMessageW(key_info.hwndList, WM_PRINTCLIENT, (WPARAM)key_mem, PRF_CLIENT);
        first_px = GetPixel(key_mem, 0, 3);
        second_px = GetPixel(key_mem, 0, item_height + 3);
        SelectObject(key_mem, key_old);
        key_old = NULL;
      }
      if (key_bmp != NULL) {
        DeleteObject(key_bmp);
        key_bmp = NULL;
      }
      if (key_mem != NULL) {
        DeleteDC(key_mem);
        key_mem = NULL;
      }
      if (key_dc != NULL) {
        ReleaseDC(key_info.hwndList, key_dc);
      }
      // the choice is put back to the first unit, the window answers with the chosen one
      SendMessageW(key_info.hwndList, WM_KEYDOWN, VK_UP, 0);
      SendMessageW(key_info.hwndList, WM_KEYUP, VK_UP, 0);
      SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
      TEST_CHECK_(moved == 1, "want the arrow key to choose the second unit of the open list, got %ld", (long)moved);
      TEST_CHECK_(first_px == RGB(0x2B, 0x2B, 0x2B),
                  "want the first row, now unchosen, in the colour of the dark palette after the arrow key, got %08lx",
                  first_px);
      TEST_CHECK_(second_px == GetSysColor(COLOR_HIGHLIGHT),
                  "want the second row, now chosen, in the colour of the selection after the arrow key, got %08lx",
                  second_px);
    }
    if (theme_dark()) {
      // the field of a drop down list is painted from the theme, which is light in every
      // state, so the component paints it: the check reads the field back through
      // WM_PRINTCLIENT, which paints without the window and never races a repaint
      HDC screen = GetDC(combo);
      HDC mem = CreateCompatibleDC(screen);
      HBITMAP bmp = NULL;
      HBITMAP old = NULL;
      RECT rc;
      COLORREF frame_pixel = 0;
      COLORREF face_pixel = 0;

      GetClientRect(combo, &rc);
      bmp = CreateCompatibleBitmap(screen, rc.right - rc.left, rc.bottom - rc.top);
      if ((screen != NULL) && (mem != NULL) && (bmp != NULL)) {
        old = (HBITMAP)SelectObject(mem, bmp);
        SendMessageW(combo, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT);
        frame_pixel = GetPixel(mem, 0, (rc.bottom - rc.top) / 2);
        face_pixel = GetPixel(mem, 1, 1);
        SelectObject(mem, old);
        old = NULL;
      }
      if (bmp != NULL) {
        DeleteObject(bmp);
        bmp = NULL;
      }
      if (mem != NULL) {
        DeleteDC(mem);
        mem = NULL;
      }
      if (screen != NULL) {
        ReleaseDC(combo, screen);
        screen = NULL;
      }
      TEST_CHECK_(
          frame_pixel == RGB(0x39, 0x39, 0x39), "want the frame of the field in the colour of the dark palette, got %08lx", frame_pixel);
      TEST_CHECK_(
          face_pixel == RGB(0x2D, 0x2D, 0x2D), "want the face of the field in the colour of the dark palette, got %08lx", face_pixel);
    }
  }
  label = FindWindowExW(picker, NULL, L"STATIC", NULL);
  if (TEST_CHECK(label != NULL)) {
    TEST_CHECK(control_lines_fit(label));
    TEST_MSG("want every sentence of the question readable whole in the room it is given");
  }
  ok_btn = FindWindowExW(picker, NULL, L"Button", NULL);
  if (!TEST_CHECK(ok_btn != NULL)) {
    return;
  }
  TEST_CHECK(font_of_dpi(ok_btn, (int)GetDpiForWindow(picker)));
  TEST_MSG("want the button drawn with a font of the window");
  SendMessageW(picker, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok_btn);
  TEST_CHECK(!IsWindow(picker));
  TEST_MSG("want the picker window gone after the press of its repair button");
}

/**
 * @brief Answer the picker with the key that answers nothing
 *
 * The window opens on the button that answers nothing, and the key takes it down without
 * an answer.  The message is the one IsDialogMessageW sends for ESC.
 *
 * @param hwnd the owner the timer was made for
 * @param msg the message of the timer
 * @param id the identifier of the timer
 * @param tickcount the tick count when the timer fired
 */
static void CALLBACK picker_test_cancel_timer(HWND const hwnd, UINT const msg, UINT_PTR const id, DWORD const tickcount) {
  HWND picker = NULL;
  HWND ok_btn = NULL;
  HWND cancel_btn = NULL;

  (void)msg;
  (void)id;
  (void)tickcount;
  KillTimer(hwnd, ADC_PICKER_TEST_TIMER_ID);
  picker = GetLastActivePopup(hwnd);
  if (!TEST_CHECK((picker != NULL) && (picker != hwnd))) {
    return;
  }
  ok_btn = FindWindowExW(picker, NULL, L"Button", NULL);
  if (!TEST_CHECK(ok_btn != NULL)) {
    return;
  }
  cancel_btn = FindWindowExW(picker, ok_btn, L"Button", NULL);
  if (!TEST_CHECK(cancel_btn != NULL)) {
    return;
  }
  {
    GUITHREADINFO gti = {.cbSize = sizeof(gti)};

    // GetFocus only answers a window of the queue of this thread, and the queue holds no
    // focused window while another window of the reader holds the input.  The focus of the
    // thread is what the picker set, and the system keeps it where GetGUIThreadInfo reads it.
    if (!TEST_CHECK(GetGUIThreadInfo(GetCurrentThreadId(), &gti))) {
      return;
    }
    TEST_CHECK_(gti.hwndFocus == cancel_btn,
                "want the window to open on the button that answers nothing, focus is %p, cancel is %p",
                (void *)gti.hwndFocus,
                (void *)cancel_btn);
  }
  SendMessageW(picker, WM_COMMAND, IDCANCEL, (LPARAM)cancel_btn);
  TEST_CHECK(!IsWindow(picker));
  TEST_MSG("want the picker window gone after the key that answers nothing");
}

/**
 * @brief The line of the combo names the device and the unit of it
 *
 * The driver package lists one name for more than one device,
 * and the same device can be connected more than once, so the
 * line carries the devnode of the unit.  Only a unit an entry
 * names reaches here, so the name is always there.
 */
static void test_device_picker_label(void) {
  struct device_entry const entry = {.id = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS", .name = L"iD14"};
  wchar_t line[256];
  wchar_t const unit[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2";

  TEST_CHECK(label(&entry, unit, line, sizeof(line) / sizeof(line[0])) == true);
  TEST_CHECK(wcscmp(line, L"iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2)") == 0);
  TEST_CHECK(label(NULL, unit, line, sizeof(line) / sizeof(line[0])) == false);
  TEST_CHECK(label(&entry, NULL, line, sizeof(line) / sizeof(line[0])) == false);
  TEST_CHECK(label(&entry, unit, NULL, sizeof(line) / sizeof(line[0])) == false);
  TEST_CHECK(label(&entry, unit, line, 0) == false);
  TEST_CHECK(label(&entry, L"", line, sizeof(line) / sizeof(line[0])) == false);
}

/**
 * @brief The picker names every unit of the device and answers with the one
 *
 * Two units of one device are connected, so the
 * combo has to tell them apart by the unit
 * each line names, and the window has to be
 * wide enough for the widest one to be read
 * whole.  The reader answers with the button
 * that repairs, and the window hands the unit
 * it held selected back to the caller.
 */
static void test_picker_window_names_every_unit(void) {
  struct device_entry *devices = NULL;
  struct device_target *targets = NULL;
  struct device_target *picked = NULL;
  struct settings s;
  struct device_entry entry;
  struct device_target target;
  struct ov_error err = {0};
  ATOM atom = 0;
  HFONT font = NULL;
  HWND owner = NULL;
  WNDCLASSEXW wc;

  memset(&s, 0, sizeof(s));
  memset(&entry, 0, sizeof(entry));
  entry.id = L"TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS";
  entry.name = L"iD14";
  TEST_CHECK(OV_ARRAY_PUSH(&devices, entry));
  entry.id = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS";
  TEST_CHECK(OV_ARRAY_PUSH(&devices, entry));
  s.devices = devices;
  memset(&target, 0, sizeof(target));
  target.devnode = L"TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2";
  target.state = DEVICE_STATE_PROBLEM;
  TEST_CHECK(OV_ARRAY_PUSH(&targets, target));
  target.devnode = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&feedface&0&3";
  target.state = DEVICE_STATE_PROBLEM;
  TEST_CHECK(OV_ARRAY_PUSH(&targets, target));

  TEST_SUCCEEDED(theme_init(true, &err), &err);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&err);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_picker_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  font = CreateFontW(test_font_height(USER_DEFAULT_SCREEN_DPI),
                     0,
                     0,
                     0,
                     FW_NORMAL,
                     FALSE,
                     FALSE,
                     FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
  if (!TEST_CHECK(font != NULL)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_attach(owner, &err), &err);
  SendMessageW(owner, WM_SETFONT, (WPARAM)font, TRUE);
  ShowWindow(owner, SW_SHOW);

  TEST_CASE("the picker names every unit and answers with the one it held");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_PICKER_TEST_TIMER_ID, ADC_PICKER_TEST_TIMER_MS, picker_test_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_device_picker_show(owner, &s, targets, &picked, &err), &err)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_PICKER_TEST_TIMER_ID);
    TEST_CHECK(IsWindowEnabled(owner) != 0);
    TEST_MSG("want the owner unlocked when the picker is done");
    TEST_CHECK(GetActiveWindow() == owner);
    TEST_MSG("want the owner active again after the popup is gone");
    if (TEST_CHECK(picked != NULL)) {
      TEST_CHECK_(wcscmp(picked->devnode, L"TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2") == 0,
                  "want the first unit, got [%ls]",
                  picked->devnode);
    }
    device_targets_release(&picked);
  }
  TEST_CASE("the picker answers nothing when the key that answers nothing is pressed");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_PICKER_TEST_TIMER_ID, ADC_PICKER_TEST_TIMER_MS, picker_test_cancel_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_device_picker_show(owner, &s, targets, &picked, &err), &err)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_PICKER_TEST_TIMER_ID);
    TEST_CHECK(picked == NULL);
    TEST_MSG("want no device when the reader answered nothing");
  }
  TEST_CASE_(NULL);

cleanup:
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (font != NULL) {
    DeleteObject(font);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  OV_ARRAY_DESTROY(&targets);
  OV_ARRAY_DESTROY(&devices);
}

/** @brief The dpis the checks of the geometry walk */
static UINT const geometry_dpi_list[] = {96, 120, 144, 192};
#define GEOMETRY_DPI_COUNT ((int)(sizeof(geometry_dpi_list) / sizeof(geometry_dpi_list[0])))

/**
 * @brief The buttons that answer the picker ride the right edge
 *
 * The two buttons are a pair at the right of the window,
 * the one that repairs on the left of the one that answers
 * nothing, the way the dialogs of the system hold them.
 */
static void test_picker_buttons_ride_the_right(void) {
  int i = 0;

  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct picker_layout g;
    int const width = layout_scale(ADC_PICKER_W, dpi);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    int const gap = layout_scale(ADC_LAYOUT_GAP, dpi);
    int const button_w = layout_scale(ADC_PICKER_BUTTON_W, dpi);
    int const button_h = layout_scale(ADC_PICKER_BUTTON_H, dpi);
    int const row = layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi) + layout_scale(ADC_PICKER_LABEL_H, dpi) + gap +
                    layout_scale(ADC_PICKER_COMBO_H, dpi) + gap;

    picker_compute(width, dpi, &g);
    TEST_CHECK(g.ok.top == row);
    TEST_CHECK(g.cancel.top == row);
    TEST_CHECK(g.ok.bottom == row + button_h);
    TEST_CHECK(g.cancel.bottom == row + button_h);
    TEST_CHECK_(g.cancel.right == width - margin,
                "dpi %u: cancel ends at %d, the margin ends at %d",
                (unsigned)dpi,
                (int)g.cancel.right,
                (int)(width - margin));
    TEST_CHECK_(g.ok.right + gap == g.cancel.left,
                "dpi %u: ok ends at %d, cancel starts at %d",
                (unsigned)dpi,
                (int)g.ok.right,
                (int)g.cancel.left);
    TEST_CHECK(g.ok.right - g.ok.left == button_w);
    TEST_CHECK(g.cancel.right - g.cancel.left == button_w);
    TEST_CHECK_(g.ok.left > margin, "dpi %u: the pair starts at %d, the margin ends at %d", (unsigned)dpi, (int)g.ok.left, (int)margin);
  }
}

/**
 * @brief The width of the picker holds the pair of buttons
 */
static void test_picker_holds_its_buttons(void) {
  TEST_CHECK_(ADC_PICKER_W >= 2 * ADC_PICKER_BUTTON_W + ADC_LAYOUT_GAP + 2 * ADC_LAYOUT_MARGIN_X,
              "the width %d, the pair and its margins need %d",
              (int)ADC_PICKER_W,
              (int)(2 * ADC_PICKER_BUTTON_W + ADC_LAYOUT_GAP + 2 * ADC_LAYOUT_MARGIN_X));
}

/**
 * @brief A window of a test shows nothing and takes no press
 *
 * A window that is drawn is in the way of the reader, and one that takes a press takes the
 * press that was meant for something else.  The opacity is read back, because a layered
 * window that was never given one is drawn opaque on some systems.
 */
static void test_window_of_a_test_shows_nothing(void) {
  struct ov_error err = {0};
  ATOM atom = 0;
  HWND window = NULL;
  WNDCLASSEXW wc;
  BYTE alpha = 255;

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.lpszClassName = L"adc_test_window_invisible";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  window = CreateWindowExW(0, wc.lpszClassName, L"window", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(window != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(test_suppress_opacity(window, &alpha, &err), &err);
  TEST_CHECK_(alpha == 0, "want a window that shows nothing, got an opacity of %u", (unsigned)alpha);
  TEST_CHECK((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);
  TEST_MSG("want the window to let the mouse through to what is under it");

cleanup:
  if (window != NULL) {
    DestroyWindow(window);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
}

TEST_LIST = {
    {"window_of_a_test_shows_nothing", test_window_of_a_test_shows_nothing},
    {"picker_buttons_ride_the_right", test_picker_buttons_ride_the_right},
    {"picker_holds_its_buttons", test_picker_holds_its_buttons},
    {"device_picker_label", test_device_picker_label},
    {"picker_window_names_every_unit", test_picker_window_names_every_unit},
    {NULL, NULL},
};
