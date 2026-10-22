#pragma once
// Entry points of the helper modes compiled into putgd.exe. main() dispatches on
// the first argument; each helper owns its own windows, COM/GDI+ initialization
// and exits when its work is done. Arguments are UTF-8.
#include <string>
#include <vector>

namespace putgd {
int residentMain(const std::vector<std::string>& args);   // resident/Resident.cpp
int captureMain(const std::vector<std::string>& args);    // capture/CaptureMain.cpp  (--capture)
int imageJobMain(const std::vector<std::string>& args);   // capture/ImageJob.cpp     (--image-job)
int s3WorkerMain(const std::vector<std::string>& args);   // s3/S3Worker.cpp          (--s3-worker)
int uiMain(const std::vector<std::string>& args);         // ui/UiMain.cpp            (--ui VIEW)
int editorMain(const std::vector<std::string>& args);     // editor/EditorMain.cpp    (--edit-image)
int recordMain(const std::vector<std::string>& args);     // record/RecordMain.cpp    (--record)
}
