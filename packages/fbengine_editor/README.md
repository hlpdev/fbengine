# fbengine_editor

fbengine's editor: a program on its packages that opens a game's project folder.

```
fb run packages/fbengine_editor -- <a game's folder>
```

Its panes dock and resize, what's open in a project is kept between runs, in the user's folder for
settings and not in the project, and everything it does is a command in one list, which its menus
and keys run, with undo and redo. The scene view, the tree, the inspector, the asset browser, and
playing the game come in the steps after this one.
