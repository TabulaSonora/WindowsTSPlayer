// Exports a song to WAV through the host layer's own export path, so the result can be compared
// byte for byte against `tabula-sonora render`.
//
// This is the strong check on the port: the export runs Session::plan_export and Session::run_export
// -- a second ToneGenerator over the same NoteRenderer, the same arrangement the Apple build uses --
// so if the vendored session changed any audio behaviour, the bytes will differ.
//
//   ts-export-tool <rom> <midi> <out.wav>

#include "host/ts_session.hpp"

#include <cstdio>
#include <exception>
#include <string>

int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <rom> <midi> <out.wav>\n", argv[0]);
        return 2;
    }

    try {
        ts::host::Session session;

        // Quick verification: the caller has already established which file this is, and hashing
        // 27 MB on every test run buys nothing.
        session.load_rom(argv[1], false);
        session.load_song(argv[2]);

        const auto plan = session.plan_export();
        ts::host::Session::run_export(plan, argv[3], [](double) { return true; });
    } catch (const std::exception& error) {
        std::fprintf(stderr, "export failed: %s\n", error.what());
        return 1;
    }

    return 0;
}
