#include "window_surface.h"

#include "include/guest_page_services.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"

#include <cerrno>
#include <cstdint>
#include <unistd.h>

int quota_publish_pixmap(int window_fd, const SkPixmap& pixels) {
    if (!pixels.addr() || pixels.width() != GUEST_PREVIEW_WIDTH ||
        pixels.height() != GUEST_PREVIEW_HEIGHT)
        return -EINVAL;

    uint32_t row[GUEST_PREVIEW_WIDTH];
    const auto info = SkImageInfo::Make(GUEST_PREVIEW_WIDTH, 1,
                                        kBGRA_8888_SkColorType, kPremul_SkAlphaType,
                                        SkColorSpace::MakeSRGB());
    for (int y = 0; y < pixels.height(); ++y) {
        if (!pixels.readPixels(info, row, sizeof(row), 0, y))
            return -EINVAL;
        ssize_t written;
        do {
            written = write(window_fd, row, sizeof(row));
        } while (written < 0 && errno == EINTR);
        if (written != sizeof(row))
            return written < 0 ? -errno : -EIO;
    }
    return 0;
}
