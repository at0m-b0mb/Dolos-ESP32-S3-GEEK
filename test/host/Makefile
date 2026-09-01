# Host tests for the Dolos DuckyScript engine. No ESP-IDF required.
CC     ?= cc
INC    := -I../../components/ducky/include -I../../components/ui/include -I../../components/dconsole/include -I.
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g $(INC)
DUCKY  := ../../components/ducky/src
DSRC   := $(DUCKY)/dscript.c $(DUCKY)/ducky.c $(DUCKY)/hid_keymap.c $(DUCKY)/layout.c $(DUCKY)/unicode.c $(DUCKY)/lint.c
BIN    := ./bin
SUITES := test_sbuf test_dscript test_keymap test_ducky test_dui test_layout test_config test_auth test_unicode test_lint test_fuzz test_menu test_button

.PHONY: all clean portability

# POSIX functions that macOS declares unconditionally and glibc hides under
# -std=c11. Twice now one of these has compiled cleanly here and failed on the
# Linux runner, so the portable sources are checked before anything is built.
POSIX_ONLY := strncasecmp strcasecmp strnlen strdup strsep index rindex
PORTABLE := $(DUCKY)/dscript.c $(DUCKY)/ducky.c $(DUCKY)/lint.c \
            $(DUCKY)/hid_keymap.c $(DUCKY)/layout.c $(DUCKY)/unicode.c \
            $(DUCKY)/dconfig.c $(DUCKY)/menu.c ../../components/dconsole/src/auth.c
portability:
	@bad=0; for f in $(PORTABLE); do \
	  code=$$(sed -e 's://.*::' -e '/^[[:space:]]*\*/d' -e 's:/\*.*\*/::' $$f); \
	  for fn in $(POSIX_ONLY); do \
	    if printf '%s' "$$code" | grep -qE "[^A-Za-z_]$$fn[[:space:]]*\("; then \
	      echo "  $$f uses $$fn() - POSIX, not ISO C; glibc rejects it under -std=c11"; \
	      bad=1; fi; \
	  done; done; \
	if [ $$bad -ne 0 ]; then echo "portability check FAILED"; exit 1; fi; \
	echo "portability: portable sources use ISO C only"

all: portability $(SUITES:%=run-%)
	@echo "\nAll suites passed."
run-%: $(BIN)/%
	@echo "--- $< ---"; $<
$(BIN)/test_keymap:  test_keymap.c  $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_ducky:   test_ducky.c   $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_layout:  test_layout.c  $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_unicode: test_unicode.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_lint:    test_lint.c    $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_dscript: test_dscript.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
# The fuzz suite is always built WITH sanitizers: a buffer overrun in a parser
# must abort the run, not quietly pass.
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
$(BIN)/test_fuzz:    test_fuzz.c    $(DUCKY)/dconfig.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $(SAN) $^ -o $@
$(BIN)/test_config:  test_config.c  $(DUCKY)/dconfig.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_menu:    test_menu.c    $(DUCKY)/menu.c $(DUCKY)/dconfig.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
$(BIN)/test_button:  test_button.c  ../../components/ui/src/button.c | $(BIN)
	$(CC) $(CFLAGS) $(UIINC) $^ -o $@
$(BIN)/test_auth:    test_auth.c    ../../components/dconsole/src/auth.c | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@
# built with sanitizers: an out-of-bounds write here must abort, not pass
$(BIN)/test_sbuf:    test_sbuf.c    | $(BIN)
	$(CC) $(CFLAGS) $(SAN) -I../../main $^ -o $@
$(BIN):
	@mkdir -p $(BIN)
clean:
	@rm -rf $(BIN)

# --- UI suite + mockups ---
UI := ../../components/ui/src
UIINC := -I../../components/ui/include
$(BIN)/test_dui: test_dui.c $(UI)/dui.c $(UI)/canvas.c $(UI)/font5x8.c $(UI)/font7x12.c $(UI)/qrcodegen.c $(DUCKY)/menu.c $(DUCKY)/dconfig.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $(UIINC) $^ -o $@
$(BIN)/render_mockup: render_mockup.c $(UI)/dui.c $(UI)/canvas.c $(UI)/font5x8.c $(UI)/font7x12.c $(UI)/qrcodegen.c $(DUCKY)/menu.c $(DUCKY)/dconfig.c $(DSRC) | $(BIN)
	$(CC) $(CFLAGS) $(UIINC) $^ -o $@
.PHONY: ui mockups
ui: run-test_dui
mockups: $(BIN)/render_mockup
	@mkdir -p out && $(BIN)/render_mockup out
