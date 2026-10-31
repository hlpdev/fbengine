#ifndef FBE_APP_H
#define FBE_APP_H

// The app's own calls to the OS: its message box, opening links and files, the clipboard, and the
// player's languages.
//
// Neither opens anything headless (fbe_window_set_headless, or SDL's video driver set to
// "offscreen" or "dummy", as tests run): a message goes to stderr, and a link is kept, for tests
// to read.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a message box says it is.
#define FBE_APP_MESSAGE_ERROR       0
#define FBE_APP_MESSAGE_WARNING     1
#define FBE_APP_MESSAGE_INFORMATION 2

// Shows the OS's message box, over the window if it's open, and waits for the player to close
// it; headless, or if the OS can't show it, writes "title: text" to stderr instead. False for a
// kind that isn't one of FBE_APP_MESSAGE_.
FBE_API bool fbe_app_show_message(const char* title, const char* text, int32_t kind);

// Opens a link in the OS's browser, or a file or folder, by its path, in the app the OS opens it
// with; a path that isn't absolute is from the current folder (on Windows, the OS's own full
// path, which resolves a drive's own folder too). False if the OS can't. Headless, it opens
// nothing, and keeps the link, a path as its file:// link, which the others give: on Windows the
// shell's own, UrlCreateFromPathW's, and elsewhere RFC 8089's, its UTF-8 in %XX.
FBE_API bool fbe_app_open_url(const char* url);
// The links fbe_app_open_url kept, headless, the first first; "" for no such one.
FBE_API int32_t fbe_app_opened_count(void);
FBE_API const char* fbe_app_opened_at(int32_t index);

// Puts `text`, UTF-8, on the OS's clipboard, which SDL's video holds while the OS has none, as
// headless; false if it can't. It starts the video if nothing has.
FBE_API bool fbe_app_set_clipboard(const char* text);
// The text on the clipboard, "" for none.
FBE_API const char* fbe_app_clipboard(void);

// The player's languages, as the OS prefers them, the first most, each a BCP 47 tag in its usual
// case, as "en-US" or "fr": the count reads them again, and the index is into what it read; ""
// for no such one.
FBE_API int32_t fbe_app_language_count(void);
FBE_API const char* fbe_app_language_at(int32_t index);

#ifdef __cplusplus
}
#endif

#endif
