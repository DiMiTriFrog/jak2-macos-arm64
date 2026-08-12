#include "term_util.h"

#include "common/platform/BuildConfig.h"

#if defined _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <condition_variable>
#include <mutex>
#elif (defined(__LINUX__) || defined(__gnu_linux__) || defined(__linux__) || defined(__APPLE__)) && \
    !OG_TARGET_IOS
#include <cstdlib>
#include <stdio.h>
#include <unistd.h>

#include <sys/ioctl.h>
#endif

namespace term_util {
void clear() {
#if defined _WIN32
  system("cls");
#elif (defined(__LINUX__) || defined(__gnu_linux__) || defined(__linux__) || defined(__APPLE__)) && \
    !OG_TARGET_IOS
  system("clear");
#elif OG_TARGET_IOS
  // There is no terminal on iOS; keep this shared helper a no-op.
  return;
#endif
}

int row_count() {
#if defined _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
  return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#elif (defined(__LINUX__) || defined(__gnu_linux__) || defined(__linux__) || defined(__APPLE__)) && \
    !OG_TARGET_IOS
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  return w.ws_row;
#elif OG_TARGET_IOS
  return 0;
#endif
}

int col_count() {
#if defined _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
  return csbi.srWindow.Right - csbi.srWindow.Left + 1;
#elif (defined(__LINUX__) || defined(__gnu_linux__) || defined(__linux__) || defined(__APPLE__)) && \
    !OG_TARGET_IOS
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  return w.ws_col;
#elif OG_TARGET_IOS
  return 0;
#endif
}
}  // namespace term_util
