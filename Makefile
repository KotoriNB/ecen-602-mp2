# Makefile -- ECEN 602 MP2: SBCP chat server and client
#
#   make              build server and client
#   make test         server test suite (tests/test_sbcp.c, about 25 s)
#   make test-quick   the same, skipping the slow timing case
#   make test-client  end-to-end checks of ./client against ./server (needs python3)
#   make probe        build tests/sbcp_probe (checks another server's byte layout)
#   make SANITIZE=1   build with AddressSanitizer + UndefinedBehaviorSanitizer
#   make clean        remove binaries, object files and test logs
#   make dist         clean, then produce the submission tarball

CC      := gcc
CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O2 -g -Iinclude
LDFLAGS :=

ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
LDFLAGS += -fsanitize=address,undefined
endif

BUILD      := build
LIB_OBJ    := $(BUILD)/sbcp_codec.o $(BUILD)/net.o $(BUILD)/util.o
SERVER_OBJ := $(BUILD)/server.o $(LIB_OBJ)
CLIENT_OBJ := $(BUILD)/client.o
HEADERS    := include/sbcp.h include/sbcp_codec.h include/net.h include/util.h

.PHONY: all clean test test-quick test-client probe dist

all: server client

server: $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(BUILD)/server.o:     src/server.c $(HEADERS)
$(BUILD)/client.o:     src/client.c include/sbcp.h
$(BUILD)/sbcp_codec.o: src/sbcp_codec.c include/sbcp.h include/sbcp_codec.h
$(BUILD)/net.o:        src/net.c include/net.h include/util.h
$(BUILD)/util.o:       src/util.c include/util.h

tests/test_sbcp: tests/test_sbcp.c $(LIB_OBJ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ tests/test_sbcp.c $(LIB_OBJ) $(LDFLAGS)

tests/sbcp_probe: tests/sbcp_probe.c $(LIB_OBJ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ tests/sbcp_probe.c $(LIB_OBJ) $(LDFLAGS)

test: server tests/test_sbcp
	./tests/test_sbcp

test-quick: server tests/test_sbcp
	./tests/test_sbcp --quick

test-client: server client
	python3 tests/client_checks.py ./server ./client

probe: tests/sbcp_probe

clean:
	rm -rf $(BUILD) tests/logs *.dSYM
	rm -f server client tests/test_sbcp tests/sbcp_probe core core.* *~

dist: clean
	tar czf ../ecen-602-mp2.tar.gz --exclude='*.tar.gz' --exclude='.git' .
	@echo "wrote ../ecen-602-mp2.tar.gz"
