// Selects the file manager's extension API for the shared plugin source (#1105).
#pragma once
#if defined(PUTGD_FM_NAUTILUS)
#include "nautilus-api.h"
#elif defined(PUTGD_FM_CAJA)
#include "caja-api.h"
#else
#error "Define PUTGD_FM_NAUTILUS or PUTGD_FM_CAJA"
#endif

#define PUTGD_FM_LAUNCHER "/usr/bin/putgd-launcher"
#define PUTGD_FM_LABEL "Upload with put.gd"
#define PUTGD_FM_ICON "putgd"
