CC ?= gcc
PICOQUIC_HOME ?= $(HOME)/picoquic
BUILD := $(PICOQUIC_HOME)/build

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -pthread
CPPFLAGS += \
	-Iinclude \
	-Isrc \
	-I$(PICOQUIC_HOME)/picoquic \
	-I$(PICOQUIC_HOME)/picohttp

LDLIBS += \
	$(BUILD)/libpicohttp-core.a \
	$(BUILD)/libpicoquic-log.a \
	$(BUILD)/libpicoquic-core.a \
	$(BUILD)/_deps/picotls-build/libpicotls-openssl.a \
	$(BUILD)/_deps/picotls-build/libpicotls-fusion.a \
	$(BUILD)/_deps/picotls-build/libpicotls-minicrypto.a \
	$(BUILD)/_deps/picotls-build/libpicotls-core.a \
	-lssl -lcrypto -ldl -lbrotlidec -lbrotlienc -pthread

TARGET := proxy
SRC := \
	src/main.c \
	src/local_proxy.c \
	src/socket_io.c \
	src/mpquic_client.c \
	src/mpquic_requests.c \
	src/mpquic_paths.c
OBJ := $(SRC:.c=.o)
DEPS := $(OBJ:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ) $(DEPS)

-include $(DEPS)
