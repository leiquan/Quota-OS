#pragma once

namespace blink {
class WebNavigationControl;
class WebURL;
}

// Submits the selected package document to the main frame. Returns a negative
// errno on file errors; Blink notifies the frame client when loading completes.
int quota_load_document(blink::WebNavigationControl& main_frame,
                        const blink::WebURL& document_url);
