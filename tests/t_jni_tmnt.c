#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_jni_int.h"
#include "guest/kl_driver.h"
extern const klj_binding klj_bind_net[];
extern int klj_horizon_message_field(void *, const char *, const char *, const char *, klj_val *);
#define CORE "horizonos/supplement/hzplatformclientcore/HorizonPlatformCore"
#define MSG "horizonos/supplement/hzplatformclientcore/HorizonMessage"
static klj_val call(const char *name, klj_val *a, int n) {
    for (const klj_binding *b = klj_bind_net; b->cls; b++)
        if (!strcmp(b->cls, CORE) && !strcmp(b->name, name)) return b->fn(kl_jni_env(), NULL, a, n);
    abort();
}
static uint64_t request(const char *service, const char *method, int cookie) {
    klj_val a[] = {{.l=kl_jni_new_string(service)}, {.l=kl_jni_new_string(method)}, {.j=1},
                  {.l=kl_jni_new_string("{}")}, {.j=cookie}};
    return call("makeRequest", a, 5).j;
}
static klj_val field(void *obj, const char *name, const char *sig) {
    klj_val v; assert(klj_horizon_message_field(obj, MSG, name, sig, &v)); return v;
}
int main(int argc, char **argv) {
    kl_target t = {0}; t.name = argc > 1 ? argv[1] : "x6635073589916590";
    kl_driver_init(&t, NULL, 0); kl_jni_env();
    setenv("KL_PLAT_USER_ID", "1234567890123456", 1);
    int tmnt = !strcmp(t.name, "x6635073589916590");
    uint64_t id = request("users", "get_logged_in_user", 17);
    assert(call("getMessageCount", NULL, 0).j == (unsigned)tmnt);
    if (tmnt) {
        void *msg = call("popMessage", NULL, 0).l; assert(msg);
        assert(field(msg, "requestId", "J").j == id);
        assert(field(msg, "cookie", "I").j == 17);
        assert(field(msg, "statusCode", "I").j == 0);
        const char *data = klj_str(field(msg, "data", "Ljava/lang/String;").l);
        assert(strstr(data, "1234567890123456") && strstr(data, "OFFLINE"));
        assert(!call("popMessage", NULL, 0).l);
        // An invalid id must never enter JSON or produce a shared identity.
        setenv("KL_PLAT_USER_ID", "bad\"id", 1);
        request("users", "get_logged_in_user", 0);
        assert(!call("getMessageCount", NULL, 0).j);
    }
    id = request("users", "get_org_scoped_id", 21);
    assert(call("getMessageCount", NULL, 0).j == (unsigned)tmnt);
    if (tmnt) {
        void *m = call("popMessage", NULL, 0).l;
        assert(field(m, "requestId", "J").j == id);
        assert(field(m, "statusCode", "I").j != 0);
        assert(field(m, "cookie", "I").j == 21);
        assert(strstr(klj_str(field(m, "statusMessage", "Ljava/lang/String;").l), "unavailable"));
        assert(!strcmp(klj_str(field(m, "data", "Ljava/lang/String;").l), "{}"));
    }
    id = request("iap", "get_viewer_purchases", 31);
    assert(call("getMessageCount", NULL, 0).j == (unsigned)tmnt);
    if (tmnt) {
        void *m = call("popMessage", NULL, 0).l;
        assert(field(m, "requestId", "J").j == id);
        assert(field(m, "statusCode", "I").j != 0);
        assert(field(m, "cookie", "I").j == 31);
        assert(!strcmp(klj_str(field(m, "data", "Ljava/lang/String;").l), "{}"));
    }
    request("users", "get_access_token", 0);
    request("users", "get_user_proof", 0);
    request("iap", "get_purchases", 0);
    assert(!call("getMessageCount", NULL, 0).j);
    id = request("entitlements", "get_is_viewer_entitled", 0);
    assert(call("getMessageCount", NULL, 0).j == 1);
    void *msg = call("popMessage", NULL, 0).l;
    assert(field(msg, "requestId", "J").j == id);
    assert(!field(msg, "data", "Ljava/lang/String;").l);
    puts("TMNT Horizon response checks passed");
}
