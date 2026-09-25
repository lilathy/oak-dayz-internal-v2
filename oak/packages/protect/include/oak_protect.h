#pragma once

// Shared Oak commercial-protection SDK for all product clients.
// Keep feature/render code outside this boundary.

#include <cstdint>

#ifndef OAK_PRODUCT_SLUG
#define OAK_PRODUCT_SLUG "dayz"
#endif

bool OakProtectionAuthorize();
bool OakProtectionIsAuthorized();
void OakProtectionShutdown();

// Opaque build marker used for leak attribution. The mutate tool rewrites this
// blob per release without changing loader/inject timing.
extern "C" volatile char g_OakBuildMark[80];

// Cheap integrity probe used by scattered feature gates.
std::uint32_t OakProtectIntegrityTag();
