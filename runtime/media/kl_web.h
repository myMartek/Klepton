// kl_web — an offscreen system web view (WKWebView) driven from C.
//
// Bigscreen's browser panels are Vuplex 3D WebView on Android: the game asks
// for a web view of a size, loads URLs into it, sends it pointer and key
// input, and samples its picture from a GL texture. This is the host side of
// that: one WKWebView per view, kept on the main thread, its picture taken as
// RGBA snapshots for the GL thread to upload, and its page events queued for
// the game thread to forward. Every call is safe from any thread.
#ifndef KL_WEB_H
#define KL_WEB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_web kl_web;

// Asset source for "jar:file:...apk!/assets/x" URLs: the bytes of `name`
// ("assets/web/x.html") or NULL. The result is malloc'd; *len is set.
typedef void *(*kl_web_asset_fn)(const char *name, size_t *len);
void kl_web_set_asset_reader(kl_web_asset_fn fn);

kl_web *kl_web_create(int w, int h, const char *tag);
void    kl_web_destroy(kl_web *v);

void kl_web_load_url(kl_web *v, const char *url);
void kl_web_load_html(kl_web *v, const char *html, const char *base_url);
// NULL or "" puts the system default back.
void kl_web_set_user_agent(kl_web *v, const char *ua);
// JavaScript run in the page; with id != 0 the result comes back as an
// event of kind KL_WEB_EV_JS_RESULT carrying that id.
void kl_web_eval(kl_web *v, const char *js, int rid);
void kl_web_reload(kl_web *v);
void kl_web_go_back(kl_web *v);
void kl_web_go_forward(kl_web *v);
void kl_web_stop(kl_web *v);
void kl_web_resize(kl_web *v, int w, int h);
void kl_web_set_active(kl_web *v, int on);   // pause()/resume()
void kl_web_set_focused(kl_web *v, int on);
// Script injected after every page load (Vuplex's page bridge); kept per view.
void kl_web_set_page_script(kl_web *v, const char *js);

// Input in view pixels (0..w, 0..h, top-left origin).
enum { KL_WEB_PTR_MOVE = 0, KL_WEB_PTR_DOWN = 1, KL_WEB_PTR_UP = 2 };
void kl_web_pointer(kl_web *v, int kind, int x, int y, int button /*0 l,1 m,2 r*/);
void kl_web_scroll(kl_web *v, int dx, int dy, int x, int y);
void kl_web_key(kl_web *v, const char *key);   // "a", "Enter", "Backspace", ...

// Cookie as Vuplex sends it: {"Name","Value","Domain","Path",...} JSON.
void kl_web_set_cookie_json(const char *json);

// Page events, oldest first. Returns 1 and fills the out-params (malloc'd
// strings the caller frees) or 0 when none is waiting.
typedef enum {
    KL_WEB_EV_LOAD_STARTED = 1,   // arg: url
    KL_WEB_EV_LOAD_PROGRESS,      // arg: "0.42"
    KL_WEB_EV_LOAD_FINISHED,      // arg: url
    KL_WEB_EV_LOAD_FAILED,        // arg: "description,url"
    KL_WEB_EV_URL_CHANGED,        // arg: url
    KL_WEB_EV_TITLE_CHANGED,      // arg: title
    KL_WEB_EV_MESSAGE,            // arg: what the page posted (string)
    KL_WEB_EV_JS_RESULT,          // arg: result string; id set
    KL_WEB_EV_MEDIA,              // arg: "1" a video/audio plays, "0" none does
} kl_web_ev;
int kl_web_next_event(kl_web *v, kl_web_ev *kind, char **arg, int *id);

// Newest picture: copies RGBA rows (top row first) into dst when a frame
// newer than *seq exists, updates *seq, and returns 1; 0 otherwise. The
// frame is always w x h of the view's current size (*w, *h are set).
int kl_web_frame(kl_web *v, uint8_t *dst, size_t cap, int *w, int *h,
                 uint64_t *seq);
// Pixel stats of the newest frame, for logs: mean luma 0..255, -1 if none.
int kl_web_frame_luma(kl_web *v);

#ifdef __cplusplus
}
#endif
#endif
