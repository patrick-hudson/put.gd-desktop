// putgd.exe: the resident, or one of its helper modes (see common/Modes.h).
#include "common/BuildInfo.h"
#include "common/Frames.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "resident/UpdateService.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(putgd::paths::narrow(argv[i]));
    const std::string mode = args.empty() ? std::string() : args[0];
    if (mode == "--version-json") {
        std::printf("%s\n", putgd::json::serialize(putgd::protocol::buildIdentity()).c_str());
        return 0;
    }
    if (mode == "--version") { std::printf("putgd %s\n", putgd::build::version); return 0; }
    if (mode == "--capture") return putgd::captureMain(args);
    if (mode == "--image-job") return putgd::imageJobMain(args);
    if (mode == "--s3-worker") return putgd::s3WorkerMain(args);
    if (mode == "--ui") return putgd::uiMain(args);
    if (mode == "--edit-image") return putgd::editorMain(args);
    if (mode == "--record") return putgd::recordMain(args);
    if (mode == "--check-update") return putgd::UpdateService::childMain(args);
    return putgd::residentMain(args);
}
// MinGW -municode and MSVC /SUBSYSTEM:WINDOWS both reach wmain through wWinMain.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int code = wmain(argc, argv);
    LocalFree(argv);
    return code;
}
