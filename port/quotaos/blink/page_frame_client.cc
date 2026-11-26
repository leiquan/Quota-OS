#include "page_frame_client.h"

#include "package_document.h"
#include "include/guest_page_services.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "ui/gfx/geometry/size.h"
#include "third_party/blink/public/web/web_navigation_control.h"

#include "base/files/file_util.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/web/web_document.h"
#include "v8/include/v8.h"

#include <unistd.h>
#include <cerrno>
#include <utility>

void QuotaPageFrameClient::BindToFrame(blink::WebNavigationControl* frame) {
    frame_ = frame;
    loaded_ = false;
}

int QuotaPageFrameClient::AttachAndLoad(
    blink::WebView& view, blink::WebFrameWidget& widget,
    blink::WebNonCompositedWidgetClient& window_client,
    const blink::WebURL& document_url) {
    if (!frame_ || widget.LocalRoot() != frame_)
        return -ENODEV;
    loaded_ = false;
    widget.InitializeNonCompositing(&window_client);
    view.DidAttachLocalMainFrame();
    view.GetSettings()->SetJavaScriptEnabled(true);
    widget.Resize(gfx::Size(GUEST_PREVIEW_WIDTH, GUEST_PREVIEW_HEIGHT));
    view.SetIsActive(true);
    view.SetFocusedFrame(frame_);
    widget.SetFocus(true);
    return quota_load_document(*frame_, document_url);
}

void QuotaPageFrameClient::DidFinishLoad() {
    loaded_ = true;
}

void QuotaPageFrameClient::FrameDetached(blink::DetachReason reason) {
    loaded_ = false;
    if (auto* frame = std::exchange(frame_, nullptr))
        frame->Close(reason);
}

namespace {
void WritePacket(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    if (args.Length() != 1 || !args[0]->IsString()) {
        isolate->ThrowError("Expected one RPC packet");
        return;
    }
    v8::String::Utf8Value data(isolate, args[0]);
    if (!*data || !data.length() || data.length() > 1024) {
        isolate->ThrowError("Invalid RPC packet length");
        return;
    }
    ssize_t n;
    do { n = write(3, *data, data.length()); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN) args.GetReturnValue().Set(false);
    else if (n < 0 || static_cast<size_t>(n) != data.length()) isolate->ThrowError("RPC write failed");
    else args.GetReturnValue().Set(true);
}

void ReadPacket(const v8::FunctionCallbackInfo<v8::Value>& args) {
    char data[1024];
    ssize_t n;
    do { n = read(3, data, sizeof(data)); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN) return;
    auto* isolate = args.GetIsolate();
    if (n <= 0) {
        isolate->ThrowError("RPC peer closed");
        return;
    }
    v8::Local<v8::String> result;
    if (v8::String::NewFromUtf8(isolate, data, v8::NewStringType::kNormal, n).ToLocal(&result))
        args.GetReturnValue().Set(result);
}
}  // namespace

void QuotaPageFrameClient::DidClearWindowObject() {
    if (!rpc_enabled_ || !frame_ ||
        frame_->GetDocument().Url().GetString().Utf8() != "file:///app/app.html")
        return;

    std::string source;
    if (!base::ReadFileToStringWithMaxSize(
            base::FilePath("/usr/lib/quota-runtime/page-preload.js"), &source, 32 * 1024))
        return;
    auto* isolate = frame_->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope handles(isolate);
    auto context = frame_->MainWorldScriptContext();
    if (context.IsEmpty()) return;
    v8::Context::Scope scope(context);
    v8::Local<v8::String> code;
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> factory;
    v8::Local<v8::Function> write_packet, read_packet;
    if (!v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal,
                                 source.size()).ToLocal(&code) ||
        !v8::Script::Compile(context, code).ToLocal(&script) ||
        !script->Run(context).ToLocal(&factory) || !factory->IsFunction() ||
        !v8::Function::New(context, WritePacket).ToLocal(&write_packet) ||
        !v8::Function::New(context, ReadPacket).ToLocal(&read_packet))
        return;
    v8::Local<v8::Value> transport[] = {write_packet, read_packet};
    v8::Local<v8::Value> result;
    if (!factory.As<v8::Function>()->Call(context, v8::Undefined(isolate), 2, transport)
            .ToLocal(&result))
        return;
}
