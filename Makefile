CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror=implicit-function-declaration
PKGS    := x11 xi xrandr xrender cairo
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDFLAGS := $(shell pkg-config --libs $(PKGS)) -lm -ldl

TARGET  := win11-snapd
SRCS    := src/main.c src/config.c src/layouts.c src/x11_state.c src/snap.c src/popup.c src/csd_decor.c src/assist.c
OBJS    := $(SRCS:.c=.o)

PREFIX_BIN        := $(HOME)/.local/bin
PREFIX_APPS       := $(HOME)/.local/share/applications
PREFIX_THEMES     := $(HOME)/.themes
PREFIX_CONFIG     := $(HOME)/.config/win11-snap
PREFIX_SYSTEMD    := $(HOME)/.config/systemd/user
PREFIX_EXTENSIONS := $(HOME)/.local/share/cinnamon/extensions
EXT_UUID          := win11-snap@pedro
THEME_NAME        := Mint-Y-Dark-Aqua-Win11

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	-systemctl --user disable --now win11-snapd.service 2>/dev/null || true
	rm -f "$(PREFIX_SYSTEMD)/win11-snapd.service"
	rm -f "$(PREFIX_BIN)/win11-snap-settings"
	rm -f "$(PREFIX_APPS)/win11-snap-settings.desktop"
	-systemctl --user daemon-reload 2>/dev/null || true
	install -d "$(PREFIX_BIN)"
	install -m 0755 "$(TARGET)" "$(PREFIX_BIN)/$(TARGET)"
	install -d "$(PREFIX_THEMES)"
	rm -rf "$(PREFIX_THEMES)/$(THEME_NAME)"
	cp -r "theme/$(THEME_NAME)" "$(PREFIX_THEMES)/$(THEME_NAME)"
	install -d "$(PREFIX_CONFIG)"
	@if [ ! -f "$(PREFIX_CONFIG)/config.ini" ]; then \
		install -m 0644 "config/config.ini" "$(PREFIX_CONFIG)/config.ini"; \
	else \
		echo "Keeping existing $(PREFIX_CONFIG)/config.ini"; \
	fi
	install -d "$(PREFIX_EXTENSIONS)"
	rm -rf "$(PREFIX_EXTENSIONS)/$(EXT_UUID)"
	ln -sfn "$(CURDIR)" "$(PREFIX_EXTENSIONS)/$(EXT_UUID)"

uninstall:
	-systemctl --user disable --now win11-snapd.service 2>/dev/null || true
	-pkill -x win11-snapd 2>/dev/null || true
	rm -f "$(PREFIX_EXTENSIONS)/$(EXT_UUID)"
	rm -rf "$(PREFIX_EXTENSIONS)/$(EXT_UUID)"
	rm -f "$(PREFIX_BIN)/$(TARGET)"
	rm -f "$(PREFIX_BIN)/win11-snap-settings"
	rm -f "$(PREFIX_APPS)/win11-snap-settings.desktop"
	rm -f "$(PREFIX_SYSTEMD)/win11-snapd.service"
	rm -rf "$(PREFIX_THEMES)/$(THEME_NAME)"
	-systemctl --user daemon-reload 2>/dev/null || true

clean:
	rm -f $(OBJS) $(TARGET)
