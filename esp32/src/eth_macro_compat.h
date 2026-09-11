#pragma once

// Arduino-ESP32 ETH.h defines the generic identifiers below as macros.
// SunBridge intentionally uses its own WT32-ETH01 values in main.cpp, so
// include ETH.h once here and remove only those configuration macros before
// main.cpp is parsed. The ETH enums/classes remain available through ETH.h.
#include <ETH.h>

#ifdef ETH_PHY_ADDR
#undef ETH_PHY_ADDR
#endif
#ifdef ETH_PHY_POWER
#undef ETH_PHY_POWER
#endif
#ifdef ETH_MDC_PIN
#undef ETH_MDC_PIN
#endif
#ifdef ETH_MDIO_PIN
#undef ETH_MDIO_PIN
#endif
#ifdef ETH_CLK_MODE
#undef ETH_CLK_MODE
#endif
#ifdef ETH_PHY_TYPE
#undef ETH_PHY_TYPE
#endif
