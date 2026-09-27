#pragma once

// ============================================================================
// T-Deck Pro E-Paper GPIO configuration
// ============================================================================
//
// DO NOT guess these values.
//
// Populate these from the T-Deck Pro board definition/schematic that matches
// your exact hardware revision.
//
// Once confirmed, the rest of EinkDisplay.cpp does not need to change.
// ============================================================================

#define TDECK_PRO_EPD_CS    34
#define TDECK_PRO_EPD_DC    35
#define TDECK_PRO_EPD_RST   -1
#define TDECK_PRO_EPD_BUSY  37