#!/bin/bash -e
# QAIRT SDK setup for ANVIL VFI filter
#
# The Qualcomm AI Runtime (QAIRT) SDK provides QNN headers and libraries
# needed for HTP NPU inference. It must be downloaded separately from:
#   https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct
#
# Set QAIRT_SDK_ROOT to your local QAIRT installation, e.g.:
#   export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/2.42.0.251225
#
# This script copies headers to the build prefix so mpv can compile
# against them. The actual .so libraries are deployed to the device
# separately (they ship as part of the APK assets or are pre-deployed).

. ../../include/path.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$prefix_dir/include/QNN"
	exit 0
else
	exit 255
fi

# Auto-detect QAIRT SDK if not set
if [ -z "$QAIRT_SDK_ROOT" ]; then
	# Common installation paths
	for candidate in \
		/opt/qcom/aistack/qairt/*/include/QNN/QnnInterface.h \
		$HOME/qairt/*/include/QNN/QnnInterface.h \
		$HOME/.local/lib/python*/site-packages/qairt/*/include/QNN/QnnInterface.h; do
		if [ -f "$candidate" ]; then
			QAIRT_SDK_ROOT=$(dirname $(dirname $(dirname "$candidate")))
			break
		fi
	done
fi

if [ -z "$QAIRT_SDK_ROOT" ] || [ ! -d "$QAIRT_SDK_ROOT/include/QNN" ]; then
	echo "WARNING: QAIRT SDK not found. ANVIL QNN inference will be disabled."
	echo "To enable: export QAIRT_SDK_ROOT=/path/to/qairt/version"
	echo "Download from: https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct"
	# Create a stub header so compilation succeeds without QNN
	mkdir -p "$prefix_dir/include/QNN/System" "$prefix_dir/include/QNN/HTP"
	cat > "$prefix_dir/include/QNN/QnnInterface.h" << 'EOF'
// Stub: QAIRT SDK not available. QNN inference disabled at compile time.
#ifndef QNN_INTERFACE_H
#define QNN_INTERFACE_H
#define QNN_SDK_STUB 1
#endif
EOF
	exit 0
fi

echo "Using QAIRT SDK: $QAIRT_SDK_ROOT"

# Copy headers to prefix (headers only, no .so files)
mkdir -p "$prefix_dir/include/QNN/System" "$prefix_dir/include/QNN/HTP"
cp "$QAIRT_SDK_ROOT"/include/QNN/*.h "$prefix_dir/include/QNN/"
cp "$QAIRT_SDK_ROOT"/include/QNN/System/*.h "$prefix_dir/include/QNN/System/" 2>/dev/null || true
cp "$QAIRT_SDK_ROOT"/include/QNN/HTP/*.h "$prefix_dir/include/QNN/HTP/" 2>/dev/null || true

echo "QNN headers installed to $prefix_dir/include/QNN/"
