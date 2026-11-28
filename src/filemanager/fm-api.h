// Selects the file manager's extension API for the shared plugin source (#1105).
#pragma once
#if defined(PUTGD_FM_NAUTILUS)
#include "nautilus-api.h"
#elif defined(PUTGD_FM_CAJA)
#include "caja-api.h"
#else
#error "Define PUTGD_FM_NAUTILUS or PUTGD_FM_CAJA"
#endif

#include "encrypted-entry.h"

#define PUTGD_FM_LAUNCHER "/usr/bin/putgd-launcher"
#define PUTGD_FM_LABEL "Upload with put.gd"
#define PUTGD_FM_ENCRYPTED_LABEL "Upload encrypted with put.gd" // #2119
#define PUTGD_FM_ICON "putgd"
