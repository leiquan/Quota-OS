#pragma once

#include "third_party/blink/public/web/web_local_frame_client.h"

namespace blink {
class WebFrameWidget;
class WebNonCompositedWidgetClient;
class WebView;
}

class QuotaPageFrameClient final : public blink::WebLocalFrameClient {
public:
    explicit QuotaPageFrameClient(bool rpc_enabled) : rpc_enabled_(rpc_enabled) {}
    void DidClearWindowObject() override;
    void BindToFrame(blink::WebNavigationControl* frame) override;
    void FrameDetached(blink::DetachReason reason) override;
    void DidFinishLoad() override;

    int AttachAndLoad(blink::WebView& view, blink::WebFrameWidget& widget,
                      blink::WebNonCompositedWidgetClient& window_client,
                      const blink::WebURL& document_url);
    bool attached() const { return frame_ != nullptr; }
    bool loaded() const { return loaded_; }

private:
    blink::WebNavigationControl* frame_ = nullptr;
    bool loaded_ = false;
    bool rpc_enabled_;
};
