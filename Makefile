CC ?= cc
.DEFAULT_GOAL := all

# The repository pins one SDK revision; developers may override it explicitly
# when testing an SDK change that has not reached the submodule pin yet.
KILIX_GAME_SDK_DIR ?= third_party/kilix-game-sdk
include $(KILIX_GAME_SDK_DIR)/mk/kilix-game-sdk.mk

SDK_BUILD := $(abspath build/sdk)
KILIX_GAME_KIT_BUILD_DIR ?= $(SDK_BUILD)/game-kit
KILIX_TOP_DOWN_BUILD_DIR ?= $(SDK_BUILD)/top-down
KILIX_ASSETS_BUILD_DIR ?= $(SDK_BUILD)/assets

include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
include $(KILIX_TOP_DOWN_DIR)/mk/kilix-top-down.mk
include $(KILIX_ASSETS_DIR)/mk/kilix-assets.mk

override CPPFLAGS += $(KILIX_GAME_KIT_CPPFLAGS) $(KILIX_TD_CPPFLAGS) \
	$(KILIX_ASSETS_CPPFLAGS) -Isrc
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -pthread $(WARNINGS) -MMD -MP
LDLIBS := $(KILIX_ASSETS_LDLIBS) $(KILIX_GAME_KIT_LDLIBS)

BIN := kilix-land-agent
SRC := src/main.c src/studio.c src/graphics.c src/render.c \
	src/app_launcher.c \
	src/agent_protocol.c src/agent_session.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))
DEPS := $(OBJ:.o=.d)

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ) $(KILIX_ASSETS_LIB) $(KILIX_TD_LIBS) \
	$(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_ASSETS_LIB) \
		$(KILIX_TD_LIBS) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

build/%.o: src/%.c src/studio.h src/graphics.h src/render.h \
	src/app_launcher.h src/agent_protocol.h src/agent_session.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p $@

test: $(BIN)
	sha256sum --check assets/graphics/SHA256SUMS
	sha256sum --check assets/demo/SHA256SUMS
	python3 tools/check_asset_digest_agreement.py
	./$(BIN) --selftest
	./$(BIN) --graphics-test
	./$(BIN) --observe >/dev/null
	./kilix-land-agentd --help >/dev/null
	python3 tools/qwen_room_demo.py --help >/dev/null
	python3 tools/qwen_video_demo.py --help >/dev/null
	python3 -m unittest discover -s tests -v
	./$(BIN) --render-test build/studio-room.ppm
	./$(BIN) --chat-render-test build/studio-chat.ppm

clean:
	$(RM) -r build $(BIN)

-include $(DEPS)
