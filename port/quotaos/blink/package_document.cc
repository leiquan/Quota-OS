#include "package_document.h"

#include "base/files/scoped_file.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_navigation_params.h"

#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <string>
#include <utility>
#include <unistd.h>

int quota_load_document(blink::WebNavigationControl& main_frame,
                        const blink::WebURL& document_url) {
    base::ScopedFD file(open("/app/app.html", O_RDONLY | O_CLOEXEC));
    if (!file.is_valid())
        return -errno;

    std::string html;
    char chunk[1024];
    for (;;) {
        ssize_t count = read(file.get(), chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -errno;
        if (count == 0)
            break;
        html.append(chunk, static_cast<size_t>(count));
    }

    auto params = std::make_unique<blink::WebNavigationParams>();
    params->url = document_url;
    blink::WebNavigationParams::FillStaticResponse(
        params.get(), "text/html", "UTF-8", base::span<const char>(html));
    main_frame.CommitNavigation(std::move(params), nullptr);
    return 0;
}
