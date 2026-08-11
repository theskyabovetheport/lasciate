# lasciate -- build and install
#
# Installs under $HOME by default; no root needed, and nothing outside the
# prefix is touched. `make PREFIX=/usr/local install` for a system-wide install.

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/lasciate

CC      ?= cc

# -Og and not -O2: nothing here is hot enough to optimise for -- a handful of
# short strings per keystroke -- so the aggressive levels buy nothing and cost
# the ability to read a stack trace. Not -O0 either, which would be the obvious
# choice: _FORTIFY_SOURCE below is a no-op without some optimisation enabled,
# and -Og is the lowest level that keeps it.
CFLAGS  ?= -Og
LDLIBS  := -ldl

# Deliberately not part of CFLAGS. CFLAGS belongs to whoever is building and
# packaging replaces it wholesale; on the command line it overrides even a
# `+=`, so flags that have to survive that are kept in their own variable and
# passed after it. _FORTIFY_SOURCE is undefined first because the toolchain may
# already have set it, and redefining it is a warning. Note it needs an -O of
# some kind to do anything, which is what CFLAGS defaults to.
REQ_CFLAGS  := -Wall -Wextra -fstack-protector-strong \
               -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
REQ_LDFLAGS := -Wl,-z,relro,-z,now

# Which theme's glyphs to check, and the font to check them against. FONT is
# empty by default, meaning the one that theme asks for -- set it to check a
# face you are thinking of putting in XSECURELOCK_FONT instead. Only used by
# check.
FONT  ?=
THEME ?= inferno

.PHONY: all test test-wrapper check install uninstall clean

all: shim.so

shim.so: src/shim.c
	$(CC) -shared -fPIC $(CFLAGS) $(REQ_CFLAGS) $(LDFLAGS) $(REQ_LDFLAGS) \
	    -o $@ $< $(LDLIBS)

# Everything that can be checked without an X server: which strings get
# rewritten, and the invariants the layout and the length-hiding depend on.
test: test-wrapper src/shim.c
	@$(CC) -DLASCIATE_SELFTEST $(CFLAGS) $(REQ_CFLAGS) -o .selftest src/shim.c $(LDLIBS)
	@./.selftest; rc=$$?; rm -f .selftest; exit $$rc

# The wrapper's own failure modes, which the C self-test cannot reach. Both are
# about the same thing: the screen has to end up locked, and when it cannot be,
# that has to be said somewhere a person will see it. Run against a stand-in
# xsecurelock in a temporary tree, so nothing here touches a real lock.
test-wrapper:
	@d=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$d"' EXIT INT TERM; \
	mkdir -p "$$d/bin" "$$d/cfg/lasciate/themes" "$$d/home" "$$d/so"; \
	chmod 700 "$$d/so"; \
	printf '#!/bin/sh\necho LOCKED\nenv\n' > "$$d/bin/xsecurelock"; \
	chmod 755 "$$d/bin/xsecurelock"; \
	probe="$$d/cfg/lasciate/themes/probe"; \
	rc=0; \
	run() { env -u LASCIATE_SHIM -u XDG_DATA_HOME -u LD_PRELOAD -u LASCIATE_THEME \
	    HOME="$$d/home" XDG_CONFIG_HOME="$$d/cfg" PATH="$$d/bin:$$PATH" "$$@" \
	    ./bin/lasciate; }; \
	locks() { \
	    printf '%s\n' "$$2" > "$$probe"; \
	    if run LASCIATE_THEME=probe $$3 2>/dev/null | grep -q LOCKED; then \
	        printf "  ok    locks anyway, %s\n" "$$1"; \
	    else \
	        printf "  FAIL  NOT LOCKED: %s\n" "$$1"; rc=1; \
	    fi; \
	}; \
	locks "theme line leaves a false status" '[ -n "" ] && Y=1'; \
	locks "theme names a command that is not there" 'notacommand'; \
	locks "theme calls exit 0" 'exit 0'; \
	locks "theme calls exit 1" 'exit 1'; \
	locks "theme makes a variable readonly" 'readonly LASCIATE_PROMPT=x'; \
	locks "theme sets IFS" 'IFS=:'; \
	locks "theme shadows echo, then exits" 'echo() { exit 0; }; exit 3'; \
	locks "theme shadows logger, then exits" 'logger() { exit 0; }; exit 3'; \
	printf 'export LD_PRELOAD=/nope.so; readonly LD_PRELOAD; exit 1\n' > "$$probe"; \
	if run LASCIATE_THEME=probe 2>/dev/null | grep -q LOCKED; then \
	    printf "  !!    readonly LD_PRELOAD now locks -- delete this case\n"; \
	else \
	    printf "  ok    known limit: readonly LD_PRELOAD defeats the failsafe\n"; \
	fi; \
	: > "$$probe"; \
	if env -u HOME -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u LASCIATE_SHIM \
	   PATH="$$d/bin:$$PATH" ./bin/lasciate 2>/dev/null | grep -q LOCKED; then \
	    printf "  ok    locks with HOME and the XDG vars unset\n"; \
	else \
	    printf "  FAIL  unset HOME stopped the lock\n"; rc=1; \
	fi; \
	if XDG_CONFIG_HOME="$$d/cfg" PATH=/nonexistent ./bin/lasciate 2>&1 \
	   | grep -q 'NOT LOCKED'; then \
	    printf "  ok    says so when xsecurelock is missing\n"; \
	else \
	    printf "  FAIL  missing xsecurelock went unreported\n"; rc=1; \
	fi; \
	if run 2>/dev/null | grep -q '^LASCIATE_PROMPT=RELINQVITE'; then \
	    printf "  ok    inferno is the look with no theme named\n"; \
	else \
	    printf "  FAIL  the default theme was not loaded\n"; rc=1; \
	fi; \
	if run LASCIATE_THEME=matrix 2>/dev/null \
	   | grep -q '^LASCIATE_PROMPT=Wake up, Neo'; then \
	    printf "  ok    a named theme is read from themes/\n"; \
	else \
	    printf "  FAIL  LASCIATE_THEME=matrix did not load themes/matrix\n"; rc=1; \
	fi; \
	if run LASCIATE_THEME=matrix LASCIATE_PROMPT=IGNOREME 2>/dev/null \
	   | grep -q '^LASCIATE_PROMPT=Wake up, Neo'; then \
	    printf "  ok    a theme wins over a variable it names\n"; \
	else \
	    printf "  FAIL  LASCIATE_PROMPT survived LASCIATE_THEME=matrix\n"; rc=1; \
	fi; \
	if run LASCIATE_THEME=matrix LASCIATE_NO_GLYPHS=1 2>/dev/null \
	   | grep -q '^LASCIATE_NO_GLYPHS=1'; then \
	    printf "  ok    a theme leaves alone what it does not name\n"; \
	else \
	    printf "  FAIL  a theme took a variable it never mentions\n"; rc=1; \
	fi; \
	printf 'LASCIATE_PROMPT="MINE"\n' > "$$d/cfg/lasciate/themes/matrix"; \
	if run LASCIATE_THEME=matrix 2>/dev/null | grep -q '^LASCIATE_PROMPT=MINE'; then \
	    printf "  ok    a theme of yours shadows the one that shipped\n"; \
	else \
	    printf "  FAIL  the shipped theme won over yours\n"; rc=1; \
	fi; \
	if run LASCIATE_THEME="$$d/cfg/lasciate/themes/matrix" 2>/dev/null \
	   | grep -q '^LASCIATE_PROMPT=MINE'; then \
	    printf "  ok    a theme given as a path is read from there\n"; \
	else \
	    printf "  FAIL  a theme path was not honoured\n"; rc=1; \
	fi; \
	rm -f "$$d/cfg/lasciate/themes/matrix"; \
	if run LASCIATE_THEME=nosuchtheme 2>/dev/null \
	   | grep -q '^LASCIATE_PROMPT=RELINQVITE'; then \
	    printf "  ok    an unknown theme falls back to inferno\n"; \
	else \
	    printf "  FAIL  an unknown theme left no look at all\n"; rc=1; \
	fi; \
	if run LASCIATE_THEME=nosuchtheme 2>&1 | grep -q "no theme called"; then \
	    printf "  ok    an unknown theme says so, and locks anyway\n"; \
	else \
	    printf "  FAIL  an unknown theme went unreported\n"; rc=1; \
	fi; \
	if run LASCIATE_NO_GLYPHS=1 2>/dev/null | grep -q '^LASCIATE_NO_GLYPHS=1'; then \
	    printf "  ok    the environment carries what no theme names\n"; \
	else \
	    printf "  FAIL  a variable no theme sets was lost\n"; rc=1; \
	fi; \
	printf 'void x(void);\n' > "$$d/so/ok.c"; \
	$(CC) -shared -fPIC -o "$$d/so/plain.so" "$$d/so/ok.c" 2>/dev/null; \
	printf 'int gone(void); int hook(void){return gone();}\n' > "$$d/so/bad.c"; \
	$(CC) -shared -fPIC -Wl,-z,now -o "$$d/so/bad.so" "$$d/so/bad.c" 2>/dev/null; \
	printf 'preload=%s\n' "$$d/so/plain.so" > "$$probe"; \
	if run LASCIATE_THEME=probe 2>/dev/null | grep -q "^LD_PRELOAD=$$d/so/plain.so"; then \
	    printf "  FAIL  a theme wrote straight into the preload\n"; rc=1; \
	else \
	    printf "  ok    a theme cannot write into the preload\n"; \
	fi; \
	locks "theme sets an LD_PRELOAD that cannot load" \
	      "export LD_PRELOAD=$$d/so/bad.so"; \
	locks "that, plus an early exit" \
	      "export LD_PRELOAD=$$d/so/bad.so; exit 1"; \
	: > "$$probe"; \
	if run LD_PRELOAD="$$d/so/plain.so" 2>/dev/null \
	   | grep -q "^LD_PRELOAD=$$d/so/plain.so"; then \
	    printf "  ok    an inherited LD_PRELOAD is passed through\n"; \
	else \
	    printf "  FAIL  lost an inherited LD_PRELOAD\n"; rc=1; \
	fi; \
	mkfifo "$$d/so/fifo.so" 2>/dev/null; \
	if timeout 10 env XDG_CONFIG_HOME="$$d/cfg" PATH="$$d/bin:$$PATH" \
	   LASCIATE_SHIM="$$d/so/fifo.so" ./bin/lasciate 2>/dev/null | grep -q LOCKED; then \
	    printf "  ok    a fifo named as the shim does not hang the lock\n"; \
	else \
	    printf "  FAIL  a fifo named as the shim hung or stopped the lock\n"; rc=1; \
	fi; \
	if run LASCIATE_SHIM="$$d/so/bad.so" 2>/dev/null | grep -q '^LD_PRELOAD='; then \
	    printf "  FAIL  preloaded a shim that cannot load\n"; rc=1; \
	else \
	    printf "  ok    refuses to preload a shim that cannot load\n"; \
	fi; \
	exit $$rc

# The default glyphs are only safe if one face covers all of them. A glyph that
# falls back to another font arrives at a different width and breaks the run,
# which is the failure mode worth catching before you see it on a locked
# screen. Needs fontconfig.
#
# fc-match rather than fc-list, because the question is which face gets used
# and not which faces exist.
# The font may be an alias or a pattern rather than a family -- both themes ask
# for one -- and no family is named after an alias, so matching family names
# against it reports every glyph missing on a machine that renders all. So the
# alias is resolved to a family first, and the question asked of `fc-list`:
# does any installed file of that family carry this codepoint? `fc-match` will
# not answer it -- a `:charset=` constraint re-scores the whole match and it
# happily names a different family that has the glyph, which reads as a
# fallback even when the font asked for carries it too.
#
# THEME picks which set. matrix's digits are left out: anything that can render
# the inscription has ASCII, and it is the katakana that a font is likely to be
# missing.
check:
	@command -v fc-match >/dev/null || { echo "fc-match not found"; exit 1; }
	@t=""; \
	for d in themes "$${XDG_CONFIG_HOME:-$$HOME/.config}/lasciate/themes" \
	         "$${XDG_DATA_HOME:-$$HOME/.local/share}/lasciate/themes"; do \
	    [ -r "$$d/$(THEME)" ] && { t="$$d/$(THEME)"; break; }; \
	done; \
	case "$(THEME)" in */*) [ -r "$(THEME)" ] && t="$(THEME)";; esac; \
	[ -n "$$t" ] || { echo "no theme '$(THEME)'"; exit 1; }; \
	echo "$(THEME) ($$t)"; \
	themefont=$$(. "$$t"; printf '%s' "$${XSECURELOCK_FONT:-monospace}"); \
	glyphs=$$(. "$$t"; printf '%s' "$$LASCIATE_BASES$$LASCIATE_MARKS"); \
	font="$(FONT)"; [ -n "$$font" ] || font="$$themefont"; \
	face=$$(fc-match --format='%{family}' "$$font" | cut -d, -f1); \
	echo "'$$font' resolves to: $$face"; \
	cps=$$(printf '%s' "$$glyphs" | iconv -f UTF-8 -t UTF-32LE | od -An -tx4 -v \
	    | tr ' ' '\n' | grep . | while read -r c; do \
	        printf '%04X ' "0x$$c"; done); \
	missing=0; checked=0; \
	for cp in $$cps; do \
	    [ "$$cp" = 0020 ] && continue; \
	    checked=$$((checked + 1)); \
	    if fc-list ":charset=$$cp" family 2>/dev/null | grep -qF "$$face"; then \
	        printf "  U+%s ok\n" "$$cp"; \
	    else \
	        printf "  U+%s MISSING -- %s does not have it\n" "$$cp" "$$face"; \
	        missing=1; \
	    fi; \
	done; \
	[ "$$checked" -gt 0 ] || { echo "  this theme draws no glyphs of its own"; exit 0; }; \
	if [ $$missing -ne 0 ]; then \
	    echo; \
	    echo "faces on this machine that cover the whole set:"; \
	    mono=$$(fc-list ":charset=$$cps:spacing=100" family 2>/dev/null \
	        | tr ',' '\n' | sort -u); \
	    all=$$(fc-list ":charset=$$cps" family 2>/dev/null | tr ',' '\n' | sort -u); \
	    if [ -z "$$all" ]; then \
	        echo "  none -- no installed font has all of these glyphs;"; \
	        echo "  set LASCIATE_BASES to glyphs a font here does have"; \
	    else \
	        printf '%s\n' "$$all" | head -12 | while IFS= read -r fam; do \
	            [ -n "$$fam" ] || continue; \
	            if printf '%s\n' "$$mono" | grep -qxF "$$fam"; then \
	                printf "  %-38s equal advances\n" "$$fam"; \
	            else \
	                printf "  %-38s proportional\n" "$$fam"; \
	            fi; \
	        done; \
	        n=$$(printf '%s\n' "$$all" | grep -c .); \
	        [ "$$n" -gt 12 ] && printf "  ... and %d more\n" $$((n - 12)); \
	        echo; \
	        echo "put one in XSECURELOCK_FONT, or try it first with"; \
	        echo "  make check THEME=$(THEME) FONT='<family>'"; \
	        echo "a proportional face needs bases that happen to advance alike,"; \
	        echo "or every keystroke redraws the run at a different width."; \
	    fi; \
	    exit 1; \
	fi

# shim.so goes in 644, not 664: it is loaded into the process that handles your
# password, so nothing but you should be able to write it.
install: shim.so
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR) $(DESTDIR)$(DATADIR)/themes
	install -m 755 bin/lasciate $(DESTDIR)$(BINDIR)/lasciate
	install -m 644 shim.so $(DESTDIR)$(DATADIR)/shim.so
	install -m 644 themes/inferno themes/matrix themes/moria \
	    $(DESTDIR)$(DATADIR)/themes/
	@echo
	@echo "installed. point your locker at $(BINDIR)/lasciate, e.g."
	@echo "  xss-lock --transfer-sleep-lock -- lasciate"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lasciate $(DESTDIR)$(DATADIR)/shim.so \
	      $(DESTDIR)$(DATADIR)/themes/inferno $(DESTDIR)$(DATADIR)/themes/matrix \
	      $(DESTDIR)$(DATADIR)/themes/moria
	-rmdir $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(DATADIR) 2>/dev/null || true

clean:
	rm -f shim.so .selftest
