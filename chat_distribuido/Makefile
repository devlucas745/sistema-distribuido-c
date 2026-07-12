CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
BIN_DIR = bin

.PHONY: all clean run-server run-client

all: $(BIN_DIR)/server $(BIN_DIR)/client

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/server: server/server.c common/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ server/server.c

$(BIN_DIR)/client: client/client.c common/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ client/client.c

run-server: $(BIN_DIR)/server
	./$(BIN_DIR)/server

run-client: $(BIN_DIR)/client
	./$(BIN_DIR)/client

clean:
	rm -rf $(BIN_DIR) received_files
