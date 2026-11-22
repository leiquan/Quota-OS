// Produce a single 1280 x 800 XRGB8888 surface for the Quota OS ramfb.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

static NSString *output;
static void fail(NSString *message) {
    fprintf(stderr, "%s\n", message.UTF8String);
    exit(1);
}

@interface Preview : NSObject <WKNavigationDelegate>
@end
@implementation Preview
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
    WKSnapshotConfiguration *options = [WKSnapshotConfiguration new];
    options.rect = view.bounds;
    [view takeSnapshotWithConfiguration:options completionHandler:^(NSImage *image, NSError *error) {
        if (!image) fail(error.localizedDescription);
        CGImageRef cg = [image CGImageForProposedRect:NULL context:nil hints:nil];
        if (!cg) fail(@"WebKit returned an empty snapshot");
        NSMutableData *pixels = [NSMutableData dataWithLength:1280 * 800 * 4];
        CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(pixels.mutableBytes, 1280, 800, 8, 5120,
            rgb, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(rgb);
        if (!ctx) fail(@"Cannot allocate snapshot context");
        CGContextDrawImage(ctx, CGRectMake(0, 0, 1280, 800), cg);
        CGContextRelease(ctx);
        if (![pixels writeToFile:output atomically:YES]) fail(@"Cannot save preview");
        exit(0);
    }];
}
- (void)webView:(WKWebView *)view didFailNavigation:(WKNavigation *)nav withError:(NSError *)error {
    fail(error.localizedDescription);
}
- (void)webView:(WKWebView *)view decidePolicyForNavigationAction:(WKNavigationAction *)action
  decisionHandler:(void (^)(WKNavigationActionPolicy))reply {
    reply([action.request.URL.absoluteString isEqualToString:@"about:blank"] ?
          WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSError *error = nil;
        NSString *html = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
        if (!html) fail(error.localizedDescription);
        output = @(argv[2]);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        config.defaultWebpagePreferences.allowsContentJavaScript = NO;
        WKWebView *view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 800) configuration:config];
        __attribute__((objc_precise_lifetime)) Preview *delegate = [Preview new];
        view.navigationDelegate = delegate;
        __attribute__((objc_precise_lifetime)) NSWindow *window = [[NSWindow alloc] initWithContentRect:view.bounds
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        window.contentView = view;
        // Local, self-contained documents only: block external subresources and frames.
        NSString *rules = @"[{\"trigger\":{\"url-filter\":\"^[a-z]+://\"},\"action\":{\"type\":\"block\"}}]";
        [[WKContentRuleListStore defaultStore] compileContentRuleListForIdentifier:@"quota-local-preview"
            encodedContentRuleList:rules completionHandler:^(WKContentRuleList *list, NSError *err) {
                if (!list) fail(err.localizedDescription);
                [config.userContentController addContentRuleList:list];
                [view loadHTMLString:html baseURL:nil];
            }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(),
                       ^{ fail(@"WebKit preview timed out"); });
        [NSApp run];
    }
    return 1;
}
