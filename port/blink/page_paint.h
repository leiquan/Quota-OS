#pragma once

class SkSurface;
namespace blink {
class LocalFrameView;
}

// Called on the page thread with the live local root and its reusable raster surface.
int quota_paint_page(blink::LocalFrameView& root, SkSurface& surface, int window_fd);
