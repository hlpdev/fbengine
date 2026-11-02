# fbengine_ui

fbengine's UI, written in FutureBlitz on fbengine's 2D canvas and text: a tree of nodes laid out
in rows, columns, and stacks with anchors, styled by themes, with widgets, focus by keyboard and
gamepad, a debug UI, a console, and its text in the player's language.

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
Local play = Button("Play")
menu.Add(play)
Local ui = Ui(screen)

// Each frame: the input, then what its widgets read, then what it draws over what the game draws.
Try Graphics(1280, 720)
While Window.Open()
    Flip()
    ui.Update()
    If play.Clicked() Then Print("playing")
    Screen.Canvas().Clear()
    ui.Draw()
End While
```

A widget of a game's own overrides Draw, and draws in its box through the Painter it's given, in the
UI's units, on whole pixels of the screen. Styles name a widget's parts, as `slider.thumb`, with the
theme; `Theme.Dark` has them all. With no font, the first Draw takes the engine's own, Font.BuiltIn.

Released with fbengine at the same version. Apache-2.0.
