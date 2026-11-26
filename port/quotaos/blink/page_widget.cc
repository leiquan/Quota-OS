#include "page_widget.h"

#include "third_party/blink/public/web/web_local_frame.h"

#include <utility>

blink::WebFrameWidget* QuotaPageWidget::Create(blink::WebLocalFrame& frame,
                                              const viz::FrameSinkId& id) {
    mojo::AssociatedRemote<blink::mojom::FrameWidgetHost> frame_host;
    mojo::AssociatedRemote<blink::mojom::WidgetHost> widget_host;
    frame_host_ = frame_host.BindNewEndpointAndPassDedicatedReceiver();
    widget_host_ = widget_host.BindNewEndpointAndPassDedicatedReceiver();
    return frame.InitializeFrameWidget(
        frame_host.Unbind(), frame_remote_.BindNewEndpointAndPassDedicatedReceiver(),
        widget_host.Unbind(), widget_remote_.BindNewEndpointAndPassDedicatedReceiver(), id);
}

bool QuotaPageWidget::TakeRedrawRequest() {
    return std::exchange(redraw_pending_, false);
}
