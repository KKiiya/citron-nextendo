# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(web_service STATIC
    announce_room_json.cpp
    announce_room_json.h
    game_catalog.cpp
    game_catalog.h
    game_catalog_manifest.cpp
    game_catalog_manifest.h
    nextendo_api.cpp
    nextendo_api.h
    npln_patch_catalog.cpp
    npln_patch_catalog.h
    npln_patch_manifest.cpp
    npln_patch_manifest.h
    npln_patch_state.h
    population_scraper_client.cpp
    population_scraper_client.h
    precompiled_headers.h
    ssbu_mod_installer.cpp
    ssbu_mod_installer.h
    verify_login.cpp
    verify_login.h
    verify_user_jwt.cpp
    verify_user_jwt.h
    web_backend.cpp
    web_backend.h
    web_result.h
)

create_target_directory_groups(web_service)
target_link_libraries(web_service PRIVATE common network nlohmann_json::nlohmann_json httplib::httplib OpenSSL::Crypto)

if (CITRON_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(web_service PRIVATE precompiled_headers.h)
endif()

citron_configure_lto(web_service)
