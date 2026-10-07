#!/bin/bash
# ========================================
# Docker-based ESP-IDF Build Script
# For ESP32-S3 N16R8 (16MB Flash + 8MB PSRAM)
# ========================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}ESP32-S3 N16R8 Firmware Builder${NC}"
echo -e "${GREEN}========================================${NC}"

# Configuration
IDF_VERSION="v6.1"
PROJECT_DIR="$(pwd)"
OUTPUT_DIR="${PROJECT_DIR}/build"

# Check if Docker is available
if ! command -v docker &> /dev/null; then
    echo -e "${RED}Error: Docker is not installed or not in PATH${NC}"
    echo "Please install Docker: https://docs.docker.com/get-docker/"
    exit 1
fi

echo -e "${YELLOW}Using ESP-IDF Docker image: espressif/idf:release-${IDF_VERSION}${NC}"

# Pull the ESP-IDF Docker image
echo -e "${YELLOW}Pulling ESP-IDF Docker image...${NC}"
docker pull espressif/idf:release-${IDF_VERSION}

# Clean previous build (optional)
read -p "Clean previous build? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "${OUTPUT_DIR}"
    rm -f sdkconfig sdkconfig.old
fi

# Copy N16R8 configuration
echo -e "${YELLOW}Configuring for N16R8 board...${NC}"
cp sdkconfig.n16r8 sdkconfig.defaults

# Run build in Docker container
echo -e "${YELLOW}Starting build in Docker container...${NC}"
docker run --rm \
    -v "${PROJECT_DIR}:/project" \
    -w /project \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    espressif/idf:release-${IDF_VERSION} \
    /bin/bash -c "
        set -e
        echo '=== Setting up ESP-IDF environment ==='
        . /opt/esp/idf/export.sh

        echo '=== Configuring project ==='
        idf.py set-target esp32s3

        echo '=== Building project ==='
        idf.py build

        echo '=== Build complete ==='
        ls -lh build/*.bin
    "

# Check if build succeeded
if [ $? -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Output files in: ${OUTPUT_DIR}/"
    echo ""
    echo -e "${YELLOW}Firmware files:${NC}"
    ls -lh "${OUTPUT_DIR}/bootloader/bootloader.bin" 2>/dev/null || true
    ls -lh "${OUTPUT_DIR}/partition_table/partition-table.bin" 2>/dev/null || true
    ls -lh "${OUTPUT_DIR}/ota_data_initial.bin" 2>/dev/null || true
    ls -lh "${OUTPUT_DIR}/esp-usb-ble-hid.bin" 2>/dev/null || true
    echo ""
    echo -e "${GREEN}Flash command:${NC}"
    echo "esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \\"
    echo "  write_flash \\"
    echo "  0x0 build/bootloader/bootloader.bin \\"
    echo "  0x8000 build/partition_table/partition-table.bin \\"
    echo "  0x11000 build/ota_data_initial.bin \\"
    echo "  0x20000 build/esp-usb-ble-hid.bin"
    echo ""

    # Create firmware package
    echo -e "${YELLOW}Creating firmware package...${NC}"
    mkdir -p firmware_n16r8
    cp build/bootloader/bootloader.bin firmware_n16r8/
    cp build/partition_table/partition-table.bin firmware_n16r8/
    cp build/ota_data_initial.bin firmware_n16r8/
    cp build/esp-usb-ble-hid.bin firmware_n16r8/
    cp partitions_16mb.csv firmware_n16r8/

    cat > firmware_n16r8/flash.sh << 'FLASHEOF'
#!/bin/bash
# Quick flash script for ESP32-S3 N16R8

PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-921600}"

echo "Flashing to ${PORT} at ${BAUD} baud..."

esptool.py --chip esp32s3 --port "${PORT}" --baud "${BAUD}" \
  --before default_reset --after hard_reset write_flash \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x11000 ota_data_initial.bin \
  0x20000 esp-usb-ble-hid.bin

echo "Done! Connect serial monitor to see output."
FLASHEOF

    chmod +x firmware_n16r8/flash.sh

    echo -e "${GREEN}Firmware package created: firmware_n16r8/${NC}"
    echo "Run: cd firmware_n16r8 && ./flash.sh /dev/ttyUSB0"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Build failed!${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
