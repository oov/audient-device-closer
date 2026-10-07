#ifndef ADC_LAYOUT_H
#define ADC_LAYOUT_H

#include <windows.h>

/**
 * @brief Geometry of the main window, in logical units
 *
 * A logical unit is 1/96 of an inch, that is the value Windows reports as the base dpi
 * (USER_DEFAULT_SCREEN_DPI).  Nothing in the application may place a control in physical
 * pixels: physical sizes only exist after layout_scale() turned a logical value into
 * pixels for the dpi of the window.
 */
enum adc_layout {
  ADC_LAYOUT_MARGIN_X = 12,      /* left and right of the client area */
  ADC_LAYOUT_MARGIN_TOP = 10,    /* first control to the top edge */
  ADC_LAYOUT_MARGIN_BOTTOM = 12, /* the group band to the bottom edge */
  ADC_LAYOUT_GAP = 8,            /* default distance between two controls */
  ADC_LAYOUT_SECTION_GAP = 20,   /* the button to the group band: wider than GAP */
  ADC_LAYOUT_GROUP_GAP = 12,     /* between the two groups of the band */

  ADC_LAYOUT_BUTTON_H = 30,
  ADC_LAYOUT_STATUS_H = 20, /* the height of a one-line label (the progress window uses it) */
  ADC_LAYOUT_BUTTON_W = 280,
  ADC_LAYOUT_CHECK_H = 22,
  /* The room a check box caption needs beyond its text: the box of the control and the gap
     to the caption.  The box belongs to comctl32 and is as tall as the text; these are the
     values it uses at 96dpi. */
  ADC_LAYOUT_CHECK_BOX_W = 16,
  ADC_LAYOUT_CHECK_BOX_GAP = 4,

  /* The two groups of the band share one top line.  The height is the room the taller
     of them needs, so their frames line up along the top and the bottom edge. */
  ADC_LAYOUT_GROUP_INSET = 12, /* control to the group frame */
  ADC_LAYOUT_GROUP_H = 88,     /* 2 * INSET + 2 * TASK_H + TASK_GAP */
  ADC_LAYOUT_TASK_W = 220,
  ADC_LAYOUT_TASK_H = 26,
  ADC_LAYOUT_TASK_GAP = 12, /* between the two task buttons */
  /* The width the settings group never goes below.  Its real width is measured from its own
     captions at run time, see layout_compute: a translation can be longer than the English
     text and a language can bring a font of its own, so a number here could only be right by
     accident. */
  ADC_LAYOUT_SETTINGS_MIN_W = 300,
  ADC_LAYOUT_CAPTION_H = 16,  /* the room reserved for a caption when the font could not be asked */
  ADC_LAYOUT_CAPTION_PAD = 8, /* slack on top of the measured text width */

  ADC_LAYOUT_MIN_CLIENT_EXTRA_W = 280, /* beside the settings group: 2 * MARGIN_X + GROUP_GAP + 2 * INSET + TASK_W */

  ADC_LAYOUT_WINDOW_W = ADC_LAYOUT_SETTINGS_MIN_W + ADC_LAYOUT_MIN_CLIENT_EXTRA_W, /* the two groups side by side */
  ADC_LAYOUT_WINDOW_H = ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_BUTTON_H + ADC_LAYOUT_SECTION_GAP + ADC_LAYOUT_CAPTION_H / 2 +
      ADC_LAYOUT_GROUP_H + ADC_LAYOUT_MARGIN_BOTTOM, /* the button over the band */

  /* The window that shows the progress of a run: the stage label, the progress bar under it
     and the button that asks the run to stop.  Its least width is below; the window grows to
     the widest line the run can report, so no report is cut off.  Its height is no number
     here, layout_progress_compute() reports the room its stack needs. */
  ADC_LAYOUT_PROGRESS_W = 320,       /* the least width the status line reads in */
  ADC_LAYOUT_PROGRESS_BAR_H = 23,    /* the progress bar of comctl32 at 96dpi */
  ADC_LAYOUT_PROGRESS_CANCEL_H = 26, /* the button that asks the run to stop */
  ADC_LAYOUT_PROGRESS_CANCEL_W = 120,

  /* The window that shows the processes that block the close.  It holds the sentence
     that says why the close did not go through, the list of the veto records under it
     and the button that takes the window down.  Unlike the main window it is resizable:
     the list is the point of the window, and the reader makes it larger to read a long
     path. */
  ADC_LAYOUT_BLOCKERS_W = 560, /* initial client size of the blockers window */
  ADC_LAYOUT_BLOCKERS_H = 320,
  ADC_LAYOUT_BLOCKERS_MIN_W = 320, /* the smallest client area the list stays readable in */
  ADC_LAYOUT_BLOCKERS_MIN_H = 200,
  ADC_LAYOUT_BLOCKERS_LABEL_H = 20, /* the sentence that names the outcome of the run */
  ADC_LAYOUT_BLOCKERS_OK_H = 30,    /* the button that takes the window down */
  ADC_LAYOUT_BLOCKERS_OK_W = 120,
  ADC_LAYOUT_SCROLL_W = 17, /* width of the vertical scroll bar of the list */
  /* comctl32 grows the header only with the font height when the dpi changes, it
     does not scale it, so the height of the header is a layout value. */
  ADC_LAYOUT_HEADER_H = 24,

  ADC_LAYOUT_FONT_POINT = 9, /* font size in points, at 96dpi */

  /* Painting details of the themed components (theme_*.c).  They are dimensions like
     every other one: no component draws from a literal, all of them scale with dpi. */
  ADC_LAYOUT_FRAME_LINE = 1,           /* hairline: group frame and header divider */
  ADC_LAYOUT_FRAME_BREAK = 4,          /* slack between the caption and the frame line */
  ADC_LAYOUT_HEADER_TEXT_L = 8,        /* left inset of a header caption */
  ADC_LAYOUT_HEADER_TEXT_R = 4,        /* right inset of a header caption */
  ADC_LAYOUT_HEADER_DIVIDER_INSET = 4, /* vertical inset of a column divider */
  ADC_LAYOUT_BUTTON_EDGE = 1,          /* border width of a push button */
  ADC_LAYOUT_BUTTON_CORNER = 5,        /* corner radius of a push button */
  ADC_LAYOUT_BUTTON_SHIELD = 16,       /* side of the elevation shield */
  ADC_LAYOUT_BUTTON_ICON_GAP = 8,      /* shield to caption */
  ADC_LAYOUT_BUTTON_MARGIN = 12        /* caption margin inside a push button */
};

/* The columns are shares of the usable list width, they add up to the total below.
   An absolute width would not follow the window size.  The order of the columns is
   PID, process, device: the pid and the image name of the same process are read as one
   piece of information, and the device of the record is what one looks at last. */
#define ADC_LAYOUT_COLUMN_COUNT 3
#define ADC_LAYOUT_COLUMN_SHARE {100, 575, 325}
#define ADC_LAYOUT_COLUMN_TOTAL 1000

/**
 * @brief Every rectangle of the main window for one client size and dpi
 *
 * All values are physical pixels in the client coordinates of the window.  The whole
 * geometry of the application is computed here, the window procedure only hands these
 * rectangles to MoveWindow(), which is what makes the layout testable without a screen.
 */
struct layout {
  RECT button;           /* the button that closes the device */
  RECT check_mixer;      /* close the mixer automatically */
  RECT check_audiodg;    /* close audiodg automatically */
  RECT settings_frame;   /* the line around the check boxes */
  RECT settings_caption; /* its caption, riding on the frame line */
  RECT install_frame;    /* the line around the task buttons */
  RECT install_caption;  /* its caption, riding on the frame line */
  RECT task_install;     /* register the scheduled task */
  RECT task_remove;      /* remove the scheduled task */
  int min_client_width;  /* the narrowest client area that shows both groups */
};

/**
 * @brief Compute the geometry of the main window
 *
 * @param client                  the client rectangle of the window
 * @param dpi                     the dpi of the window
 * @param settings_min_w          the width the settings group needs in pixels, measured by
 *                                the caller from the captions of its check boxes.  The two
 *                                groups are side by side, so the window cannot become
 *                                narrower than this plus ADC_LAYOUT_MIN_CLIENT_EXTRA_W.
 * @param settings_caption_width  the measured width of the settings caption in pixels
 * @param install_caption_width   the measured width of the install caption in pixels
 * @param caption_height          the measured height of a caption text in pixels,
 *                                taken from the font of the window.  The top line
 *                                of a group frame runs through the middle of the
 *                                caption, the way a group box of the system draws
 *                                its own, so the height decides where the line
 *                                goes.  0 falls back to the room the layout
 *                                reserves for a caption.
 */
void layout_compute(RECT const *const client,
                    UINT const dpi,
                    int const settings_min_w,
                    int const settings_caption_width,
                    int const install_caption_width,
                    int const caption_height,
                    struct layout *const out);

/**
 * @brief One rectangle of the window that shows the blocking processes
 *
 * The window holds the sentence that says why the close did not go through, the list of
 * the veto records under it and the button that takes it down.
 */
struct blockers_layout {
  RECT label;                          /* the sentence above the list */
  RECT list;                           /* the list view, under the label */
  RECT ok;                             /* the button that takes the window down */
  int column[ADC_LAYOUT_COLUMN_COUNT]; /* widths of the list columns */
  int header_height;                   /* the header of the list view */
};

/**
 * @brief Compute the geometry of the window that shows the blocking processes
 *
 * The label sits at the top, the list fills the window under it and above the button;
 * the window is resizable and the layout follows every client size it is given.
 *
 * @param width  the client width of the window in pixels
 * @param height the client height of the window in pixels
 * @param dpi    the dpi of the window
 * @param out    receives the geometry
 */
void layout_blockers_compute(int const width, int const height, UINT const dpi, struct blockers_layout *const out);

/**
 * @brief Every rectangle of the progress window for one width and dpi
 *
 * All values are physical pixels in the client coordinates of the progress window.  The
 * window holds the stage label and, under it, the progress bar, and nothing else.
 */
struct progress_layout {
  RECT label;        /* the text of the stage the run is at */
  RECT bar;          /* the progress bar under it */
  RECT cancel;       /* the button that asks the run to stop */
  int needed_height; /* the client height the stack needs, the window is made that tall */
};

/**
 * @brief Compute the geometry of the progress window
 *
 * The stack is placed from the top edge; out reports the client height it needs.
 *
 * @param width the client width of the window in pixels
 * @param dpi   the dpi of the window
 * @param out   receives the geometry and the height it needs
 */
void layout_progress_compute(int const width, UINT const dpi, struct progress_layout *const out);

/**
 * @brief Turn one logical value into physical pixels for the given dpi
 *
 * @param dpi  the dpi of the window, 0 falls back to the base of 96.
 */
int layout_scale(int const logical, UINT const dpi);

/**
 * @brief The narrowest client area that shows both groups of the band side by side
 *
 * @param settings_min_w the width the settings group needs, in physical pixels, 0 for its minimum
 * @param dpi            the dpi of the window
 * @return the width in physical pixels
 */
int layout_min_client_width(int settings_min_w, UINT dpi);

/**
 * @brief The outer rectangle of a window with one client size
 *
 * The only place that turns a client size into the size the window manager is told.
 *
 * @param client_width  the width of the client area in physical pixels
 * @param client_height the height of the client area in physical pixels
 * @param dpi           the dpi of the window, 0 falls back to the base of 96
 * @param style         the window style the frame is drawn with
 * @param ex_style      the extended window style of the same frame
 * @param out           receives the outer rectangle, its top left corner at the origin
 */
void layout_window_rect(int client_width, int client_height, UINT dpi, DWORD style, DWORD ex_style, RECT *out);

/**
 * @brief The height argument of CreateFontW, which is negative for a character height
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
int layout_font_height(UINT const dpi);

/**
 * @brief The font the controls of a window draw with, made for one dpi
 *
 * Every window owns its font and deletes it with the window.
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the font, NULL when it could not be made; the caller deletes it
 */
HFONT layout_make_font(UINT const dpi);

#endif /* ADC_LAYOUT_H */
