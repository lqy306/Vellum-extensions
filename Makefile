# Vellum Extensions —— 从源码构建扩展二进制
# 需要 make、cc、pkg-config 及清单列出的开发包（见 BUILDING.md）。

CC ?= cc
CFLAGS ?= -O2 -fPIC -std=c11 -Wall -Wextra -Isrc -DGETTEXT_PACKAGE="vellum"
BUILD_DIR ?= build

PLUGINS = timestamp-plugin word-count-plugin ai-completion-plugin link-check-plugin \
          project-sidebar-plugin build-run-plugin vim-mode-plugin screenshot-plugin welcome-plugin

.PHONY: all clean

all: $(foreach p,$(PLUGINS),$(BUILD_DIR)/$(p).so)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/timestamp-plugin.so: src/plugins/timestamp-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gio-2.0 gmodule-2.0)

$(BUILD_DIR)/word-count-plugin.so: src/plugins/word-count-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gio-2.0 gmodule-2.0)

$(BUILD_DIR)/ai-completion-plugin.so: src/plugins/ai-completion-plugin.c src/plugins/ai-code-summary.c src/plugins/ai-code-summary.h src/plugins/ai-completion-features.c src/plugins/ai-completion-features.h src/plugins/ai-completion-private.h src/plugins/ai-diff.c src/plugins/ai-diff.h src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< src/plugins/ai-code-summary.c src/plugins/ai-completion-features.c src/plugins/ai-diff.c $(shell pkg-config --cflags --libs gtk4 libadwaita-1 gio-2.0 gmodule-2.0 libsoup-3.0 json-glib-1.0 gtksourceview-5)

$(BUILD_DIR)/link-check-plugin.so: src/plugins/link-check-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gio-2.0 gmodule-2.0 libsoup-3.0)

$(BUILD_DIR)/project-sidebar-plugin.so: src/plugins/project-sidebar-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gtk4 libadwaita-1 gio-2.0 gmodule-2.0)

$(BUILD_DIR)/build-run-plugin.so: src/plugins/build-run-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gtk4 libadwaita-1 gio-2.0 gmodule-2.0)

$(BUILD_DIR)/vim-mode-plugin.so: src/plugins/vim-mode-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gtk4 gio-2.0 gmodule-2.0)

$(BUILD_DIR)/screenshot-plugin.so: src/plugins/screenshot-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gtk4 gio-2.0 gmodule-2.0)

$(BUILD_DIR)/welcome-plugin.so: src/plugins/welcome-plugin.c src/mt-plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $< $(shell pkg-config --cflags --libs gtk4 libadwaita-1 gio-2.0 gmodule-2.0)

clean:
	rm -rf $(BUILD_DIR)
