// pebble_tsys.h -- Pebble-specific Tsys backed by the SDK heap.
#ifndef PEBBLE_TSYS_H
#define PEBBLE_TSYS_H

#include "common/tsys.h"

// Call once before any VM allocation.
Tsys pebble_tsys_init(void);

#endif
