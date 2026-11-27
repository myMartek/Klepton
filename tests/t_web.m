// t_web — kl_web against a real WKWebView on the Mac.
//
// The game side runs on a worker thread, as on the headset; the main thread
// runs the run loop WebKit needs. Checks: a page from the asset scheme loads
// (start/finish events), its bridge posts a message, the snapshot has the
// page's colour, a synthesised click reaches a listener, keys type into a
// focused field, and scrolling moves the page.
#import <AppKit/AppKit.h>
#include "kl_web.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("  ok: " __VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *PAGE =
"<!doctype html><html><head><link rel=stylesheet href='s.css'></head>"
"<body style='margin:0;height:3000px'>"
"<div id=box style='width:100%;height:100%;position:fixed;left:0;top:0'></div>"
"<input id=t style='position:fixed;left:10px;top:10px;width:200px;height:30px'>"
"<script>"
"var box=document.getElementById('box');"
"box.addEventListener('click',function(e){box.style.background='rgb(0,0,255)';"
"_vuplexBridge.postMessage('clicked '+e.clientX+','+e.clientY);});"
"document.getElementById('t').addEventListener('input',function(e){"
"_vuplexBridge.postMessage('typed '+e.target.value);});"
"window.addEventListener('scroll',function(){_vuplexBridge.postMessage('scrolled '+Math.round(window.scrollY));});"
"_vuplexBridge.postMessage('hello');"
"</script></body></html>";

static void *reader(const char *name, size_t *len) {
    const char *s = NULL;
    if (strcmp(name, "assets/web/t.html") == 0) s = PAGE;
    else if (strcmp(name, "assets/web/s.css") == 0) s = "#box{background:rgb(255,0,0)}";
    if (!s) return NULL;
    *len = strlen(s);
    return strdup(s);
}

static kl_web *V;
static char got_msgs[4096];
static int started, finished;

static void pump(int ms) {
    for (int t = 0; t < ms; t += 20) {
        kl_web_ev k; char *a; int id;
        while (kl_web_next_event(V, &k, &a, &id)) {
            if (k == KL_WEB_EV_LOAD_STARTED) started++;
            if (k == KL_WEB_EV_LOAD_FINISHED) finished++;
            if (k == KL_WEB_EV_MESSAGE) {
                strncat(got_msgs, a, sizeof got_msgs - strlen(got_msgs) - 2);
                strcat(got_msgs, "|");
            }
            free(a);
        }
        // Keep asking for frames, as the GL thread does every frame.
        uint64_t seq = UINT64_MAX; int w, h;
        kl_web_frame(V, NULL, 0, &w, &h, &seq);
        usleep(20000);
    }
}

static uint8_t px[800 * 450 * 4];
static int centre_rgb(int *r, int *g, int *b) {
    static uint64_t seq;
    int w = 0, h = 0;
    for (int i = 0; i < 100; i++) {
        if (kl_web_frame(V, px, sizeof px, &w, &h, &seq)) break;
        usleep(20000);
    }
    if (w != 800 || h != 450) return 0;
    const uint8_t *p = px + ((size_t)300 * w + 400) * 4;
    *r = p[0]; *g = p[1]; *b = p[2];
    return 1;
}

static void *game(void *arg) {
    kl_web_set_asset_reader(reader);
    V = kl_web_create(800, 450, "t");
    kl_web_load_url(V, "jar:file:///nowhere/x.apk!/assets/web/t.html");
    pump(3000);
    CHECK(started >= 1 && finished >= 1, "load started (%d) and finished (%d)", started, finished);
    CHECK(strstr(got_msgs, "hello|") != NULL, "page bridge posted 'hello' (%s)", got_msgs);

    int r = 0, g = 0, b = 0;
    pump(500);
    CHECK(centre_rgb(&r, &g, &b) && r > 200 && g < 50 && b < 50,
          "snapshot shows the red page via the asset scheme's CSS (%d,%d,%d)", r, g, b);

    kl_web_pointer(V, KL_WEB_PTR_MOVE, 400, 300, 0);
    kl_web_pointer(V, KL_WEB_PTR_DOWN, 400, 300, 0);
    kl_web_pointer(V, KL_WEB_PTR_UP, 400, 300, 0);
    pump(800);
    CHECK(strstr(got_msgs, "clicked 400,300|") != NULL, "click arrived at 400,300");
    CHECK(centre_rgb(&r, &g, &b) && b > 200 && r < 50, "snapshot turned blue (%d,%d,%d)", r, g, b);

    kl_web_pointer(V, KL_WEB_PTR_DOWN, 50, 25, 0);
    kl_web_pointer(V, KL_WEB_PTR_UP, 50, 25, 0);
    kl_web_key(V, "h");
    kl_web_key(V, "i");
    kl_web_key(V, "Backspace");
    kl_web_key(V, "o");
    pump(800);
    CHECK(strstr(got_msgs, "typed ho|") != NULL, "keys typed into the field (h, i, Backspace, o)");

    kl_web_scroll(V, 0, 400, 400, 300);
    pump(800);
    CHECK(strstr(got_msgs, "scrolled 400|") != NULL, "scroll moved the page by 400 (%s)", got_msgs);
    printf("  (eval)\n"); fflush(stdout);

    kl_web_eval(V, "1+2", 7);
    int seen = 0;
    for (int t = 0; t < 50 && !seen; t++) {
        kl_web_ev k; char *a; int id;
        while (kl_web_next_event(V, &k, &a, &id)) {
            if (k == KL_WEB_EV_JS_RESULT && id == 7) seen = atoi(a);
            free(a);
        }
        usleep(20000);
    }
    CHECK(seen == 3, "eval result came back (%d)", seen);
    fflush(stdout);

    kl_web_destroy(V);
    printf("%s\n", fails ? "t_web: FAILED" : "t_web: all passed");
    exit(fails ? 1 : 0);
    return NULL;
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        // A windowless background process is napped: its sleeps stretch to
        // seconds and the whole test takes half a minute.
        id act = [[NSProcessInfo processInfo] beginActivityWithOptions:
                  NSActivityUserInitiated | NSActivityLatencyCritical reason:@"t_web"];
        (void)act;
        pthread_t t;
        pthread_create(&t, NULL, game, NULL);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            printf("t_web: timed out\n");
            exit(2);
        });
        [NSApp run];
    }
    return 0;
}
