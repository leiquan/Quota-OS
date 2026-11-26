#pragma once

namespace blink {
class WebWidget;
}

// Returns 1 after dispatch, 0 for an empty queue, or a negative errno.
int quota_read_window_input(int window_fd, blink::WebWidget& widget);
