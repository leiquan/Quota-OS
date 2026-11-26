#pragma once

class SkPixmap;

// The caller owns the window descriptor and keeps the bitmap alive for the call.
// Returns zero after the desktop accepts every row, or a negative errno.
int quota_publish_pixmap(int window_fd, const SkPixmap& pixels);
