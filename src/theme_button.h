#ifndef ADC_THEME_BUTTON_H
#define ADC_THEME_BUTTON_H

#include <windows.h>

/**
 * Painting of a push button.  Internal to the theme implementation, see theme_internal.h.
 */

/**
 * @brief Take over the painting of a push button
 *
 * The caption of a themed push button is drawn with DrawThemeText, which reads its colour
 * from the theme definition and ignores SetTextColor, and the face of the explorer theme
 * is light in every state.  With the dark palette the whole button is therefore painted by
 * this component, with the light palette nothing happens and the theme keeps control.
 *
 * @param button the button to attach to, NULL is allowed
 */
void theme_button_attach(HWND const button);

/**
 * @brief Give the painting of a push button back to comctl32
 *
 * @param button the button to detach from, NULL is allowed
 */
void theme_button_detach(HWND const button);

#endif /* ADC_THEME_BUTTON_H */
