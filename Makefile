CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude
LDFLAGS = -lssl -lcrypto -lcurl -pthread

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

# Core source files (excluding main.c and test files)
CORE_SOURCES = bencode.c \
               torrent_parser.c \
               contact_tracker.c \
               handshake_with_peer.c \
               receive_message.c \
               parse_message.c \
               requestPayload.c \
               sendRequest.c \
               store_pieces.c \
               verify_pieces.c \
               file_writer.c \
               outgoingMessages.c \
			   upload_manager.c \
               manage_peers.c \
               init_torrent_state.c \
			   multithreaded_download_coordinator.c \
               download_coordinator.c

CORE_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SOURCES))

# Executables - now in parent folder
MAIN_CLIENT = bittorrent_client

# Default target
all: directories $(MAIN_CLIENT)

# Create necessary directories
directories:
	@mkdir -p $(BUILD_DIR)

# Main client executable
$(MAIN_CLIENT): $(CORE_OBJECTS) $(BUILD_DIR)/main.o
	@echo "Linking $@..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(MAIN_CLIENT)
	@echo "✓ Clean complete"

# Clean and rebuild
rebuild: clean all

# Run the client
run: all
	./$(MAIN_CLIENT)

# Show help
help:
	@echo "BitTorrent Client Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build main client"
	@echo "  clean            - Remove all build artifacts"
	@echo "  rebuild          - Clean and build main client"
	@echo "  run              - Build and run client"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Usage:"
	@echo "  make                          # Build client"
	@echo "  ./bittorrent_client file.torrent  # Run"

.PHONY: all directories clean rebuild run help