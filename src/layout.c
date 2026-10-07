#include "layout.h"

#include <string.h>

/**
 * @brief A rectangle from its left, top and size
 *
 * @param left the left edge
 * @param top the top edge
 * @param width how wide the rectangle is
 * @param height how tall the rectangle is
 * @return the rectangle
 */
static RECT rect_of(int const left, int const top, int const width, int const height) {
  RECT r;
  r.left = left;
  r.top = top;
  r.right = left + width;
  r.bottom = top + height;
  return r;
}

/* The shares of the list width the columns take.  The columns are shares of the usable
   list width, they add up to ADC_LAYOUT_COLUMN_TOTAL.  An absolute width would not follow
   the window size. */
static int const column_share[ADC_LAYOUT_COLUMN_COUNT] = ADC_LAYOUT_COLUMN_SHARE;

/**
 * @brief Compute the geometry of the progress window
 *
 * @param width the client width of the window in pixels
 * @param dpi   the dpi of the window
 * @param out   receives the geometry
 */
void layout_progress_compute(int const width, UINT const dpi, struct progress_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const usable = (width > 2 * margin) ? (width - 2 * margin) : 0;

  memset(out, 0, sizeof(*out));
  out->label = rect_of(margin, layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi), usable, layout_scale(ADC_LAYOUT_STATUS_H, dpi));
  out->bar = rect_of(margin, out->label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi), usable, layout_scale(ADC_LAYOUT_PROGRESS_BAR_H, dpi));
  out->cancel = rect_of(margin + (usable - layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_W, dpi)) / 2,
                        out->bar.bottom + layout_scale(ADC_LAYOUT_GAP, dpi),
                        layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_W, dpi),
                        layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_H, dpi));
  out->needed_height = out->cancel.bottom + layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi);
}

/**
 * @brief Turn one logical value into physical pixels for the given dpi
 *
 * @param logical the value in logical units (1/96 inch)
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the value in physical pixels
 */
int layout_scale(int const logical, UINT const dpi) {
  static UINT const base = USER_DEFAULT_SCREEN_DPI;
  UINT const d = (dpi != 0) ? dpi : base;
  return MulDiv(logical, (int)d, (int)base);
}

/**
 * @brief The width of the settings group, measured or its minimum
 *
 * @param measured the width the caller measured, in physical pixels, 0 when there is none
 * @param dpi      the dpi of the window
 * @return the width the group is drawn with, in physical pixels
 */
static int group_min_width(int const measured, UINT const dpi) {
  return (measured > 0) ? measured : layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi);
}

/**
 * @brief The narrowest client area that shows both groups of the band side by side
 *
 * @param settings_min_w the width the settings group needs, in physical pixels
 * @param dpi            the dpi of the window
 * @return the width in physical pixels
 */
int layout_min_client_width(int const settings_min_w, UINT const dpi) {
  return group_min_width(settings_min_w, dpi) + layout_scale(ADC_LAYOUT_MIN_CLIENT_EXTRA_W, dpi);
}

/**
 * @brief The outer rectangle of a window whose client area has one fixed size
 *
 * @param client_width  the width of the client area in physical pixels
 * @param client_height the height of the client area in physical pixels
 * @param dpi           the dpi of the window, 0 falls back to the base of 96
 * @param style         the window style the frame is drawn with
 * @param ex_style      the extended window style of the same frame
 * @param out           receives the outer rectangle, its top left corner at the origin
 */
void layout_window_rect(
    int const client_width, int const client_height, UINT const dpi, DWORD const style, DWORD const ex_style, RECT *const out) {
  SetRect(out, 0, 0, client_width, client_height);
  if (!AdjustWindowRectExForDpi(out, style, FALSE, ex_style, dpi)) {
    /* the margins of a plain window stand in for a frame that could not be asked for */
    int const side = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    int const caption = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
    SetRect(out, -side, -caption, client_width + side, client_height + caption);
  }
}

/**
 * @brief The height argument of CreateFontW, which is negative for a character height
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
int layout_font_height(UINT const dpi) {
  UINT const d = (dpi != 0) ? dpi : USER_DEFAULT_SCREEN_DPI;
  return -MulDiv(ADC_LAYOUT_FONT_POINT, (int)d, 72);
}

/**
 * @brief The font the controls of a window draw with, made for one dpi
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the font, NULL when it could not be made; the caller deletes it
 */
HFONT layout_make_font(UINT const dpi) {
  return CreateFontW(layout_font_height(dpi),
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
}

/**
 * @brief The two boxes of a group: the frame, and the caption that rides on its top line
 *
 * The caption sits at the top of the group and the top line of the frame runs
 * through the middle of the caption text, the arrangement a group box of the
 * system draws on its own.  A group the application themes itself therefore
 * looks the same as one the system draws, and the offset of the line is derived
 * from the height of the caption, so it holds at every dpi.
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
 * @param caption_height the measured height of a caption text in pixels, taken
 *                       from the font of the window: the top line of a group
 *                       frame runs through the middle of the caption, the way a
 *                       group box of the system draws its own.  0 falls back to
 *                       the room the layout reserves for a caption.
 * @param out receives the geometry
 */
void layout_compute(RECT const *const client,
                    UINT const dpi,
                    int const settings_min_w,
                    int const settings_caption_width,
                    int const install_caption_width,
                    int const caption_height,
                    struct layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const reserved = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
  int const caption_h = (caption_height > 0) ? caption_height : reserved;
  int const band_bottom = (client->bottom - client->top) - layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi);
  int const frame_top = band_bottom - layout_scale(ADC_LAYOUT_GROUP_H, dpi);
  int const group_top = frame_top - caption_h / 2;
  int const button_top = layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi);
  int const inset = layout_scale(ADC_LAYOUT_GROUP_INSET, dpi);
  int settings_w = group_min_width(settings_min_w, dpi);
  int install_left = margin + settings_w + layout_scale(ADC_LAYOUT_GROUP_GAP, dpi);
  int install_w = (client->right - client->left) - margin - install_left;
  int min_client_width = layout_min_client_width(settings_min_w, dpi);
  int const task_top = frame_top + inset;

  if (install_w < inset * 2) {
    install_w = inset * 2;
  }
  memset(out, 0, sizeof(*out));
  out->button = rect_of(margin, button_top, layout_scale(ADC_LAYOUT_BUTTON_W, dpi), layout_scale(ADC_LAYOUT_BUTTON_H, dpi));
  group_box(&out->settings_frame,
            &out->settings_caption,
            margin,
            group_top,
            settings_w,
            layout_scale(ADC_LAYOUT_GROUP_H, dpi),
            caption_h,
            settings_caption_width,
            margin + inset);
  group_box(&out->install_frame,
            &out->install_caption,
            install_left,
            group_top,
            install_w,
            layout_scale(ADC_LAYOUT_GROUP_H, dpi),
            caption_h,
            install_caption_width,
            install_left + inset);
  out->check_mixer = rect_of(margin + inset, frame_top + inset, settings_w - 2 * inset, layout_scale(ADC_LAYOUT_CHECK_H, dpi));
  out->check_audiodg = rect_of(margin + inset,
                               frame_top + inset + layout_scale(ADC_LAYOUT_CHECK_H + ADC_LAYOUT_GAP, dpi),
                               settings_w - 2 * inset,
                               layout_scale(ADC_LAYOUT_CHECK_H, dpi));
  {
    int const last_bottom = out->check_audiodg.bottom;
    int const shift = ((out->settings_frame.bottom - last_bottom) - (out->check_mixer.top - out->settings_frame.top)) / 2;
    if (shift != 0) {
      out->check_mixer.top += shift;
      out->check_mixer.bottom += shift;
      out->check_audiodg.top += shift;
      out->check_audiodg.bottom += shift;
    }
  }
  out->task_install = rect_of(install_left + inset, task_top, install_w - 2 * inset, layout_scale(ADC_LAYOUT_TASK_H, dpi));
  out->task_remove = rect_of(install_left + inset,
                             task_top + layout_scale(ADC_LAYOUT_TASK_H + ADC_LAYOUT_TASK_GAP, dpi),
                             install_w - 2 * inset,
                             layout_scale(ADC_LAYOUT_TASK_H, dpi));
  out->min_client_width = min_client_width;
}

/**
 * @brief Compute the geometry of the progress window
 *
 * @param width  the client width of the window in pixels
 * @param height the client height of the window in pixels
 * @param dpi    the dpi of the window
 * @param out    receives the geometry
 */
void layout_blockers_compute(int const width, int const height, UINT const dpi, struct blockers_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const ok_h = layout_scale(ADC_LAYOUT_BLOCKERS_OK_H, dpi);
  int const label_bottom = layout_scale(ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_BLOCKERS_LABEL_H + ADC_LAYOUT_GAP, dpi);
  int const list_bottom = height - layout_scale(ADC_LAYOUT_MARGIN_BOTTOM + ADC_LAYOUT_BLOCKERS_OK_H + ADC_LAYOUT_GAP, dpi);
  int const list_usable = width - 2 * margin - layout_scale(ADC_LAYOUT_SCROLL_W, dpi);
  int used = 0;
  int i = 0;

  memset(out, 0, sizeof(*out));
  if ((width < 2 * margin) || (list_bottom < label_bottom)) {
    return;
  }
  out->label =
      rect_of(margin, layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi), width - 2 * margin, layout_scale(ADC_LAYOUT_BLOCKERS_LABEL_H, dpi));
  out->list = rect_of(margin, label_bottom, width - 2 * margin, list_bottom - label_bottom);
  out->ok = rect_of(width - margin - layout_scale(ADC_LAYOUT_BLOCKERS_OK_W, dpi),
                    list_bottom + layout_scale(ADC_LAYOUT_GAP, dpi),
                    layout_scale(ADC_LAYOUT_BLOCKERS_OK_W, dpi),
                    ok_h);
  out->header_height = layout_scale(ADC_LAYOUT_HEADER_H, dpi);
  if (list_usable > 0) {
    for (i = 0; i < ADC_LAYOUT_COLUMN_COUNT - 1; i++) {
      out->column[i] = MulDiv(list_usable, column_share[i], ADC_LAYOUT_COLUMN_TOTAL);
      used += out->column[i];
    }
    out->column[ADC_LAYOUT_COLUMN_COUNT - 1] = list_usable - used;
  }
}
