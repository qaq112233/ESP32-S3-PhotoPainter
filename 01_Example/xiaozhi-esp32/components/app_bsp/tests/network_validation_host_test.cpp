#include "../server_protocol.h"

#include <assert.h>
#include <stddef.h>

using namespace photopainter_server;

int main() {
    assert(IsStaticUri("/"));
    assert(IsStaticUri("/index.html"));
    assert(IsStaticUri("/script.min.js"));
    assert(!IsStaticUri("/photopull.json"));
    assert(!IsStaticUri("/../photopull.json"));
    assert(!IsStaticUri("/index.html?download=config"));
    assert(StaticStoragePath("/") == nullptr);
    assert(StaticStoragePath("/index.html") != nullptr);
    assert(!IsUploadContentLengthValid(0));
    assert(!IsUploadContentLengthValid(1));
    assert(IsUploadContentLengthValid(2));
    assert(IsUploadContentLengthValid(kUploadBmpMax + 1));
    assert(!IsUploadContentLengthValid(kUploadBmpMax + 2));

    /* The mode byte may arrive in a chunk by itself. */
    UploadFraming framing(1 + 1152054);
    size_t offset = 0;
    size_t payload = 0;
    assert(framing.first_chunk());
    assert(framing.Consume(1, &offset, &payload));
    assert(offset == 1 && payload == 0);
    assert(!framing.first_chunk());
    assert(framing.Consume(4096, &offset, &payload));
    assert(offset == 0 && payload == 4096);
    assert(framing.remaining() == 1152054 - 4096);
    assert(!framing.Consume(framing.remaining() + 1, &offset, &payload));
    assert(framing.Consume(framing.remaining(), &offset, &payload));
    assert(framing.remaining() == 0);
    assert(!framing.Consume(1, &offset, &payload));
    return 0;
}
