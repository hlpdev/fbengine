// On Windows, starting the audio leaves the main thread's COM apartment-threaded, so SDL's
// OleInitialize, which a window's drag and drop needs, still works after it: a game can start its
// audio before its window. Through fbengine_audio_native's C API, on the null backend.

#include "audio/fbe_audio.h"

#include <catch2/catch_test_macros.hpp>
#include <objbase.h>
#include <ole2.h>

TEST_CASE("OLE starts on the thread that started the audio") {
    REQUIRE(fbe_audio_start(1, 0, 0, nullptr, 0.0));
    const HRESULT ole = OleInitialize(nullptr);
    CHECK(ole != RPC_E_CHANGED_MODE);
    CHECK(SUCCEEDED(ole));
    if (SUCCEEDED(ole)) {
        OleUninitialize();
    }
    fbe_audio_stop();
}
