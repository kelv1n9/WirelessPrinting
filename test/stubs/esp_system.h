#pragma once
#include <setjmp.h>
extern jmp_buf testRestartJump;
extern int testRestarts;
[[noreturn]] inline void esp_restart() { ++testRestarts; longjmp(testRestartJump, 1); }
