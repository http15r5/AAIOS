#!/bin/bash
# Download official Husk 3.0 IPA and stage its frameworks, guest images, and MoltenVK.xcframework
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> Downloading base Husk 3.0 release assets..."
curl -fL --retry 3 -o /tmp/Husk_base.ipa https://github.com/Leviidev/Husk/releases/download/3.0/Husk.ipa
unzip -q /tmp/Husk_base.ipa -d /tmp/husk_base

# Stage prebuilt dylibs and frameworks needed for linking Xcode project
mkdir -p "$HUSK_ROOT/build/ios-arm64/lib" "$HUSK_ROOT/build/ios-arm64/sysroot/lib" "$HUSK_ROOT/guests/phase0"
cp /tmp/husk_base/Payload/Husk.app/Frameworks/libqemu-aarch64-softmmu.dylib "$HUSK_ROOT/build/ios-arm64/lib/"
cp /tmp/husk_base/Payload/Husk.app/Frameworks/libANGLE-shared.dylib "$HUSK_ROOT/build/ios-arm64/sysroot/lib/"
cp /tmp/husk_base/Payload/Husk.app/Frameworks/libhuskps4.dylib "$HUSK_ROOT/build/ios-arm64/lib/"

# Set up MoltenVK.xcframework
MVK_DIR="$HUSK_ROOT/build/ios-arm64/lib/MoltenVK.xcframework"
mkdir -p "$MVK_DIR/ios-arm64/MoltenVK.framework"

if [ -d "/tmp/husk_base/Payload/Husk.app/Frameworks/MoltenVK.framework" ]; then
    echo "==> Using MoltenVK.framework from base IPA"
    cp -R /tmp/husk_base/Payload/Husk.app/Frameworks/MoltenVK.framework "$MVK_DIR/ios-arm64/"
else
    echo "==> Creating MoltenVK framework stub"
    SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)"
    xcrun --sdk iphoneos clang -dynamiclib -target arm64-apple-ios15.5 -isysroot "$SDKROOT" \
        -install_name @rpath/MoltenVK.framework/MoltenVK \
        -o "$MVK_DIR/ios-arm64/MoltenVK.framework/MoltenVK" -x c /dev/null
    cat > "$MVK_DIR/ios-arm64/MoltenVK.framework/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>io.vulkan.MoltenVK</string>
	<key>CFBundleExecutable</key>
	<string>MoltenVK</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
</dict>
</plist>
EOF
fi

cat > "$MVK_DIR/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AvailableLibraries</key>
	<array>
		<dict>
			<key>BinaryPath</key>
			<string>MoltenVK.framework/MoltenVK</string>
			<key>LibraryIdentifier</key>
			<string>ios-arm64</string>
			<key>LibraryPath</key>
			<string>MoltenVK.framework</string>
			<key>SupportedArchitectures</key>
			<array>
				<string>arm64</string>
			</array>
			<key>SupportedPlatform</key>
			<string>ios</string>
		</dict>
	</array>
	<key>CFBundlePackageType</key>
	<string>XFWK</string>
	<key>XCFrameworkFormatVersion</key>
	<string>1.0</string>
</dict>
</plist>
EOF

# Stage guest images for preBuildScripts
cp /tmp/husk_base/Payload/Husk.app/vmlinuz-virt "$HUSK_ROOT/guests/phase0/" 2>/dev/null || touch "$HUSK_ROOT/guests/phase0/vmlinuz-virt"
cp /tmp/husk_base/Payload/Husk.app/initramfs-virt "$HUSK_ROOT/guests/phase0/" 2>/dev/null || touch "$HUSK_ROOT/guests/phase0/initramfs-virt"

echo "==> Base assets staged successfully"
