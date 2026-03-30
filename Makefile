CC = gcc
CFLAGS = -Wall -Wextra -O2

# OpenSSL (Homebrew) include/library paths for macOS
OPENSSL_INC = -I/opt/homebrew/opt/openssl@3/include -I/usr/local/opt/openssl@3/include
OPENSSL_LIB = -L/opt/homebrew/opt/openssl@3/lib -L/usr/local/opt/openssl@3/lib

TARGET = proxy
SRCS = proxy.c

CLIENT = client
CLIENT_SRCS = client.c
SERVER = server
SERVER_SRCS = server.c
MD5_LIBS = -lssl -lcrypto

all: $(TARGET) $(CLIENT) #$(SERVER)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(OPENSSL_INC) $(SRCS) -o $(TARGET) $(LIBS) $(OPENSSL_LIB) $(MD5_LIBS)

$(CLIENT): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT) $(LIBS)

$(SERVER): $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o $(SERVER) $(LIBS)

run_proxy: $(TARGET)
	./$(TARGET) 8888 2

run_client: $(CLIENT)
	./$(CLIENT) http://www.yahoo.com/news/articles/iranian-military-mocks-trumps-claim-042851486.html 8888

debug:
	$(MAKE) CFLAGS="$(CFLAGS) -g -DDEBUG"

clean:
	rm -f $(TARGET)
	rm -f $(CLIENT)
	rm -f $(SERVER)
	rm -rf ./cache