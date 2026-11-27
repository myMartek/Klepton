// kl_web — WKWebView behind a C interface (see kl_web.h).
//
// WebKit lives on the main thread, so every call below is posted there and
// returns at once. The picture is a snapshot loop: while the game asks for
// frames, the view is snapshotted, the image is drawn into an RGBA buffer off
// the main thread, and the newest buffer waits for the GL thread to copy it.
// Page events (load start/progress/finish/fail, URL changes, what the page
// posts to its bridge) are queued here and drained by the game thread.
//
// The shipping runtime compiles this file WITHOUT ARC (build/xros/libklepton.a),
// the host tests and m_boot WITH it, so every object kept past its autorelease
// pool goes through KL_RETAIN/KL_RELEASE, as in kl_remote.m.
#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "kl_web.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if __has_feature(objc_arc)
#define KL_RETAIN(x) (x)
#define KL_RELEASE(x) ((void)0)
#define KL_AUTORELEASE(...) (__VA_ARGS__)
#else
#define KL_RETAIN(x) [(x) retain]
#define KL_RELEASE(x) [(x) release]
#define KL_AUTORELEASE(...) [(__VA_ARGS__) autorelease]
#endif

#define WLOG(...) do { fprintf(stderr, "  [web] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int env_int(const char *k, int def) {
    const char *s = getenv(k);
    return (s && *s) ? atoi(s) : def;
}

// ---- the C side of a view ----------------------------------------------------

typedef struct kl_web_evq {
    struct kl_web_evq *next;
    kl_web_ev kind;
    int id;
    char *arg;
} kl_web_evq;

struct kl_web {
    pthread_mutex_t mu;
    char tag[96];
    int w, h;                       // view size in pixels (= points here)
    kl_web_evq *eq_head, *eq_tail;
    unsigned eq_n;
    // picture: the front buffer is what kl_web_frame copies out
    uint8_t *front;
    int fw, fh;
    uint64_t fseq;
    int fluma;
    _Atomic uint64_t wanted_ms;     // last time the game asked for a frame
    _Atomic int active;
    _Atomic int dead;
    _Atomic uint64_t poke_ms;       // last input or load: snapshots at full rate
    void *host;                     // KLWebHost, retained (main thread only)
};

static void evq_push(kl_web *v, kl_web_ev kind, const char *arg, int id) {
    kl_web_evq *e = calloc(1, sizeof *e);
    if (!e) return;
    e->kind = kind;
    e->id = id;
    e->arg = strdup(arg ? arg : "");
    pthread_mutex_lock(&v->mu);
    // A page that posts without pause must not grow this forever while the
    // game is not draining (paused view, room change): the oldest go first.
    if (v->eq_n >= 512 && v->eq_head) {
        kl_web_evq *old = v->eq_head;
        v->eq_head = old->next;
        if (!v->eq_head) v->eq_tail = NULL;
        v->eq_n--;
        free(old->arg);
        free(old);
    }
    if (v->eq_tail) v->eq_tail->next = e; else v->eq_head = e;
    v->eq_tail = e;
    v->eq_n++;
    pthread_mutex_unlock(&v->mu);
}

// ---- asset URLs ----------------------------------------------------------------

static kl_web_asset_fn g_asset_reader;
void kl_web_set_asset_reader(kl_web_asset_fn fn) { g_asset_reader = fn; }

// "jar:file:///…/x.apk!/assets/web/a.html?q" → "klasset://apk/assets/web/a.html?q"
// so the page's relative links (styles.css, the font) resolve through the
// same scheme handler.
static NSString *map_url(const char *url) {
    if (!url) return @"about:blank";
    if (strncmp(url, "jar:", 4) == 0) {
        const char *bang = strstr(url, "!/");
        if (bang) return [NSString stringWithFormat:@"klasset://apk/%s", bang + 2];
    }
    if (strncmp(url, "file:///android_asset/", 22) == 0)
        return [NSString stringWithFormat:@"klasset://apk/assets/%s", url + 22];
    return [NSString stringWithUTF8String:url] ?: @"about:blank";
}

static NSString *mime_for(NSString *path) {
    NSString *ext = path.pathExtension.lowercaseString;
    NSDictionary *m = @{ @"html": @"text/html", @"htm": @"text/html", @"css": @"text/css",
                         @"js": @"text/javascript", @"json": @"application/json",
                         @"svg": @"image/svg+xml", @"png": @"image/png",
                         @"jpg": @"image/jpeg", @"jpeg": @"image/jpeg",
                         @"gif": @"image/gif", @"webp": @"image/webp",
                         @"ttf": @"font/ttf", @"otf": @"font/otf",
                         @"woff": @"font/woff", @"woff2": @"font/woff2",
                         @"mp4": @"video/mp4", @"webm": @"video/webm" };
    return m[ext] ?: @"application/octet-stream";
}

// ---- input helpers injected into every page -----------------------------------

// Synthesised DOM input. WKWebView has no public way to inject touches or
// mouse events, so pointer input becomes PointerEvent/MouseEvent/WheelEvent
// dispatched at the element under the point, with click, focus and the
// scrolling a real wheel would do. Pages that ignore untrusted events (very
// few) will not react; the cloud browser and YouTube listen normally.
static NSString *const kInputScript =
@"(function(){if(window.__klweb)return;var down=null,last=null,btns=0;"
 "function T(x,y){return document.elementFromPoint(x,y)||document.documentElement;}"
 "function O(x,y,b,bs){return{bubbles:true,cancelable:true,composed:true,view:window,"
 "clientX:x,clientY:y,screenX:x,screenY:y,button:b,buttons:bs,pointerId:1,"
 "pointerType:'mouse',isPrimary:true,detail:1};}"
 "function F(el,t,o,p){try{return el.dispatchEvent(p?new PointerEvent(t,o):new MouseEvent(t,o));}catch(e){return true;}}"
 "function focusable(el){return el&&el.closest?el.closest('input,textarea,select,[contenteditable=\"\"],[contenteditable=true],button,a[href],[tabindex]'):null;}"
 "window.__klweb={"
 "move:function(x,y){var el=T(x,y);var o=O(x,y,-1,btns);"
 "if(last!==el){if(last){F(last,'pointerout',o,1);F(last,'mouseout',o);}F(el,'pointerover',o,1);F(el,'mouseover',o);last=el;}"
 "F(el,'pointermove',o,1);F(el,'mousemove',O(x,y,0,btns));},"
 "down:function(x,y,b){var el=T(x,y);down=el;btns|=(b==2?2:b==1?4:1);var o=O(x,y,b,btns);"
 "F(el,'pointerdown',o,1);if(F(el,'mousedown',o)){var f=focusable(el);"
 "if(f&&f.focus){f.focus();}else if(document.activeElement&&document.activeElement!==document.body&&document.activeElement.blur){document.activeElement.blur();}}},"
 "up:function(x,y,b){var el=T(x,y);btns&=~(b==2?2:b==1?4:1);var o=O(x,y,b,btns);"
 "F(el,'pointerup',o,1);F(el,'mouseup',o);"
 "if(down&&(down===el||down.contains(el)||el.contains(down))){if(b===0){F(el,'click',o);}else if(b===2){F(el,'contextmenu',o);}}down=null;},"
 "wheel:function(dx,dy,x,y){var el=T(x,y);var ok=true;try{ok=el.dispatchEvent(new WheelEvent('wheel',"
 "{bubbles:true,cancelable:true,composed:true,clientX:x,clientY:y,deltaX:dx,deltaY:dy,deltaMode:0}));}catch(e){}"
 "if(!ok)return;for(var n=el;n&&n!==document.body&&n!==document.documentElement;n=n.parentElement){"
 "var s=getComputedStyle(n);if((/(auto|scroll)/.test(s.overflowY)&&n.scrollHeight>n.clientHeight&&dy)||"
 "(/(auto|scroll)/.test(s.overflowX)&&n.scrollWidth>n.clientWidth&&dx)){n.scrollBy(dx,dy);return;}}"
 "window.scrollBy(dx,dy);},"
 "key:function(k){var a=document.activeElement||document.body;"
 "var named={Enter:13,Backspace:8,Tab:9,Escape:27,Delete:46,ArrowLeft:37,ArrowUp:38,ArrowRight:39,ArrowDown:40,Home:36,End:35,PageUp:33,PageDown:34,Space:32};"
 "var ch=(k.length<=2&&!(k in named))?k:(k==='Space'?' ':null);var code=named[k]||(ch?ch.toUpperCase().charCodeAt(0):0);"
 "var ko={key:ch||k,code:k,keyCode:code,which:code,bubbles:true,cancelable:true,composed:true};"
 "var go=a.dispatchEvent(new KeyboardEvent('keydown',ko));"
 "if(go&&ch){a.dispatchEvent(new KeyboardEvent('keypress',ko));}"
 "if(go){var ed=a&&(a.tagName==='INPUT'||a.tagName==='TEXTAREA');"
 "if(ed&&ch){try{var s0=a.selectionStart,s1=a.selectionEnd;a.setRangeText(ch,s0,s1,'end');}catch(e){a.value+=ch;}a.dispatchEvent(new InputEvent('input',{bubbles:true,data:ch,inputType:'insertText'}));}"
 "else if(ed&&k==='Backspace'){try{var b0=a.selectionStart,b1=a.selectionEnd;if(b0===b1&&b0>0)b0--;a.setRangeText('',b0,b1,'end');}catch(e){a.value=a.value.slice(0,-1);}a.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'deleteContentBackward'}));}"
 "else if(ed&&k==='Enter'&&a.tagName==='INPUT'){if(a.form){if(a.form.requestSubmit)a.form.requestSubmit();else a.form.submit();}a.dispatchEvent(new Event('change',{bubbles:true}));}"
 "else if(a.isContentEditable&&ch){document.execCommand('insertText',false,ch);}"
 "else if(!ed&&k==='Space'){window.scrollBy(0,window.innerHeight*0.8);}}"
 "a.dispatchEvent(new KeyboardEvent('keyup',ko));}"
 "};})();";

// Video frames are not in a WebKit snapshot: a <video> is a hardware layer
// of its own, and the snapshot of a playing YouTube video was its page with
// a hole (sound, no picture; 2026-11-27). So each playing video is copied,
// about 30 times a second, into a CPU-backed canvas laid exactly over it —
// its next sibling, same box, below whatever the page puts above the video
// (controls) — which the snapshot does contain. Protected (EME) video draws
// black, as it would anywhere. Also reports whether any media plays
// (klmedia), for the audio session rule (kl_audio_web_window).
static NSString *const kVideoScript =
@"(function(){if(window.__klvid)return;window.__klvid=1;var map=new WeakMap(),playing=-1;"
 "function box(v,c){var cs=getComputedStyle(v);var s=c.style;s.left=v.offsetLeft+'px';s.top=v.offsetTop+'px';"
 "s.width=v.offsetWidth+'px';s.height=v.offsetHeight+'px';s.zIndex=cs.zIndex;s.objectFit=cs.objectFit||'contain';"
 "s.transform=cs.transform;s.transformOrigin=cs.transformOrigin;}"
 "function canvasFor(v){var c=map.get(v);if(c&&c.parentNode===v.parentNode&&c.previousSibling===v)return c;"
 "if(c&&c.parentNode)c.parentNode.removeChild(c);c=document.createElement('canvas');c.setAttribute('data-klvid','1');"
 "c.style.cssText='position:absolute;pointer-events:none;margin:0;padding:0;border:0;display:block';"
 "map.set(v,c);if(v.parentNode)v.parentNode.insertBefore(c,v.nextSibling);return c;}"
 "function draw(v){var w=v.videoWidth,h=v.videoHeight;if(!w||!h)return;var c=canvasFor(v);box(v,c);"
 "var cw=Math.min(w,1280),ch=Math.max(1,Math.round(h*cw/w));if(c.width!==cw)c.width=cw;if(c.height!==ch)c.height=ch;"
 "var g=c.getContext('2d',{willReadFrequently:true});g.drawImage(v,0,0,cw,ch);c.style.visibility='visible';}"
 "function tick(){var any=0;var vs=document.querySelectorAll('video');for(var i=0;i<vs.length;i++){var v=vs[i];"
 "var c=map.get(v);var on=!v.paused&&!v.ended&&v.readyState>=2&&v.offsetWidth>0;if(on){any=1;try{draw(v)}catch(e){}}"
 "else if(c&&(v.ended||v.readyState<2||!v.offsetWidth))c.style.visibility='hidden';}"
 "var as=document.querySelectorAll('audio');for(var j=0;j<as.length;j++)if(!as[j].paused&&!as[j].ended)any=1;"
 "if(any!==playing){playing=any;try{window.webkit.messageHandlers.klmedia.postMessage(any)}catch(e){}}"
 "setTimeout(tick,any?33:250);}tick();})();";

// Vuplex's page side talks to its host through window._vuplexBridge.
static NSString *const kBridgeScript =
@"(function(){if(window._vuplexBridge)return;window._vuplexBridge={postMessage:function(m){"
 "try{window.webkit.messageHandlers.klvx.postMessage(String(m));}catch(e){}}};})();";

// ---- the main-thread side --------------------------------------------------------

@interface KLWebHost : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, WKURLSchemeHandler>
@end

@implementation KLWebHost {
@public
    kl_web *_v;
    WKWebView *_wv;
    NSString *_pageScript;
    dispatch_source_t _timer;
    BOOL _snapInFlight;
    uint64_t _snaps, _snapMsSum;
    BOOL _loggedFirst;
    uint64_t _loadStartMs;
    dispatch_queue_t _drawQ;
    // A page that has not changed for a second is snapshotted 5 times a
    // second instead of every tick, until it changes or is poked again.
    uint64_t _lastSig, _lastSnapMs;
    unsigned _same;
    uint64_t _pngMs;             // draw queue only
    int _pngLuma;
}

#if !__has_feature(objc_arc)
- (void)dealloc {
    [_wv release];
    [_pageScript release];
    if (_timer) dispatch_release(_timer);
    if (_drawQ) dispatch_release(_drawQ);
    [super dealloc];
}
#endif

// WKScriptMessageHandler
- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)msg {
    if (atomic_load(&_v->dead)) return;
    if ([msg.name isEqualToString:@"klmedia"]) {
        int on = [msg.body respondsToSelector:@selector(intValue)] ? [msg.body intValue] : 0;
        WLOG("%s: media %s", _v->tag, on ? "playing" : "stopped");
        evq_push(_v, KL_WEB_EV_MEDIA, on ? "1" : "0", 0);
        return;
    }
    id body = msg.body;
    NSString *s = [body isKindOfClass:[NSString class]] ? body : [body description];
    evq_push(_v, KL_WEB_EV_MESSAGE, s.UTF8String, 0);
}

// WKNavigationDelegate
- (void)webView:(WKWebView *)wv didStartProvisionalNavigation:(WKNavigation *)nav {
    NSString *u = wv.URL.absoluteString ?: @"";
    _loadStartMs = now_ms();
    WLOG("%s: load started %s", _v->tag, u.UTF8String);
    evq_push(_v, KL_WEB_EV_LOAD_STARTED, u.UTF8String, 0);
}
- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    NSString *u = wv.URL.absoluteString ?: @"";
    evq_push(_v, KL_WEB_EV_URL_CHANGED, u.UTF8String, 0);
}
- (void)webView:(WKWebView *)wv didFinishNavigation:(WKNavigation *)nav {
    NSString *u = wv.URL.absoluteString ?: @"";
    WLOG("%s: load finished in %llu ms: %s", _v->tag,
         (unsigned long long)(now_ms() - _loadStartMs), u.UTF8String);
    if (_pageScript.length) {
        [wv evaluateJavaScript:_pageScript completionHandler:^(id r, NSError *e) {
            if (e) WLOG("%s: page script failed: %s", self->_v->tag,
                        e.localizedDescription.UTF8String);
        }];
    }
    evq_push(_v, KL_WEB_EV_LOAD_FINISHED, u.UTF8String, 0);
    // Whether WebKit counts the page as shown: "hidden" means no animation
    // frames and no lazy loading (see ensureAttached).
    [wv evaluateJavaScript:@"document.visibilityState" completionHandler:^(id r, NSError *e) {
        if ([r isKindOfClass:[NSString class]])
            WLOG("%s: page visibility %s", self->_v->tag, [r UTF8String]);
    }];
}
- (void)failed:(WKWebView *)wv error:(NSError *)e {
    if ([e.domain isEqualToString:NSURLErrorDomain] && e.code == NSURLErrorCancelled) return;
    NSURL *fu = e.userInfo[NSURLErrorFailingURLErrorKey];
    NSString *u = fu.absoluteString ?: (wv.URL.absoluteString ?: @"");
    WLOG("%s: load failed (%s %ld): %s", _v->tag, e.domain.UTF8String, (long)e.code,
         e.localizedDescription.UTF8String);
    NSString *arg = [NSString stringWithFormat:@"%@,%@", e.localizedDescription ?: @"error", u];
    evq_push(_v, KL_WEB_EV_LOAD_FAILED, arg.UTF8String, 0);
}
- (void)webView:(WKWebView *)wv didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e {
    [self failed:wv error:e];
}
- (void)webView:(WKWebView *)wv didFailNavigation:(WKNavigation *)n withError:(NSError *)e {
    [self failed:wv error:e];
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)wv {
    WLOG("%s: web content process ended — reloading", _v->tag);
    [wv reload];
}

// WKUIDelegate: a page opening a new window (target=_blank, window.open)
// loads in this view, as Vuplex does with popups off.
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)c
   forNavigationAction:(WKNavigationAction *)a windowFeatures:(WKWindowFeatures *)f {
    if (a.request.URL) [wv loadRequest:a.request];
    return nil;
}
#if !TARGET_OS_OSX
- (void)webView:(WKWebView *)wv requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)o
 initiatedByFrame:(WKFrameInfo *)fr type:(WKMediaCaptureType)t
  decisionHandler:(void (^)(WKPermissionDecision))dh {
    dh(WKPermissionDecisionDeny);   // no prompt can show from an off-screen view
}
#endif

// WKURLSchemeHandler for klasset:// — the APK's own pages.
- (void)webView:(WKWebView *)wv startURLSchemeTask:(id<WKURLSchemeTask>)task {
    NSURL *url = task.request.URL;
    NSString *path = url.path;
    if ([path hasPrefix:@"/"]) path = [path substringFromIndex:1];
    path = path.stringByRemovingPercentEncoding ?: path;
    size_t len = 0;
    void *bytes = g_asset_reader ? g_asset_reader(path.UTF8String, &len) : NULL;
    if (!bytes) {
        WLOG("%s: asset %s not in the APK", _v->tag, path.UTF8String);
        NSHTTPURLResponse *r = KL_AUTORELEASE([[NSHTTPURLResponse alloc]
            initWithURL:url statusCode:404 HTTPVersion:@"HTTP/1.1" headerFields:@{}]);
        [task didReceiveResponse:r];
        [task didFinish];
        return;
    }
    NSData *d = [NSData dataWithBytesNoCopy:bytes length:len freeWhenDone:YES];
    NSHTTPURLResponse *r = KL_AUTORELEASE([[NSHTTPURLResponse alloc]
        initWithURL:url statusCode:200 HTTPVersion:@"HTTP/1.1"
       headerFields:@{ @"Content-Type": mime_for(path),
                       @"Content-Length": [NSString stringWithFormat:@"%zu", len],
                       @"Access-Control-Allow-Origin": @"*" }]);
    [task didReceiveResponse:r];
    [task didReceiveData:d];
    [task didFinish];
}
- (void)webView:(WKWebView *)wv stopURLSchemeTask:(id<WKURLSchemeTask>)task {}

// KVO: progress and title.
- (void)observeValueForKeyPath:(NSString *)kp ofObject:(id)o change:(NSDictionary *)c context:(void *)ctx {
    if (atomic_load(&_v->dead)) return;
    if ([kp isEqualToString:@"estimatedProgress"]) {
        char b[32];
        snprintf(b, sizeof b, "%.2f", _wv.estimatedProgress);
        evq_push(_v, KL_WEB_EV_LOAD_PROGRESS, b, 0);
    } else if ([kp isEqualToString:@"title"]) {
        evq_push(_v, KL_WEB_EV_TITLE_CHANGED, (_wv.title ?: @"").UTF8String, 0);
    } else if ([kp isEqualToString:@"URL"]) {
        NSString *u = _wv.URL.absoluteString;
        if (u) evq_push(_v, KL_WEB_EV_URL_CHANGED, u.UTF8String, 0);
    }
}

// ---- snapshots ----

- (void)tick {
    kl_web *v = _v;
    if (atomic_load(&v->dead) || _snapInFlight || !_wv) return;
    [self ensureAttached];
    uint64_t t = now_ms();
    // Only while the game draws this view: a view nobody samples costs nothing.
    if (!atomic_load(&v->active) || t - atomic_load(&v->wanted_ms) > 1000) return;
    if (_same >= 30 && t - atomic_load(&v->poke_ms) > 2000 && t - _lastSnapMs < 200) return;
    _lastSnapMs = t;
    _snapInFlight = YES;
    WKSnapshotConfiguration *cfg = KL_AUTORELEASE([[WKSnapshotConfiguration alloc] init]);
    cfg.rect = _wv.bounds;
    cfg.snapshotWidth = @(v->w);
    if (@available(macOS 10.15, iOS 13.0, *)) cfg.afterScreenUpdates = YES;
    uint64_t t0 = t;
    int w = v->w, h = v->h;
#if TARGET_OS_OSX
    [_wv takeSnapshotWithConfiguration:cfg completionHandler:^(NSImage *img, NSError *err) {
        CGImageRef cg = img ? [img CGImageForProposedRect:NULL context:nil hints:nil] : NULL;
#else
    [_wv takeSnapshotWithConfiguration:cfg completionHandler:^(UIImage *img, NSError *err) {
        CGImageRef cg = img.CGImage;
#endif
        if (!cg) {
            self->_snapInFlight = NO;
            if (!self->_loggedFirst) WLOG("%s: snapshot gave no image%s%s", v->tag,
                                          err ? ": " : "", err ? err.localizedDescription.UTF8String : "");
            return;
        }
        CGImageRetain(cg);
        dispatch_async(self->_drawQ, ^{
            [self draw:cg w:w h:h started:t0];
            CGImageRelease(cg);
            dispatch_async(dispatch_get_main_queue(), ^{ self->_snapInFlight = NO; });
        });
    }];
}

- (void)draw:(CGImageRef)cg w:(int)w h:(int)h started:(uint64_t)t0 {
    kl_web *v = _v;
    if (atomic_load(&v->dead) || w <= 0 || h <= 0) return;
    size_t n = (size_t)w * h * 4;
    uint8_t *buf = malloc(n);
    if (!buf) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, (size_t)w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);   // R,G,B,A bytes
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); return; }
    CGContextSetInterpolationQuality(ctx, kCGInterpolationMedium);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    // Mean luma over a sparse grid, for the log: says "blank" or "picture"
    // without anyone having to look at a PNG.
    unsigned long long sum = 0, cnt = 0;
    for (int y = h / 16; y < h; y += h / 8 + 1)
        for (int x = w / 16; x < w; x += w / 8 + 1) {
            const uint8_t *p = buf + ((size_t)y * w + x) * 4;
            sum += (p[0] * 54u + p[1] * 183u + p[2] * 19u) >> 8;
            cnt++;
        }
    int luma = cnt ? (int)(sum / cnt) : -1;
    // Change detector: a hash over every 7th row (FNV-1a over 64-bit words).
    uint64_t sig = 1469598103934665603ull;
    for (int y = 0; y < h; y += 7) {
        const uint64_t *r = (const uint64_t *)(buf + (size_t)y * w * 4);
        for (int x = 0; x < w / 2; x += 3) sig = (sig ^ r[x]) * 1099511628211ull;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (sig == self->_lastSig) self->_same++; else { self->_same = 0; self->_lastSig = sig; }
    });
    pthread_mutex_lock(&v->mu);
    uint8_t *old = v->front;
    v->front = buf;
    v->fw = w;
    v->fh = h;
    v->fseq++;
    v->fluma = luma;
    uint64_t seq = v->fseq;
    pthread_mutex_unlock(&v->mu);
    free(old);
    [self maybeSavePNG:buf w:w h:h luma:luma];
    uint64_t ms = now_ms() - t0;
    _snaps++;
    _snapMsSum += ms;
    if (!_loggedFirst || seq % 300 == 0) {
        _loggedFirst = YES;
        WLOG("%s: frame %llu %dx%d luma %d, snapshot %llu ms (mean %llu)", v->tag,
             (unsigned long long)seq, w, h, luma, (unsigned long long)ms,
             (unsigned long long)(_snapMsSum / (_snaps ? _snaps : 1)));
    }
}

// Diagnostics: a few snapshots as PNG files (KL_BS_WEB_PNG=N, N per run;
// default 0, off: they are pictures of whatever pages the player opens) —
// on the headset in Documents/webshots/, else the temp folder.
// One per view when its picture first turns non-black and on a big change,
// at most one every 4 s per view. Draw queue only.
- (void)maybeSavePNG:(const uint8_t *)buf w:(int)w h:(int)h luma:(int)luma {
    static _Atomic int saved;
    static int cap = -1;
    if (cap < 0) cap = env_int("KL_BS_WEB_PNG", 0);
    if (atomic_load(&saved) >= cap) return;
    uint64_t t = now_ms();
    if (t - _pngMs < 4000) return;
    if (luma < 0 || (luma == 0 && _pngLuma <= 0) || (_pngLuma >= 0 && abs(luma - _pngLuma) < 25 && _pngMs)) return;
    _pngMs = t;
    _pngLuma = luma;
    int n = atomic_fetch_add(&saved, 1);
    if (n >= cap) return;
    NSString *dir = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
#if TARGET_OS_OSX
    dir = NSTemporaryDirectory();
#endif
    dir = [dir stringByAppendingPathComponent:@"webshots"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *path = [dir stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"%02d-%s-luma%d.png", n, _v->tag, luma]];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)buf, w, h, 8, (size_t)w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return;
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return;
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
    if (d) {
        CGImageDestinationAddImage(d, img, NULL);
        CGImageDestinationFinalize(d);
        CFRelease(d);
        WLOG("%s: snapshot saved to %s", _v->tag, path.UTF8String);
    }
    CGImageRelease(img);
}

// ---- where the view lives ----
//
// WebKit paints only views it believes are visible. On the Mac that is one
// borderless window parked off the screen. On visionOS the first build hung
// the views in a UIWindow on the game's immersive scene (CPImmersiveScene),
// and the room went black (2026-11-27): that scene must not get a window.
// So visionOS first checks once whether a WINDOWLESS web view snapshots at
// all (a 64-pt magenta page); when it does, the views stay windowless. When
// it does not, they go into a window on an ordinary window scene if one is
// connected, never the immersive one. KL_BS_WEB_ATTACH=0 forces windowless,
// =2 allows the immersive scene again (the first build's behaviour).
//
// ...but a windowless view is a HIDDEN page to WebKit: it snapshots, yet
// requestAnimationFrame, lazy loading and IntersectionObserver stand still,
// and YouTube showed only its frame (2026-11-27, third headset run). So the
// default (=3) is a window again, on whatever scene is connected, the
// immersive one included, but with alpha 0: WebKit counts the view as in a
// visible window, and nothing of it is drawn into the room. =1 is the
// windowless-first order above.

#if TARGET_OS_OSX
static NSWindow *g_host_win;
#else
static UIWindow *g_host_win;
static int g_wl = -1;            // windowless snapshots: -1 unknown, 1 work, 0 blank
static WKWebView *g_wl_view;

static int image_is_magenta(UIImage *img) {
    CGImageRef cg = img.CGImage;
    if (!cg) return 0;
    uint8_t px[4 * 4 * 4] = {0};
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, 4, 4, 8, 16, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return 0;
    CGContextDrawImage(ctx, CGRectMake(0, 0, 4, 4), cg);
    CGContextRelease(ctx);
    const uint8_t *c = px + (1 * 4 + 1) * 4;
    WLOG("windowless check: pixel %d,%d,%d,%d", c[0], c[1], c[2], c[3]);
    return c[0] > 200 && c[1] < 60 && c[2] > 200;
}

static void wl_done(int ok) {
    g_wl = ok;
    WLOG("windowless web views %s", ok ? "snapshot fine — no window needed"
                                       : "snapshot blank — a window is needed");
    [g_wl_view removeFromSuperview];
    KL_RELEASE(g_wl_view);
    g_wl_view = nil;
}

static void wl_try(int attempt) {
    static const double delay[] = { 0.6, 1.5, 3.0, 5.0 };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay[attempt] * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (!g_wl_view) return;
        WKSnapshotConfiguration *cfg = KL_AUTORELEASE([[WKSnapshotConfiguration alloc] init]);
        cfg.rect = CGRectMake(0, 0, 64, 64);
        [g_wl_view takeSnapshotWithConfiguration:cfg completionHandler:^(UIImage *img, NSError *e) {
            if (!g_wl_view) return;
            if (img && image_is_magenta(img)) { wl_done(1); return; }
            if (!img) WLOG("windowless check: no image%s%s", e ? ": " : "",
                           e ? e.localizedDescription.UTF8String : "");
            if (attempt + 1 < 4) wl_try(attempt + 1); else wl_done(0);
        }];
    });
}

static void wl_selftest(void) {
    if (g_wl_view || g_wl != -1) return;
    WKWebViewConfiguration *cfg = KL_AUTORELEASE([[WKWebViewConfiguration alloc] init]);
    g_wl_view = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 64, 64) configuration:cfg];
    [g_wl_view loadHTMLString:@"<html><body style='margin:0;background:rgb(255,0,255)'></body></html>"
                      baseURL:nil];
    WLOG("windowless check started");
    wl_try(0);
}

static int scene_is_immersive(UIScene *s) {
    NSString *role = s.session.role ?: @"";
    return [role containsString:@"Immersive"] ||
           [NSStringFromClass(s.class) containsString:@"Immersive"];
}
#endif

- (void)ensureAttached {
    int mode = env_int("KL_BS_WEB_ATTACH", 3);
    if (mode == 0 || _wv.window) return;
#if TARGET_OS_OSX
    if (!g_host_win) {
        g_host_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 4096, 4096)
                                                 styleMask:NSWindowStyleMaskBorderless
                                                   backing:NSBackingStoreBuffered defer:NO];
        g_host_win.releasedWhenClosed = NO;
        [g_host_win orderBack:nil];
        WLOG("host window: off-screen borderless window");
    }
    [g_host_win.contentView addSubview:_wv];
#else
    if (mode == 1) {
        wl_selftest();
        if (g_wl != 0) return;            // windowless works, or not known yet
    }
    if (!g_host_win || !g_host_win.windowScene ||
        g_host_win.windowScene.activationState == UISceneActivationStateUnattached) {
        UIWindowScene *best = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
            if (![s isKindOfClass:[UIWindowScene class]]) continue;
            if (mode == 1 && scene_is_immersive(s)) continue;
            if (!best || s.activationState == UISceneActivationStateForegroundActive)
                best = (UIWindowScene *)s;
        }
        if (g_host_win) { g_host_win.hidden = YES; KL_RELEASE(g_host_win); g_host_win = nil; }
        if (!best) {
            static int said;
            if (!said++) {
                for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
                    WLOG("scene %s role %s state %ld", NSStringFromClass(s.class).UTF8String,
                         s.session.role.UTF8String, (long)s.activationState);
                WLOG("host window: no ordinary window scene — views stay windowless (blank)");
            }
            return;
        }
        g_host_win = [[UIWindow alloc] initWithWindowScene:best];
        g_host_win.frame = CGRectMake(0, 0, 4096, 4096);
        g_host_win.userInteractionEnabled = NO;
        g_host_win.rootViewController = KL_AUTORELEASE([[UIViewController alloc] init]);
        if (mode == 3) {
            g_host_win.backgroundColor = UIColor.clearColor;
            g_host_win.rootViewController.view.backgroundColor = UIColor.clearColor;
            g_host_win.rootViewController.view.opaque = NO;
            g_host_win.alpha = 0.0;
        }
        g_host_win.hidden = NO;
        WLOG("host window on scene %s (%s)%s", NSStringFromClass(best.class).UTF8String,
             best.session.role.UTF8String, mode == 3 ? ", alpha 0" : "");
    }
    [g_host_win.rootViewController.view addSubview:_wv];
#endif
    WLOG("%s: view attached (%s)", _v->tag, _wv.window ? "in a window" : "still no window");
}

- (void)makeView {
    kl_web *v = _v;
    WKWebViewConfiguration *cfg = KL_AUTORELEASE([[WKWebViewConfiguration alloc] init]);
    [cfg setURLSchemeHandler:self forURLScheme:@"klasset"];
    cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
#if !TARGET_OS_OSX
    cfg.allowsInlineMediaPlayback = YES;
#endif
    WKUserContentController *ucc = cfg.userContentController;
    [ucc addScriptMessageHandler:self name:@"klvx"];
    [ucc addScriptMessageHandler:self name:@"klmedia"];
    [ucc addUserScript:KL_AUTORELEASE([[WKUserScript alloc] initWithSource:kVideoScript
        injectionTime:WKUserScriptInjectionTimeAtDocumentEnd forMainFrameOnly:NO])];
    [ucc addUserScript:KL_AUTORELEASE([[WKUserScript alloc] initWithSource:kBridgeScript
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES])];
    [ucc addUserScript:KL_AUTORELEASE([[WKUserScript alloc] initWithSource:kInputScript
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES])];
    _wv = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, v->w, v->h) configuration:cfg];
    _wv.navigationDelegate = self;
    _wv.UIDelegate = self;
#if !TARGET_OS_OSX
    _wv.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    // Opaque, white under the page like a browser: a transparent snapshot is
    // an invisible panel in the room (the first headset runs uploaded alpha 0).
    _wv.opaque = YES;
    _wv.backgroundColor = UIColor.whiteColor;
#endif
    if (@available(macOS 13.3, iOS 16.4, *)) _wv.inspectable = YES;
    [_wv addObserver:self forKeyPath:@"estimatedProgress" options:0 context:NULL];
    [_wv addObserver:self forKeyPath:@"title" options:0 context:NULL];
    [_wv addObserver:self forKeyPath:@"URL" options:0 context:NULL];
    [self ensureAttached];

    _drawQ = dispatch_queue_create("kl.web.draw", DISPATCH_QUEUE_SERIAL);
    int fps = env_int("KL_BS_WEB_FPS", 30);
    if (fps < 1) fps = 1;
    if (fps > 90) fps = 90;
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    uint64_t iv = NSEC_PER_SEC / (uint64_t)fps;
    dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)iv), iv, iv / 4);
    __unsafe_unretained KLWebHost *weak = self;   // the timer is cancelled before dealloc
    dispatch_source_set_event_handler(_timer, ^{ [weak tick]; });
    dispatch_resume(_timer);
    WLOG("%s: web view %dx%d made, snapshots at up to %d fps", v->tag, v->w, v->h, fps);
}

- (void)teardown {
    if (_timer) { dispatch_source_cancel(_timer); }
    if (_wv) {
        [_wv removeObserver:self forKeyPath:@"estimatedProgress"];
        [_wv removeObserver:self forKeyPath:@"title"];
        [_wv removeObserver:self forKeyPath:@"URL"];
        [_wv stopLoading];
        [_wv.configuration.userContentController removeScriptMessageHandlerForName:@"klvx"];
        [_wv.configuration.userContentController removeScriptMessageHandlerForName:@"klmedia"];
        _wv.navigationDelegate = nil;
        _wv.UIDelegate = nil;
        [_wv removeFromSuperview];
    }
}
@end

// ---- posting to the main thread ------------------------------------------------

static void on_main(void (^blk)(void)) {
    if ([NSThread isMainThread]) blk();
    else dispatch_async(dispatch_get_main_queue(), blk);
}

static KLWebHost *H(kl_web *v) { return (__bridge KLWebHost *)v->host; }

static NSString *S(const char *s) { return s ? ([NSString stringWithUTF8String:s] ?: @"") : @""; }

// A cookie set races the load that follows it (Bigscreen sets its access
// token and loads the cloud browser in the same breath), so loads wait until
// every cookie posted before them has landed.
static int g_cookies_pending;                       // main thread only
static NSMutableArray *g_after_cookies;            // blocks waiting on them

static void after_cookies(void (^blk)(void)) {
    if (g_cookies_pending <= 0) { blk(); return; }
    if (!g_after_cookies) g_after_cookies = [[NSMutableArray alloc] init];
    void (^c)(void) = [blk copy];
    [g_after_cookies addObject:c];
    KL_RELEASE(c);
}

kl_web *kl_web_create(int w, int h, const char *tag) {
    if (w <= 0 || h <= 0) return NULL;
#if TARGET_OS_OSX
    // A Mac host run without an application (m_boot headless) has no main
    // run loop: WebKit would be made on a thread that never services it.
    if (!NSApp) {
        static int said;
        if (!said++) WLOG("no application on this Mac run — web views stay blank");
        return NULL;
    }
#endif
    kl_web *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    pthread_mutex_init(&v->mu, NULL);
    snprintf(v->tag, sizeof v->tag, "%s", tag ? tag : "web");
    v->w = w;
    v->h = h;
    v->fluma = -1;
    atomic_store(&v->active, 1);
    on_main(^{
        KLWebHost *host = [[KLWebHost alloc] init];
        host->_v = v;
#if __has_feature(objc_arc)
        v->host = (void *)CFBridgingRetain(host);
        host = nil;
#else
        v->host = (void *)host;   // the alloc above is the one reference it holds
#endif
        [H(v) makeView];
    });
    return v;
}

void kl_web_destroy(kl_web *v) {
    if (!v) return;
    atomic_store(&v->dead, 1);
    on_main(^{
        if (v->host) {
            KLWebHost *host = H(v);
            [host teardown];
#if __has_feature(objc_arc)
            CFBridgingRelease(v->host);
#else
            [host release];
#endif
            v->host = NULL;
        }
        // The draw queue may still hold a block reading v: free after it.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            pthread_mutex_lock(&v->mu);
            kl_web_evq *e = v->eq_head;
            while (e) { kl_web_evq *n = e->next; free(e->arg); free(e); e = n; }
            free(v->front);
            pthread_mutex_unlock(&v->mu);
            pthread_mutex_destroy(&v->mu);
            free(v);
        });
    });
}

void kl_web_load_url(kl_web *v, const char *url) {
    if (!v) return;
    atomic_store(&v->poke_ms, now_ms());
    NSString *u = map_url(url);
    (void)KL_RETAIN(u);
    on_main(^{
        after_cookies(^{
            KLWebHost *host = H(v);
            if (host && host->_wv) {
                NSURL *nu = [NSURL URLWithString:u];
                if (!nu) nu = [NSURL URLWithString:[u stringByAddingPercentEncodingWithAllowedCharacters:
                                                    NSCharacterSet.URLFragmentAllowedCharacterSet]];
                WLOG("%s: load %s", v->tag, u.UTF8String);
                if (nu) [host->_wv loadRequest:[NSURLRequest requestWithURL:nu]];
            }
            KL_RELEASE(u);
        });
    });
}

void kl_web_load_html(kl_web *v, const char *html, const char *base) {
    if (!v) return;
    NSString *hs = S(html), *bs = base && *base ? map_url(base) : nil;
    (void)KL_RETAIN(hs); if (bs) (void)KL_RETAIN(bs);
    on_main(^{
        KLWebHost *host = H(v);
        if (host && host->_wv) [host->_wv loadHTMLString:hs baseURL:bs ? [NSURL URLWithString:bs] : nil];
        KL_RELEASE(hs); if (bs) KL_RELEASE(bs);
    });
}

void kl_web_set_user_agent(kl_web *v, const char *ua) {
    if (!v) return;
    NSString *s = (ua && *ua) ? S(ua) : nil;
    if (s) (void)KL_RETAIN(s);
    on_main(^{
        KLWebHost *host = H(v);
        if (host && host->_wv) host->_wv.customUserAgent = s;
        if (s) KL_RELEASE(s);
    });
}

void kl_web_set_page_script(kl_web *v, const char *js) {
    if (!v) return;
    NSString *s = S(js);
    (void)KL_RETAIN(s);
    on_main(^{
        KLWebHost *host = H(v);
        if (host) {
            KL_RELEASE(host->_pageScript);
            host->_pageScript = KL_RETAIN(s);
            // A page already up gets it now, as a finished load would.
            if (host->_wv && !host->_wv.isLoading && host->_wv.URL && s.length)
                [host->_wv evaluateJavaScript:s completionHandler:nil];
        }
        KL_RELEASE(s);
    });
}

void kl_web_eval(kl_web *v, const char *js, int rid) {
    if (!v) return;
    NSString *s = S(js);
    (void)KL_RETAIN(s);
    on_main(^{
        KLWebHost *host = H(v);
        if (host && host->_wv) {
            [host->_wv evaluateJavaScript:s completionHandler:^(id r, NSError *e) {
                if (!rid || atomic_load(&v->dead)) return;
                NSString *out = @"";
                if ([r isKindOfClass:[NSString class]]) out = r;
                else if (r && ![r isKindOfClass:[NSNull class]]) {
                    NSData *d = [NSJSONSerialization isValidJSONObject:r]
                        ? [NSJSONSerialization dataWithJSONObject:r options:0 error:nil] : nil;
                    out = d ? KL_AUTORELEASE([[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding])
                            : [r description];
                }
                evq_push(v, KL_WEB_EV_JS_RESULT, out.UTF8String, rid);
            }];
        }
        KL_RELEASE(s);
    });
}

#define SIMPLE(name, body) void name(kl_web *v) { if (!v) return; on_main(^{ KLWebHost *host = H(v); \
    if (host && host->_wv) { body; } }); }
SIMPLE(kl_web_reload, [host->_wv reload])
SIMPLE(kl_web_go_back, [host->_wv goBack])
SIMPLE(kl_web_go_forward, [host->_wv goForward])
SIMPLE(kl_web_stop, [host->_wv stopLoading])

void kl_web_resize(kl_web *v, int w, int h) {
    if (!v || w <= 0 || h <= 0) return;
    on_main(^{
        v->w = w;
        v->h = h;
        KLWebHost *host = H(v);
        if (host && host->_wv) host->_wv.frame = CGRectMake(0, 0, w, h);
    });
}

void kl_web_set_active(kl_web *v, int on) {
    if (!v) return;
    atomic_store(&v->active, on ? 1 : 0);
    // A paused page keeps its sound otherwise: Vuplex's pause() is
    // WebView.onPause(), which stops media.
    if (!on) kl_web_eval(v, "document.querySelectorAll('video,audio').forEach(function(m){m.pause()});", 0);
}

void kl_web_set_focused(kl_web *v, int on) {
    if (!v) return;
    on_main(^{
        KLWebHost *host = H(v);
        if (!host || !host->_wv) return;
        [host->_wv evaluateJavaScript:on ? @"window.focus&&window.focus();"
                                         : @"document.activeElement&&document.activeElement.blur&&document.activeElement.blur();"
                    completionHandler:nil];
    });
}

static void poke(kl_web *v) { atomic_store(&v->poke_ms, now_ms()); }

void kl_web_pointer(kl_web *v, int kind, int x, int y, int button) {
    if (!v) return;
    poke(v);
    char js[160];
    if (kind == KL_WEB_PTR_DOWN)
        snprintf(js, sizeof js, "window.__klweb&&__klweb.move(%d,%d),window.__klweb&&__klweb.down(%d,%d,%d)", x, y, x, y, button);
    else if (kind == KL_WEB_PTR_UP)
        snprintf(js, sizeof js, "window.__klweb&&__klweb.up(%d,%d,%d)", x, y, button);
    else
        snprintf(js, sizeof js, "window.__klweb&&__klweb.move(%d,%d)", x, y);
    kl_web_eval(v, js, 0);
}

void kl_web_scroll(kl_web *v, int dx, int dy, int x, int y) {
    if (!v) return;
    poke(v);
    char js[128];
    snprintf(js, sizeof js, "window.__klweb&&__klweb.wheel(%d,%d,%d,%d)", dx, dy, x, y);
    kl_web_eval(v, js, 0);
}

void kl_web_key(kl_web *v, const char *key) {
    if (!v || !key) return;
    poke(v);
    // As a JSON string literal, so any character survives.
    NSData *d = [NSJSONSerialization dataWithJSONObject:@[ S(key) ] options:0 error:nil];
    NSString *arr = d ? KL_AUTORELEASE([[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]) : @"[\"\"]";
    NSString *js = [NSString stringWithFormat:@"window.__klweb&&__klweb.key(%@[0])", arr];
    kl_web_eval(v, js.UTF8String, 0);
}

void kl_web_set_cookie_json(const char *json) {
    if (!json) return;
    NSData *d = [NSData dataWithBytes:json length:strlen(json)];
    NSDictionary *o = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
    if (![o isKindOfClass:[NSDictionary class]]) { WLOG("cookie: not JSON"); return; }
    NSMutableDictionary *p = [NSMutableDictionary dictionary];
    if (o[@"Name"])   p[NSHTTPCookieName] = o[@"Name"];
    if (o[@"Value"])  p[NSHTTPCookieValue] = o[@"Value"];
    NSString *dom = o[@"Domain"];
    NSString *url = o[@"Url"];
    if ([dom isKindOfClass:[NSString class]] && dom.length) p[NSHTTPCookieDomain] = dom;
    else if ([url isKindOfClass:[NSString class]] && url.length) p[NSHTTPCookieOriginURL] = [NSURL URLWithString:url];
    p[NSHTTPCookiePath] = ([o[@"Path"] isKindOfClass:[NSString class]] && [o[@"Path"] length]) ? o[@"Path"] : @"/";
    if ([o[@"Secure"] boolValue]) p[NSHTTPCookieSecure] = @"TRUE";
    id exp = o[@"ExpirationDate"];
    if ([exp isKindOfClass:[NSNumber class]] && [exp doubleValue] > 0)
        p[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:[exp doubleValue]];
    if ([o[@"HttpOnly"] boolValue]) p[@"HttpOnly"] = @"TRUE";
    NSHTTPCookie *c = [NSHTTPCookie cookieWithProperties:p];
    if (!c) {
        WLOG("cookie %s: not accepted (no domain?)", [o[@"Name"] description].UTF8String);
        return;
    }
    (void)KL_RETAIN(c);
    on_main(^{
        g_cookies_pending++;
        WKHTTPCookieStore *store = WKWebsiteDataStore.defaultDataStore.httpCookieStore;
        [store setCookie:c completionHandler:^{
            WLOG("cookie %s set for %s", c.name.UTF8String, c.domain.UTF8String);
            KL_RELEASE(c);
            if (--g_cookies_pending <= 0 && g_after_cookies.count) {
                NSArray *run = KL_AUTORELEASE([g_after_cookies copy]);
                [g_after_cookies removeAllObjects];
                for (void (^b)(void) in run) b();
            }
        }];
    });
}

int kl_web_next_event(kl_web *v, kl_web_ev *kind, char **arg, int *id) {
    if (!v) return 0;
    pthread_mutex_lock(&v->mu);
    kl_web_evq *e = v->eq_head;
    if (e) {
        v->eq_head = e->next;
        if (!v->eq_head) v->eq_tail = NULL;
        v->eq_n--;
    }
    pthread_mutex_unlock(&v->mu);
    if (!e) return 0;
    if (kind) *kind = e->kind;
    if (id) *id = e->id;
    if (arg) *arg = e->arg; else free(e->arg);
    free(e);
    return 1;
}

int kl_web_frame(kl_web *v, uint8_t *dst, size_t cap, int *w, int *h, uint64_t *seq) {
    if (!v) return 0;
    atomic_store(&v->wanted_ms, now_ms());
    pthread_mutex_lock(&v->mu);
    int got = 0;
    if (v->front && v->fseq != *seq) {
        size_t n = (size_t)v->fw * v->fh * 4;
        if (w) *w = v->fw;
        if (h) *h = v->fh;
        if (dst && n <= cap) {
            memcpy(dst, v->front, n);
            *seq = v->fseq;
            got = 1;
        }
    } else if (v->front) {
        if (w) *w = v->fw;
        if (h) *h = v->fh;
    }
    pthread_mutex_unlock(&v->mu);
    return got;
}

int kl_web_frame_luma(kl_web *v) {
    if (!v) return -1;
    pthread_mutex_lock(&v->mu);
    int l = v->fluma;
    pthread_mutex_unlock(&v->mu);
    return l;
}
