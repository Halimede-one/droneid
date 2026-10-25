#pragma once
#include "uart_guard.h"
// Small boot-time smoke test; no hardware I/O and no changes to live policy.
// It complements, but does not replace, host adversarial and wired tests.
bool guard_selftest(void);
