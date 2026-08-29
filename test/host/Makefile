# Host tests for the Dolos DuckyScript engine. No ESP-IDF required.
CC     ?= cc
INC    := -I../../components/ducky/include -I../../components/ui/include -I../../components/dconsole/include -I.
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g $(INC)
DUCKY  := ../../components/ducky/src
BIN    := ./bin
SUITES := test_keymap test_ducky test_dui test_layout test_config test_auth

.PHONY: all clean
all: $(SUITES:%=run-%)
	@echo "\nAll suites passed."
run-%: $(BIN)/%
	@echo "--- $< ---"; $<
$(BIN)/test_keymap: test_keymap.c $(DUCKY)/hid_keymap.c $(DUCKY)/ducky.c $(DUCKY)/layout.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_ducky:  test_ducky.c  $(DUCKY)/hid_keymap.c $(DUCKY)/ducky.c $(DUCKY)/layout.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_layout: test_layout.c $(DUCKY)/layout.c $(DUCKY)/hid_keymap.c $(DUCKY)/ducky.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_config: test_config.c $(DUCKY)/dconfig.c $(DUCKY)/layout.c $(DUCKY)/hid_keymap.c $(DUCKY)/ducky.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_auth:   test_auth.c   ../../components/dconsole/src/auth.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN):
	@mkdir -p $(BIN)
clean:
	@rm -rf $(BIN)

# --- UI suite + mockups ---
UI := ../../components/ui/src
UIINC := -I../../components/ui/include
$(BIN)/test_dui: test_dui.c $(UI)/dui.c $(UI)/canvas.c $(UI)/font5x8.c | $(BIN)
	$(CC) $(CFLAGS) $(UIINC) $^ -o $@
$(BIN)/render_mockup: render_mockup.c $(UI)/dui.c $(UI)/canvas.c $(UI)/font5x8.c | $(BIN)
	$(CC) $(CFLAGS) $(UIINC) $^ -o $@
.PHONY: ui mockups
ui: run-test_dui
mockups: $(BIN)/render_mockup
	@mkdir -p out && $(BIN)/render_mockup out
