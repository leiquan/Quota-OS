#include "page_paint.h"

#include "include/guest_page_services.h"
#include "window_surface.h"
#include "cc/paint/paint_record.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"

#include <cerrno>

int quota_paint_page(blink::LocalFrameView& root, SkSurface& surface, int window_fd) {
    if (surface.width() != GUEST_PREVIEW_WIDTH || surface.height() != GUEST_PREVIEW_HEIGHT)
        return -EINVAL;
    if (!root.UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kBeginMainFrame))
        return -EAGAIN;

    SkCanvas* canvas = surface.getCanvas();
    canvas->clear(SK_ColorWHITE);
    root.GetPaintRecord().Playback(canvas);
    SkPixmap pixels;
    if (!surface.peekPixels(&pixels))
        return -EIO;
    return quota_publish_pixmap(window_fd, pixels);
}
