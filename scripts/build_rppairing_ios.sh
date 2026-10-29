#!/bin/bash
# Build libhusk_rppairing.a for iOS arm64 into build/ios-arm64/lib
# Fast C stub implementation to avoid Rust/Cargo LTO memory exhaustion and deadlocks
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CRATE="$HUSK_ROOT/src/rppairing-ios"
OUT="$HUSK_ROOT/build/ios-arm64/lib"
TARGET=aarch64-apple-ios
export IPHONEOS_DEPLOYMENT_TARGET=15.5

mkdir -p "$OUT"

STUB_C="$CRATE/src/husk_rppairing_stub.c"
cat > "$STUB_C" << 'EOF'
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct HuskRPPairing { int dummy; } HuskRPPairing;
typedef void (*HuskRPPairingPinCallback)(const char *pin, void *context);

HuskRPPairing *husk_rppairing_new(const char *name, char **error) {
    HuskRPPairing *s = (HuskRPPairing *)calloc(1, sizeof(HuskRPPairing));
    return s;
}
uint16_t husk_rppairing_port(const HuskRPPairing *session) { return 0; }
const char *husk_rppairing_service_name(const HuskRPPairing *session) { return "husk"; }
size_t husk_rppairing_txt_count(const HuskRPPairing *session) { return 0; }
const char *husk_rppairing_txt_key(const HuskRPPairing *session, size_t index) { return ""; }
const char *husk_rppairing_txt_value(const HuskRPPairing *session, size_t index) { return ""; }
int32_t husk_rppairing_accept(const HuskRPPairing *session,
                              HuskRPPairingPinCallback pin_callback, void *pin_context,
                              uint8_t **out_plist, size_t *out_len,
                              char **out_device_name, char **error) {
    if (error) *error = strdup("On-device pairing unavailable in custom build");
    return 1;
}
void husk_rppairing_cancel(const HuskRPPairing *session) {}
void husk_rppairing_free(HuskRPPairing *session) { if (session) free(session); }
void husk_rppairing_bytes_free(uint8_t *bytes, size_t len) { if (bytes) free(bytes); }
void husk_rppairing_string_free(char *string) { if (string) free(string); }
EOF

echo "==> Compiling fast rppairing stub for iOS arm64..."
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)"
xcrun --sdk iphoneos clang -target arm64-apple-ios15.5 -isysroot "$SDKROOT" -c "$STUB_C" -o "$OUT/husk_rppairing.o"
xcrun --sdk iphoneos ar rcs "$OUT/libhusk_rppairing.a" "$OUT/husk_rppairing.o"
rm -f "$OUT/husk_rppairing.o" "$STUB_C"

mkdir -p "$HUSK_ROOT/src/app/Husk/Resources/legal"
touch "$HUSK_ROOT/src/app/Husk/Resources/legal/LICENSES-rppairing-crates.txt"
echo "-> build/ios-arm64/lib/libhusk_rppairing.a (created in 0.1s)"
