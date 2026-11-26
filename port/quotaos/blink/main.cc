#include "page_frame_client.h"
#include "page_widget.h"
#include "page_paint.h"
#include "window_input.h"
#include "include/guest_page_services.h"

#include <cerrno>
#include <functional>
#include <fcntl.h>
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/files/file.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/i18n/icu_util.h"
#include "base/message_loop/message_pump.h"
#include "base/run_loop.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/timer/timer.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "gin/v8_initializer.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "third_party/blink/public/mojom/partitioned_popins/partitioned_popin_params.mojom.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "base/containers/span.h"
#include "ui/base/resource/resource_bundle.h"
#include "third_party/blink/public/resources/grit/blink_resources.h"

namespace {
class QuotaBlinkPlatform final : public blink::Platform {
public:
    QuotaBlinkPlatform() : resources_(nullptr) {
        resources_.AddDataPackFromPath(base::FilePath("/usr/lib/quota-runtime/blink_resources.pak"),
                                       ui::kScaleFactorNone);
    }

    blink::WebString DefaultLocale() override { return blink::WebString::FromASCII("en-US"); }

    bool HasDataResource(int id) const override { return resources_.HasDataResource(id); }
    std::string GetDataResourceString(int id) override {
        return resources_.LoadDataResourceString(id);
    }
    blink::WebData GetDataResource(int id, ui::ResourceScaleFactor scale) override {
        return blink::WebData(base::as_byte_span(resources_.GetRawDataResourceForScale(id, scale)));
    }
    base::RefCountedMemory* GetDataResourceBytes(int id) override {
        return resources_.LoadDataResourceBytes(id);
    }
private:
    ui::ResourceBundle resources_;
};
}  // namespace

int main(int argc, char** argv) {
    base::AtExitManager at_exit;
    base::CommandLine::Init(argc, argv);
    base::FeatureList::InitInstance("", "");
    base::ScopedFD window(open("/dev/quota-window", O_RDWR | O_NONBLOCK));
    if (!window.is_valid())
        return 1;
    int icu = open("/usr/lib/quota-runtime/icudtl.dat", O_RDONLY);
    if (icu < 0 || !base::i18n::InitializeICUWithFileDescriptor(
            icu, base::MemoryMappedFile::Region::kWholeFile))
        return 2;
    base::File snapshot(base::FilePath("/usr/lib/quota-runtime/snapshot_blob.bin"),
                        base::File::FLAG_OPEN | base::File::FLAG_READ);
    if (!snapshot.IsValid())
        return 3;
    gin::V8Initializer::LoadV8SnapshotFromFile(std::move(snapshot), nullptr,
        gin::V8SnapshotFileType::kDefault);
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("QuotaPage");
    mojo::core::Init();
    SkGraphics::Init();
    blink::Platform::InitializeBlink();
    auto scheduler = blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
        base::MessagePump::Create(base::MessagePumpType::DEFAULT));
    QuotaBlinkPlatform platform;
    if (platform.GetDataResourceString(IDR_UASTYLE_HTML_CSS).empty())
        return 8;
    mojo::BinderMap binders;
    blink::Initialize(&platform, &binders, scheduler.get());
    auto group = scheduler->CreateWebAgentGroupScheduler();
    QuotaPageWidget widget_client;
    auto* view = blink::WebView::Create(&widget_client, false, nullptr, std::nullopt,
        false, true, nullptr, mojo::NullAssociatedReceiver(), *group, std::string(),
        SK_ColorWHITE, base::UnguessableToken::Create(), nullptr, nullptr, -1, 0,
        std::nullopt);
    // Core settings start with zero default font sizes until the embedder
    // applies the web preferences. Do so before the initial document exists.
    blink::web_pref::WebPreferences preferences;
    view->SetWebPreferences(preferences);
    QuotaPageFrameClient client(base::CommandLine::ForCurrentProcess()->HasSwitch("wap-rpc"));
    auto* frame = blink::WebLocalFrame::CreateMainFrame(view, &client, nullptr,
        mojo::NullRemote(), blink::LocalFrameToken(), blink::DocumentToken(), nullptr);
    auto* widget = widget_client.Create(*frame, viz::FrameSinkId(1, 1));
    if (!widget || client.AttachAndLoad(*view, *widget, widget_client,
            blink::WebURL(blink::KURL("file:///app/app.html"))) < 0)
        return 4;
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(
        GUEST_PREVIEW_WIDTH, GUEST_PREVIEW_HEIGHT));
    if (!surface)
        return 5;
    base::RunLoop loop;
    base::RepeatingTimer refresh;
    bool dirty = true;
    int status = 0;
    auto tick = [&]() {
        for (int count = 0; client.attached() && count < 32; ++count) {
            int event = quota_read_window_input(window.get(), *widget);
            if (event < 0) {
                status = 6;
                loop.Quit();
                return;
            }
            if (!event)
                break;
            dirty = true;
        }
        if (!client.attached()) {
            loop.Quit();
            return;
        }
        dirty |= widget_client.TakeRedrawRequest();
        auto* root = static_cast<blink::WebLocalFrameImpl*>(frame)->GetFrame()->View();
        if (dirty && root) {
            int result = quota_paint_page(*root, *surface, window.get());
            if (result < 0 && result != -EAGAIN) {
                status = 7;
                loop.Quit();
            }
            dirty = result != 0;
        }
    };
    refresh.Start(FROM_HERE, base::Milliseconds(16),
        base::BindRepeating([](decltype(tick)& run) { run(); }, std::ref(tick)));
    loop.Run();
    refresh.Stop();
    view->Close();
    return status;
}
