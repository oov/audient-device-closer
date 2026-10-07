#include <windows.h>

/**
 * @brief A process the tests can own and kill: it just waits
 *
 * @return never returns
 */
int main(void) {
  for (;;) {
    Sleep(600000);
  }
}
