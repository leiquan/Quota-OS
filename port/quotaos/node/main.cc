#include <cstdio>

#include "node.h"
#include "v8-initialization.h"

int main(int argc, char** argv) {
    if (!v8::V8::InitializeICUDefaultLocation(
            argv[0], "/usr/lib/quota-runtime/icudtl.dat")) {
        std::fputs("Node: unable to initialize system ICU data\n", stderr);
        return 1;
    }
    v8::V8::InitializeExternalStartupDataFromFile(
        "/usr/lib/quota-runtime/snapshot_blob.bin");
    return node::Start(argc, argv);
}
