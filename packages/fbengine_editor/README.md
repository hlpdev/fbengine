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

The asset browser shows res:// as a tree, thumbnails coming a few a frame; a click selects, showing
what a file's imported as beside the list, each change a line in its sidecar, undone with Control Z
while the Assets pane has the keys. A model dragged to the scene goes on being its file's. Scenes are
files, as docs/scene-files.md says: New, Open, Save, and Save As, in File.

The inspector shows what's selected: what's every entity's, then what's particular to its kind when
all that's selected is of it, then its material's. Each property is what has both an X() and a SetX(),
found by reflection, and a setter's own attributes (`@Limits`, `@ShownInDegrees`, `@NotEdited`) say
what it takes. What's typed takes on Return or going elsewhere, Escape leaves it as it was, and each is
one undo. What differs between what's selected shows nothing, and what's typed there goes to all of it,
that part of it alone.

A game's components show there too: Classes of its own, kept on an entity as `Entity.Data<T>()` gives
back, each edited as data, by what its Public Fields are. The editor can't run a game's code in itself,
so a game's `components/main.fb` says what kinds it has, and the editor builds and runs it, by `fb` on the
PATH, as a project opens, once you've said that folder's code may run, and asks once per folder; File's
Reload Components reads them again:

```
Import fbengine
Import fbengine_physics
Import "../door"

Components.Add<Door>("Door")
AddBodyComponent()
Try Components.Describe()
```

A game with no such program just has none. `@Limits` and `@ShownInDegrees` on a Field say what they
say on a setter, of each number of a vector too, and `@Skip` leaves one out. What's edited is a Bool, a
number, a String, an Enum whose members FutureBlitz's Load can find, a Vec2, a Vec3, or a Color; anything
else shows a note. fbengine_physics's "Body" is one of these. A
game's `Scene.Load` puts each on its entity, as `Entity.Data<T>()` gives it, so it adds its kinds first; what
can't be attached is an error that names both.

`-- --watch` plays a scene of its own by itself, keeping nothing of yours, for anyone to look at (`python
tools/show.py ../packages/fbengine_editor`).

Play's F5 builds the game with `fb` and runs its program, in a process of its own, on the scene as it's
saved, saving what's changed first, so nothing a run does touches what's being edited. Its folder is asked
about once, the same yes as for its components. A program finds the scene it's asked for by
`Scene.Requested()`, from `--scene`, which refuses what isn't a res:// path. What it says shows in the Output pane; Shift F5 stops it, as playing
again, closing its project, and quitting do. A game's own processes, and a run left by the editor crashing,
aren't stopped.
