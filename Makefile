# Variables
CC = gcc
COMMON_INCLUDES = -I. -I./platform
TARGET = test
SRC = test_spi_bridge.c

# Default target: Shows usage (prevents accidental "make all")
usage:
	@echo "Usage:"
	@echo "  make spi_dev       - Build 'test' for SPI device"
	@echo "  make kernel_ioctl  - Build 'test' for Kernel IOCTL"
	@echo "  make clean         - Remove binary"

# Target-Specific Variable Definitions
spi_dev: CFLAGS = $(COMMON_INCLUDES) -DUSE_SPI_DEV
spi_dev: MODE_NAME = SPI DEVICE

kernel_ioctl: CFLAGS = $(COMMON_INCLUDES) -DUSE_KERNEL_IOCTL
kernel_ioctl: MODE_NAME = KERNEL IOCTL

# Both targets call the same build logic
spi_dev kernel_ioctl: $(TARGET)

$(TARGET): $(SRC)
	@echo "------------------------------------------------"
	@echo "Building $(TARGET) for: $(MODE_NAME)"
	@echo "------------------------------------------------"
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: usage clean spi_dev kernel_ioctl