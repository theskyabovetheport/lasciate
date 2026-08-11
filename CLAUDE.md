# lasciate — editing contract

What this is and why it works the way it does: `README.md`. Rationale lives at
the code it governs. This file holds only what the code cannot say about
itself.

| file | role |
|---|---|
| `bin/lasciate` | wrapper: theme, LD_PRELOAD, exec |
| `src/shim.c` | the preload: four hooks, tamper flag |
| `themes/*` | a look, one shell fragment each |
| `Makefile` | build, install, `test`, `check` |

## Never reintroduce

**No history outside git.** No "used to", "no longer", "this replaced", no
changelog sections. A comment explains the code that exists, in the present
tense. History goes in the commit message, at any length.

**Nothing on the lock path may be able to fail.** The EXIT trap runs one fork,
no external command, no branch. `logger` there returns non-zero on a host with
no syslog socket and blocks on a full socket; a redirection on the trap's
subshell swallows the stderr written inside it; `unset` on a readonly variable
ends the shell whatever `|| true` says.

**Do not vet the preload for ownership or mode.** It can only ever guard a
`LASCIATE_SHIM` a theme named, and a theme is code already. `-f` and the
space/colon test stay: they prevent silence, not intrusion.

**Do not put a per-theme list in the Makefile.** `check` reads the theme.

## Fonts

**Coverage does not mean the face draws it.** A font may carry a codepoint as a
placeholder — the character inside a box — and fontconfig reports it present.
Cardo's Runic block is like this. Look at a glyph before adopting a face.

**Width cannot detect tofu in a monospace face.** Every glyph there is the
`.notdef` width, so a width test calls the whole set missing. Compare rendered
pixel signatures instead.

**A combining mark takes a cell.** No shaping engine is linked, so marks are
never placed over the glyph before them, in any font. A run carrying a variable
number of them varies in width and smears, since `auth_x11` clears only the
extents of what it draws. Themes set `LASCIATE_MARKS` to a single space.

**`fc-match FAMILY:charset=X` does not answer "does FAMILY have X".** The
constraint re-scores the match and it names whichever family does. Resolve the
alias to a family, then ask `fc-list ":charset=X"`.

## Tests

    make test               # shim self-test + wrapper, no X needed
    make check THEME=name   # a theme's font covers its glyphs

Nothing here can test rendering. Marks landing, advances holding, a theme
looking like anything at all: only a real lock shows that.
