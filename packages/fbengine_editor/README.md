# fbengine_editor

fbengine's editor: a program on its packages that opens a game's project folder.

```
fb run packages/fbengine_editor -- <a game's folder>
```

Its panes dock and resize, what's open in a project is kept between runs, in the user's folder for
settings and not in the project, and everything it does is a command in one list, which its menus
and keys run, with undo and redo.

In the scene view, the right button looks around, while W, A, S, D, Q, and E fly, faster with Shift;
the middle button orbits what it's looking at, and with Shift, pans; the wheel dollies. The left
button selects what's under it, with Shift adds it to that or takes it out, or drags a handle: W moves, E turns, R
scales, X switches between world and local axes, G snaps, or Control does the other way for a drag,
and F looks at what's selected.

In the tree, a click selects, with Shift adds to that, and a drag takes what's selected to under what
it's let go on, or to the top of the scene on nothing. F2, or a double click, renames, Control D
duplicates, H hides or shows, Delete takes out, and Control F finds by name, each through undo.

The inspector, the asset browser, scenes on disk, and playing the game come in the steps after this
one.
