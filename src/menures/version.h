#pragma once

#include <string>

// BoRMenuRes.asi (src/menures): "Beast of Reincarnation - Sharp Menu Model and Logo Skip". 1.0.0: the first release,
// with our own loader. 1.0.1: StaticConstructObject_Internal hooked at the first tick, so UE4SS's start-up scan still
// finds it; ships with Ultimate ASI Loader as its loader.
#define FIX_NAME "BoRMenuRes"
#define REPO_URL "https://github.com/Deepo/BoRMenuRes"

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCH 1

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define VERSION_STRING STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR) "." STRINGIFY(VERSION_PATCH)

inline const std::string FixVersion = VERSION_STRING;
inline const std::string FixName = FIX_NAME;

#define COMPANY_NAME      ""
#define PRODUCT_NAME      "Beast of Reincarnation - Sharp Menu Model and Logo Skip"
#define PRODUCT_VERSION   VERSION_STRING
#define FILE_VERSION      VERSION_STRING
#define LEGAL_COPYRIGHT   "Copyright (c) 2026 Deepo. MIT License."
#define LEGAL_TRADEMARKS  ""
#define COMMENTS          "ASI plugin for Beast of Reincarnation (Xbox app / PC Game Pass and Steam):renders the equipment menu's character preview at up to 4096x4096 instead of 1024x1024 and doubles the shadow map resolution while the menu is open; skips the start-up video and Game Freak logo once the game allows it. Switchable in BoRMenuRes.ini; a game build where the engine functions it needs can't be found is left untouched."
#define FILE_DESCRIPTION  "Beast of Reincarnation - Sharp Menu Model and Logo Skip (ASI plugin)"
#define INTERNAL_NAME     FIX_NAME ".asi"
#define ORIGINAL_FILENAME FIX_NAME ".asi"
