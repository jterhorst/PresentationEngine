// Test preview window: a screen output in a normal window. The composite is
// converted straight into the CAMetalLayer's drawable by the BGRA8 compute
// pass, paced by the display's link, with frames-in-flight bookkeeping.
//
// Keys: Right/Space next slide, Left previous, Up/Down switch scene,
// A replay the scene's script, C clear all, 1-4 clear background / slide /
// fixtures / all (the F1-F4 actions).
//
// SPDX-License-Identifier: MPL-2.0
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <memory>
#include <string>
#include <vector>

#include "core/frames_in_flight.h"
#include "engined/scene_runner.h"
#include "rhi_metal/metal_device.h"
#include "text/coretext_layout.h"

using namespace engine;

@class PreviewController;

@interface PreviewView : NSView
@property(nonatomic, weak) PreviewController* controller;
@end

@interface PreviewController : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithScenes:(const std::vector<std::string>&)paths;
- (void)keyDown:(NSEvent*)event;
@end

@implementation PreviewView
- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent*)event {
    [self.controller keyDown:event];
}
@end

namespace {
struct PreviewState {
    std::unique_ptr<rhi_metal::MetalDevice> device;
    CoreTextRasterizer text;
    ImageIODecoder images;
    std::vector<std::string> paths;
    std::vector<std::unique_ptr<SceneRunner>> scenes;
    size_t current = 0;
    MonotonicClock clock;
    Nanos origin = 0;
    bool autoplay = true;
    FramesInFlight in_flight{3};
    rhi::TextureId drawable_id = rhi::kNoTexture;

    // Rolling stats for the title bar.
    Nanos stats_since = 0;
    uint32_t frames = 0;
    FrameCounters sum;
};

void frame_done(void* ctx) { static_cast<FramesInFlight*>(ctx)->complete(0); }
}  // namespace

@implementation PreviewController {
    std::unique_ptr<PreviewState> _s;
    NSWindow* _window;
    PreviewView* _view;
    CADisplayLink* _link;
}

- (instancetype)initWithScenes:(const std::vector<std::string>&)paths {
    if ((self = [super init])) {
        _s = std::make_unique<PreviewState>();
        _s->paths = paths;
    }
    return self;
}

- (SceneRunner&)runner {
    return *_s->scenes[_s->current];
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    std::string error;
    _s->device = rhi_metal::MetalDevice::create(error);
    if (!_s->device) {
        NSLog(@"engined: %s", error.c_str());
        [NSApp terminate:nil];
        return;
    }
    for (const auto& p : _s->paths) {
        auto r = SceneRunner::load(p, *_s->device, _s->text, _s->images, error);
        if (!r) {
            NSLog(@"engined: %s", error.c_str());
            continue;
        }
        for (const auto& w : r->prepare_report().warnings) NSLog(@"engined: warning: %s", w.c_str());
        _s->scenes.push_back(std::move(r));
    }
    if (_s->scenes.empty()) {
        [NSApp terminate:nil];
        return;
    }

    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.delegate = self;
    _window.backgroundColor = NSColor.blackColor;
    _window.contentAspectRatio = NSMakeSize(16, 9);
    _view = [[PreviewView alloc] initWithFrame:frame];
    _view.controller = self;
    _view.wantsLayer = YES;
    CAMetalLayer* layer = (CAMetalLayer*)_view.layer;
    layer.device = (__bridge id<MTLDevice>)_s->device->native_device();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;  // the convert pass writes the drawable from compute
    layer.contentsGravity = kCAGravityResizeAspect;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activate];

    [self switchToScene:0];
    _link = [_view displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)switchToScene:(size_t)index {
    _s->current = index % _s->scenes.size();
    SceneRunner& r = [self runner];
    r.restart();
    _s->origin = _s->clock.now();
    _s->autoplay = true;
    CAMetalLayer* layer = (CAMetalLayer*)_view.layer;
    layer.drawableSize = CGSizeMake(r.compositor().output().width, r.compositor().output().height);
}

- (void)tick:(CADisplayLink*)link {
    SceneRunner& r = [self runner];
    uint64_t frame_id;
    if (!_s->in_flight.try_begin(frame_id)) return;  // GPU behind: skip, never wait

    CAMetalLayer* layer = (CAMetalLayer*)_view.layer;
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        _s->in_flight.complete(frame_id);
        return;
    }

    Nanos t = _s->clock.now() - _s->origin;
    if (_s->autoplay) {
        if (t >= r.duration()) {  // loop the scene's script
            r.restart();
            _s->origin = _s->clock.now();
            t = 0;
        }
        r.advance_script(t);
    }
    const Nanos edge = r.rate().edge(r.rate().frame_at(t));

    MTL::Texture* target = (__bridge MTL::Texture*)drawable.texture;
    if (_s->drawable_id == rhi::kNoTexture) _s->drawable_id = _s->device->wrap_external(target);
    else _s->device->set_external(_s->drawable_id, target);

    FrameCounters c;
    rhi::CommandList& cmd = _s->device->begin_frame();
    r.render(edge, cmd, c);
    cmd.convert_to_bgra8(r.compositor().composite(), _s->drawable_id);
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)_s->device->current_command_buffer();
    [cb presentDrawable:drawable];
    _s->device->end_frame(frame_done, &_s->in_flight);
    c += _s->device->take_counters();

    _s->sum += c;
    ++_s->frames;
    const Nanos now = _s->clock.now();
    if (now - _s->stats_since >= kNanosPerSecond / 2) {
        const double secs = (now - _s->stats_since) / 1e9;
        const double n = _s->frames ? _s->frames : 1;
        _window.title = [NSString
            stringWithFormat:@"%@ — %.1f fps · %.0f draws · %.0f passes · %.0f allocs/frame · skipped %llu%@",
                             @(r.scene().name().c_str()), _s->frames / secs, _s->sum.draws / n,
                             _s->sum.render_passes / n, _s->sum.allocations / n,
                             (unsigned long long)_s->in_flight.skipped(), _s->autoplay ? @" · auto" : @""];
        _s->stats_since = now;
        _s->frames = 0;
        _s->sum = {};
    }
}

- (void)clear:(v1::LayerTarget)layer {
    v1::Command cmd;
    cmd.mutable_clear_layer()->set_layer(layer);
    std::string error;
    [self runner].send(cmd, error);
    _s->autoplay = false;
}

- (void)keyDown:(NSEvent*)event {
    const unichar key = event.charactersIgnoringModifiers.length ? [event.charactersIgnoringModifiers characterAtIndex:0] : 0;
    switch (key) {
    case NSRightArrowFunctionKey:
    case ' ':
        _s->autoplay = false;
        [self runner].step_slide(+1);
        break;
    case NSLeftArrowFunctionKey:
        _s->autoplay = false;
        [self runner].step_slide(-1);
        break;
    case NSDownArrowFunctionKey: [self switchToScene:_s->current + 1]; break;
    case NSUpArrowFunctionKey: [self switchToScene:_s->current + _s->scenes.size() - 1]; break;
    case 'a': [self switchToScene:_s->current]; break;
    case 'c':
    case '4':
    case NSF1FunctionKey: [self clear:v1::LAYER_TARGET_ALL]; break;
    case '2':
    case NSF2FunctionKey: [self clear:v1::LAYER_TARGET_SLIDE]; break;
    case '1':
    case NSF3FunctionKey: [self clear:v1::LAYER_TARGET_BACKGROUND]; break;
    case '3':
    case NSF4FunctionKey: [self clear:v1::LAYER_TARGET_FIXTURES]; break;
    default: break;
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    [_link invalidate];
    if (_s->device) _s->device->wait_idle();
    _s->scenes.clear();
}

@end

namespace engine {

int run_preview(const std::vector<std::string>& scenes) {
    if (scenes.empty()) {
        std::fprintf(stderr, "engined --preview needs at least one scene\n");
        return 2;
    }
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [bar addItem:app_item];
        NSMenu* app_menu = [[NSMenu alloc] init];
        [app_menu addItemWithTitle:@"Quit engined" action:@selector(terminate:) keyEquivalent:@"q"];
        app_item.submenu = app_menu;
        app.mainMenu = bar;
        PreviewController* controller = [[PreviewController alloc] initWithScenes:scenes];
        app.delegate = controller;
        [app run];
    }
    return 0;
}

}  // namespace engine
