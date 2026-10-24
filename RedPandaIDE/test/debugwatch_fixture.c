#include <windows.h>

volatile int watch_value = 10;
volatile int keep_running = 1;
volatile unsigned int ticks = 0;

void debugwatch_tick(void)
{
    ++ticks; /* BP_TICK */
    watch_value += 0; /* BP_VALUE */
}

int main(void)
{
    while (keep_running) {
        debugwatch_tick();
        Sleep(20);
    }
    return 0;
}
