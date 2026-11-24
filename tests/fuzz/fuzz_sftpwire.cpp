// The SFTP v3 replies putgd-sftp-worker reads from a server through ssh (#1871): the bytes
// arrive split at arbitrary points, and every length in them comes from the server.
#include "FuzzSupport.h"
#include "storage/sftp/SftpWire.h"
#include <cstdlib>
using namespace putgd;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    sftp::wire::Framer framer;
    QString error;
    while (in.remaining()) {
        framer.append(in.part());
        while (auto packet = framer.next(&error)) {
            if (packet->size() < 1 || quint32(packet->size()) > sftp::wire::maximumPacket) std::abort();
            if (const auto reply = sftp::wire::parse(*packet, &error)) {
                if (reply->names.size() > sftp::wire::maximumNames || reply->extensions.size() > sftp::wire::maximumExtensions) std::abort();
                if (reply->type == sftp::wire::ExtendedReply) {
                    sftp::wire::fileSystem(reply->data);
                    sftp::wire::limits(reply->data);
                }
            }
        }
        if (!error.isEmpty()) break;
    }
    return 0;
}
