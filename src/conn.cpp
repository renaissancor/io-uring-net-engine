#include "conn.h"

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
