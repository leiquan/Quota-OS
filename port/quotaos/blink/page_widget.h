#pragma once

#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/mojom/page/widget.mojom.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom.h"
#include "third_party/blink/public/web/web_non_composited_widget_client.h"
#include "third_party/blink/public/web/web_view_client.h"

namespace blink {
class WebFrameWidget;
class WebLocalFrame;
}
namespace viz {
class FrameSinkId;
}

// One instance per local root. It outlives the frame's non-composited widget.
class QuotaPageWidget final : public blink::WebNonCompositedWidgetClient,
                              public blink::WebViewClient {
public:
    blink::WebFrameWidget* Create(blink::WebLocalFrame& frame, const viz::FrameSinkId& id);
    void ScheduleNonCompositedAnimation() override { redraw_pending_ = true; }
    void InvalidateContainer() override { redraw_pending_ = true; }
    bool TakeRedrawRequest();

private:
    bool redraw_pending_ = true;
    mojo::AssociatedRemote<blink::mojom::FrameWidget> frame_remote_;
    mojo::AssociatedRemote<blink::mojom::Widget> widget_remote_;
    mojo::PendingAssociatedReceiver<blink::mojom::FrameWidgetHost> frame_host_;
    mojo::PendingAssociatedReceiver<blink::mojom::WidgetHost> widget_host_;
};
