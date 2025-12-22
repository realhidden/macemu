# macemu WebRTC Streaming Server Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -fPIC
LDFLAGS = -lpthread

# Directories
SERVER_DIR = server
CLIENT_DIR = client
BUILD_DIR = build

# libdatachannel paths
LIBDATACHANNEL_DIR = libdatachannel
LIBDATACHANNEL_BUILD = $(LIBDATACHANNEL_DIR)/build
LIBDATACHANNEL_LIB = $(LIBDATACHANNEL_BUILD)/libdatachannel.a
LIBDATACHANNEL_INCLUDES = -I$(LIBDATACHANNEL_DIR)/include

# libdatachannel dependencies (built alongside libdatachannel)
LIBJUICE = $(LIBDATACHANNEL_BUILD)/deps/libjuice/libjuice.a
LIBSRTP = $(LIBDATACHANNEL_BUILD)/deps/libsrtp/libsrtp2.a
USRSCTP = $(LIBDATACHANNEL_BUILD)/deps/usrsctp/usrsctplib/libusrsctp.a

# OpenH264 (system package - replaces libvpx)
H264_CFLAGS = $(shell pkg-config --cflags openh264 2>/dev/null)
H264_LIBS = $(shell pkg-config --libs openh264 2>/dev/null || echo "-lopenh264")

# libyuv (system package - for fast color conversion)
# Note: libyuv doesn't always have pkg-config, so we check for the header directly
YUV_CFLAGS = $(shell pkg-config --cflags libyuv 2>/dev/null)
YUV_LIBS = $(shell pkg-config --libs libyuv 2>/dev/null || echo "-lyuv")

# OpenSSL (system package)
OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS = $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")

# Combined flags
SERVER_CFLAGS = $(LIBDATACHANNEL_INCLUDES) $(H264_CFLAGS) $(YUV_CFLAGS) $(OPENSSL_CFLAGS)
SERVER_LIBS = $(LIBDATACHANNEL_LIB) $(LIBJUICE) $(LIBSRTP) $(USRSCTP) $(H264_LIBS) $(YUV_LIBS) $(OPENSSL_LIBS) -lpthread -lrt

# Source files
SERVER_SRCS = $(SERVER_DIR)/server.cpp $(SERVER_DIR)/h264_encoder.cpp

# Object files
SERVER_OBJS = $(BUILD_DIR)/server.o $(BUILD_DIR)/h264_encoder.o

# Target
SERVER = $(BUILD_DIR)/macemu-webrtc

# Default target
all: deps-check libdatachannel dirs $(SERVER)
	@echo ""
	@echo "Build complete!"
	@echo "Server: $(SERVER)"
	@echo ""
	@echo "Usage:"
	@echo "  1. Start the server: ./$(SERVER)"
	@echo "     (note the PID-based socket path it prints)"
	@echo "  2. Build Basilisk II with: ./configure --enable-ipc-video && make"
	@echo "  3. Run emulator: MACEMU_CONTROL_SOCK=/tmp/macemu-<PID>.sock ./BasiliskII"
	@echo "  4. Open http://localhost:8000 in your browser"

# Create build directory
dirs:
	@mkdir -p $(BUILD_DIR)

# Build libdatachannel
libdatachannel: $(LIBDATACHANNEL_LIB)

$(LIBDATACHANNEL_LIB):
	@echo "Building libdatachannel..."
	@mkdir -p $(LIBDATACHANNEL_BUILD)
	cd $(LIBDATACHANNEL_BUILD) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DNO_WEBSOCKET=OFF \
		-DNO_MEDIA=OFF \
		-DNO_EXAMPLES=ON \
		-DNO_TESTS=ON \
		-DUSE_GNUTLS=OFF \
		-DUSE_MBEDTLS=OFF \
		-DUSE_NICE=OFF \
		-DBUILD_SHARED_LIBS=OFF
	$(MAKE) -C $(LIBDATACHANNEL_BUILD) -j$$(nproc)
	@echo "libdatachannel built successfully"

# Compile server
$(BUILD_DIR)/server.o: $(SERVER_DIR)/server.cpp $(SERVER_DIR)/ipc_protocol.h $(SERVER_DIR)/codec.h $(SERVER_DIR)/h264_encoder.h $(LIBDATACHANNEL_LIB)
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile H.264 encoder
$(BUILD_DIR)/h264_encoder.o: $(SERVER_DIR)/h264_encoder.cpp $(SERVER_DIR)/h264_encoder.h $(SERVER_DIR)/codec.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Link server
$(SERVER): $(SERVER_OBJS)
	$(CXX) -o $@ $(SERVER_OBJS) $(SERVER_LIBS)

# Run server
run: $(SERVER)
	./$(SERVER)

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Clean everything including libdatachannel
distclean: clean
	rm -rf $(LIBDATACHANNEL_BUILD)

# Check dependencies
deps-check:
	@echo "Checking dependencies..."
	@which cmake >/dev/null 2>&1 || (echo "ERROR: cmake not found. Install with: sudo apt install cmake" && exit 1)
	@which pkg-config >/dev/null 2>&1 || (echo "ERROR: pkg-config not found. Install with: sudo apt install pkg-config" && exit 1)
	@pkg-config --exists openh264 2>/dev/null || (echo "ERROR: libopenh264 not found. Install with: sudo apt install libopenh264-dev" && exit 1)
	@(pkg-config --exists libyuv 2>/dev/null || test -f /usr/include/libyuv.h) || (echo "ERROR: libyuv not found. Install with: sudo apt install libyuv-dev" && exit 1)
	@pkg-config --exists openssl 2>/dev/null || (echo "ERROR: openssl not found. Install with: sudo apt install libssl-dev" && exit 1)
	@echo "All dependencies found!"

# Install dependencies (Ubuntu/Debian)
deps:
	sudo apt-get install -y cmake pkg-config libopenh264-dev libyuv-dev libssl-dev

# Install server
install: $(SERVER)
	@mkdir -p /usr/local/bin
	cp $(SERVER) /usr/local/bin/

# Help
help:
	@echo "macemu WebRTC Streaming Server"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the server (default)"
	@echo "  libdatachannel - Build libdatachannel library only"
	@echo "  run          - Build and run the server"
	@echo "  clean        - Remove build files"
	@echo "  distclean    - Remove all build files including libdatachannel"
	@echo "  deps         - Install system dependencies"
	@echo "  deps-check   - Check if dependencies are installed"
	@echo "  install      - Install server to /usr/local/bin"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Quick Start:"
	@echo "  1. Install deps: make deps"
	@echo "  2. Build server: make"
	@echo "  3. Run server: ./build/macemu-webrtc"
	@echo "     Server prints: 'Control socket: /tmp/macemu-<PID>.sock'"
	@echo "  4. Build Basilisk II:"
	@echo "     cd ../BasiliskII/src/Unix"
	@echo "     ./configure --enable-ipc-video"
	@echo "     make"
	@echo "  5. Run emulator with socket path:"
	@echo "     MACEMU_CONTROL_SOCK=/tmp/macemu-<PID>.sock ./BasiliskII"
	@echo "  6. Open http://localhost:8000 in browser"

.PHONY: all dirs run clean distclean deps deps-check help libdatachannel install
