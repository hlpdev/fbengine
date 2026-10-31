# fbengine_ui

fbengine's UI, written in FutureBlitz on fbengine's 2D canvas and text: a tree of nodes laid out
in rows, columns, and stacks with anchors, styled by themes, with widgets, focus by keyboard and
gamepad, a debug UI, and a console.

```toml
[dependencies]
fbengine = "0.1"
fbengine_ui = "0.1"
```

```vb
Import std.core
Import std.math
Import fbengine_ui

// A menu: a column of buttons in the middle of the screen.
Local screen = Node("screen")
screen.SetArrangement(Arrangement.STACK)
Local menu = Node("menu")
menu.SetArrangement(Arrangement.COLUMN)
menu.SetGap(8)
menu.SetAnchors(Anchors.Point(0.5, 0.5))
screen.Add(menu)
screen.Layout(Rect(0, 0, 1280, 720))
```

Released with fbengine at the same version. Apache-2.0.
