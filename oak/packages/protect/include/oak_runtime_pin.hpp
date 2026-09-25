#pragma once

// SHA-256 (lowercase hex) of the production runtime package SPKI DER.
// Must match API config.runtimePackageKeyId. Empty = unpinned (dev / until set).
// Override at build time:
//   -DOAK_RUNTIME_SPKI_PIN=\"abcdef...\"
#ifndef OAK_RUNTIME_SPKI_PIN
#define OAK_RUNTIME_SPKI_PIN ""
#endif
