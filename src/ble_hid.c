/* Linux BlueZ GATT application for a Report Protocol-only BLE HID device.
 * The object model follows BlueZ 5.72 GattManager/LEAdvertisement APIs.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_hid.h"

#ifdef __linux__
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#define BLUEZ "org.bluez"
#define ROOT "/org/deskflow/otg"
#define HID_UUID "00001812-0000-1000-8000-00805f9b34fb"
#define CHAR_UUID "00002a4d-0000-1000-8000-00805f9b34fb"
#define MAP_UUID "00002a4b-0000-1000-8000-00805f9b34fb"
#define INFO_UUID "00002a4a-0000-1000-8000-00805f9b34fb"
#define REF_UUID "00002908-0000-1000-8000-00805f9b34fb"
#define BLE_POINTER_INTERVAL_NS UINT64_C(33333333)

struct otg_ble_hid;
enum ble_kind { BLE_ROOT, BLE_SERVICE, BLE_REPORT, BLE_MAP, BLE_INFO,
                BLE_REF, BLE_ADV, BLE_LEDS, BLE_CONTROL };
#define BLE_OBJECT_COUNT 12
struct ble_object { struct otg_ble_hid *b; enum ble_kind kind; int report; };

struct otg_ble_hid {
    GDBusConnection *bus;
    GDBusObjectManager *devices;
    char *peer_path;
    bool peer_connected, channel_ready;
    char *adapter;
    char *adapter_path;
    char *original_alias;
    bool alias_changed;
    guint registrations[BLE_OBJECT_COUNT];
    bool app_registered, adv_registered;
    bool resident, session_enabled;
    bool notifying[2], suspended;
    uint8_t values[2][AOA_ABSOLUTE_MOUSE_REPORT_SIZE];
    size_t value_size[2];
    uint64_t pointer_last_sent_ns;
    bool pointer_pending;
    int latency_fd;
    uint64_t pointer_interval_ns;
    uint8_t *report_map;
    size_t report_map_size;
    uint8_t *pending_desc[2];
    uint16_t pending_size[2];
    uint8_t leds;
    struct ble_object objects[BLE_OBJECT_COUNT];
};

static const gchar *xml =
"<node>"
"<interface name='org.freedesktop.DBus.ObjectManager'><method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method></interface>"
"<interface name='org.freedesktop.DBus.Properties'><method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method><signal name='PropertiesChanged'><arg type='s'/><arg type='a{sv}'/><arg type='as'/></signal></interface>"
"<interface name='org.bluez.GattService1'><property name='UUID' type='s' access='read'/><property name='Primary' type='b' access='read'/><property name='Handle' type='q' access='read'/></interface>"
"<interface name='org.bluez.GattCharacteristic1'><method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method><method name='StartNotify'/><method name='StopNotify'/><method name='WriteValue'><arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/></method><property name='UUID' type='s' access='read'/><property name='Service' type='o' access='read'/><property name='Flags' type='as' access='read'/><property name='Notifying' type='b' access='read'/><property name='Handle' type='q' access='read'/></interface>"
"<interface name='org.bluez.GattDescriptor1'><method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method><property name='UUID' type='s' access='read'/><property name='Characteristic' type='o' access='read'/><property name='Flags' type='as' access='read'/><property name='Handle' type='q' access='read'/></interface>"
"<interface name='org.bluez.LEAdvertisement1'><method name='Release'/><property name='Type' type='s' access='read'/><property name='ServiceUUIDs' type='as' access='read'/><property name='LocalName' type='s' access='read'/><property name='Appearance' type='q' access='read'/></interface>"
"</node>";

static GDBusNodeInfo *node_info;

struct pending_call {
    bool done;
    GVariant *result;
    GError *error;
};

static void call_finished(GObject *source, GAsyncResult *result, gpointer user)
{
    struct pending_call *call = user;
    call->result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                 result, &call->error);
    call->done = true;
}

/* BlueZ calls back into the exported ObjectManager while registering an
 * application.  A synchronous D-Bus call here deadlocks the GLib dispatch
 * context, so keep dispatching until the asynchronous request completes. */
static GVariant *call_wait(struct otg_ble_hid *b, const char *interface,
                            const char *method, GVariant *parameters,
                            GError **error)
{
    struct pending_call call = {0};
    g_dbus_connection_call(b->bus, BLUEZ, b->adapter_path, interface, method,
                            parameters, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                            call_finished, &call);
    while (!call.done) {
        g_main_context_iteration(NULL, TRUE);
    }
    if (call.error) {
        if (error) {
            *error = call.error;
        } else {
            g_error_free(call.error);
        }
    }
    return call.result;
}

static GVariant *strv(const char *const *v)
{ GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("as")); for (; *v; ++v) g_variant_builder_add(&b, "s", *v); return g_variant_builder_end(&b); }
static GVariant *bytes(const uint8_t *p, size_t n)
{ return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, p, n, sizeof(uint8_t)); }

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static uint64_t pointer_interval(const struct otg_ble_hid *b)
{
    return b->pointer_interval_ns ? b->pointer_interval_ns : BLE_POINTER_INTERVAL_NS;
}

static void latency_state(struct otg_ble_hid *b, bool ready)
{
    if (!ready) {
        b->pointer_pending = false;
        b->pointer_interval_ns = 0;
    }
    if (b->latency_fd < 0) {
        if (ready && !b->resident) {
            fprintf(stderr, "INFO: BLE automatic interval tuning unavailable; use isolation supervisor\n");
        }
        return;
    }
    char message[96];
    int size = snprintf(message, sizeof(message), "%s:%s\n",
                         ready ? "ready" : "not-ready", b->adapter);
    if (size > 0 && (size_t)size < sizeof(message)) {
        (void)send(b->latency_fd, message, (size_t)size,
                   MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

static void latency_poll(struct otg_ble_hid *b)
{
    if (b->latency_fd < 0) {
        return;
    }
    char message[96];
    ssize_t size;
    while ((size = recv(b->latency_fd, message, sizeof(message) - 1,
                         MSG_DONTWAIT)) > 0) {
        message[size] = '\0';
        const char *prefix = "interval_us=";
        if (!g_str_has_prefix(message, prefix)) {
            continue;
        }
        char *end;
        uint64_t us = g_ascii_strtoull(message + strlen(prefix), &end, 10);
        if ((*end != '\n' && *end != '\0') || us < 7500 || us > 4000000) {
            continue;
        }
        otg_ble_hid_set_interval(b, us);
    }
}

void otg_ble_hid_set_interval(struct otg_ble_hid *b, uint64_t us)
{
    if (us < 7500 || us > 4000000) return;
    b->pointer_interval_ns = us * UINT64_C(1000);
    if (b->pointer_interval_ns < UINT64_C(16666667))
        b->pointer_interval_ns = UINT64_C(16666667);
    fprintf(stderr, "INFO: BLE observed interval %.2fms; pointer pacing %.2fms\n",
            us / 1000.0, b->pointer_interval_ns / 1000000.0);
}

static void latency_init(struct otg_ble_hid *b)
{
    b->latency_fd = -1;
    const char *value = getenv("OTG_BLE_LATENCY_FD");
    if (!value || !*value) {
        return;
    }
    char *end;
    long fd = strtol(value, &end, 10);
    if (*end || fd < 3 || fd > INT32_MAX) {
        return;
    }
    int type;
    socklen_t size = sizeof(type);
    if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 &&
        type == SOCK_DGRAM) {
        b->latency_fd = (int)fd;
    }
}
static const char *path_for(const struct ble_object *o)
{ switch (o->kind) { case BLE_ROOT:return ROOT; case BLE_SERVICE:return ROOT "/hid"; case BLE_REPORT:return o->report ? ROOT "/hid/pointer" : ROOT "/hid/keys"; case BLE_MAP:return ROOT "/hid/map"; case BLE_INFO:return ROOT "/hid/info"; case BLE_LEDS:return ROOT "/hid/leds"; case BLE_CONTROL:return ROOT "/hid/control"; case BLE_REF:return o->report == 2 ? ROOT "/hid/leds/reference" : o->report ? ROOT "/hid/pointer/reference" : ROOT "/hid/keys/reference"; default:return ROOT "/advertisement"; } }
static const char *iface_for(enum ble_kind k)
{ switch (k) { case BLE_ROOT:return "org.freedesktop.DBus.ObjectManager"; case BLE_SERVICE:return "org.bluez.GattService1"; case BLE_LEDS: case BLE_CONTROL: case BLE_REPORT: case BLE_MAP: case BLE_INFO:return "org.bluez.GattCharacteristic1"; case BLE_REF:return "org.bluez.GattDescriptor1"; default:return "org.bluez.LEAdvertisement1"; } }

/* Stable, contiguous schema: BlueZ characteristic Handle is the VALUE handle.
 * CCCDs occupy 0x107/0x10b and remain managed by BlueZ, not exported twice. */
static uint16_t object_handle(const struct ble_object *o)
{
    switch (o->kind) {
    case BLE_SERVICE: return 0x100;
    case BLE_MAP: return 0x102;
    case BLE_INFO: return 0x104;
    case BLE_REPORT: return o->report ? 0x10a : 0x106;
    case BLE_REF: return o->report == 2 ? 0x10f : o->report ? 0x10c : 0x108;
    case BLE_LEDS: return 0x10e;
    case BLE_CONTROL: return 0x111;
    default: return 0;
    }
}

static void reset_reports(struct otg_ble_hid *b)
{
    memset(b->values, 0, sizeof(b->values));
    b->value_size[0] = b->value_size[1] = 8;
    b->pointer_pending = false;
    b->pointer_last_sent_ns = 0;
}

/* Pin the BlueZ identity address, not a transient advertising address. Never
 * switch a live GATT application's peer: its cached CCCs belong to that peer. */
bool otg_ble_hid_set_peer(struct otg_ble_hid *b, const char *address)
{
    if (!address || strlen(address) != 17) return false;
    char identity[18];
    for (unsigned i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (address[i] != ':') return false;
            identity[i] = '_';
        } else {
            if (!g_ascii_isxdigit(address[i])) return false;
            identity[i] = g_ascii_toupper(address[i]);
        }
    }
    identity[17] = 0;
    char *path = g_strdup_printf("%s/dev_%s", b->adapter_path, identity);
    bool ok = !b->peer_path || !strcmp(b->peer_path, path);
    if (ok && !b->peer_path) b->peer_path = g_strdup(path);
    g_free(path);
    return ok;
}

/* BlueZ 5.72 retains CCC subscriptions for bonded peers across disconnects;
 * StopNotify is NOT a physical-link disconnect notification. Gate readiness
 * on Device1 Connected/Paired and keep the service alive for cached CCCs. */
static void refresh_peer(struct otg_ble_hid *b)
{
    if (!b->devices) return;
    char *owner=g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(b->devices));
    if (!owner) {b->app_registered=false;b->peer_connected=false;return;}
    g_free(owner);
    GList *objects=g_dbus_object_manager_get_objects(b->devices);
    unsigned connected=0;char *candidate=NULL;bool paired=false;
    for(GList *it=objects;it;it=it->next) {
        GDBusObject *object=it->data;
        const char *path=g_dbus_object_get_object_path(object);
        size_t prefix=strlen(b->adapter_path);
        if(strncmp(path,b->adapter_path,prefix) || path[prefix]!='/') continue;
        GDBusInterface *interface=g_dbus_object_get_interface(object,"org.bluez.Device1");
        if(!interface) continue;
        GVariant *value=g_dbus_proxy_get_cached_property(G_DBUS_PROXY(interface),"Connected");
        if(value && g_variant_get_boolean(value)) {
            connected++;g_free(candidate);candidate=g_strdup(path);
            GVariant *bond=g_dbus_proxy_get_cached_property(G_DBUS_PROXY(interface),"Paired");
            paired=bond && g_variant_get_boolean(bond);
            if(bond)g_variant_unref(bond);
        }
        if(value)g_variant_unref(value);
        g_object_unref(interface);
    }
    g_list_free_full(objects,g_object_unref);
    bool same=!b->peer_path || (candidate && !strcmp(candidate,b->peer_path));
    b->peer_connected=connected==1 && paired && same;
    if(!b->peer_connected) b->suspended=false;
    if(b->peer_connected && !b->peer_path && b->notifying[0] && b->notifying[1])
        b->peer_path=g_strdup(candidate);
    g_free(candidate);
}

static void update_channel_state(struct otg_ble_hid *b)
{
    bool ready=(!b->resident || b->session_enabled) &&
               b->app_registered && b->peer_connected && b->notifying[0] &&
               b->notifying[1] && !b->suspended;
    if(ready==b->channel_ready) return;
    b->channel_ready=ready;
    if(!ready) reset_reports(b);
    latency_state(b,ready);
    fprintf(stderr,"INFO: BLE HID input channel %s; pairing and GATT service retained\n",
            ready ? "ready" : "disconnected");
}

static bool set_adapter_alias(struct otg_ble_hid *b, const char *alias)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        b->bus, BLUEZ, b->adapter_path, "org.freedesktop.DBus.Properties",
        "Set", g_variant_new("(ssv)", "org.bluez.Adapter1", "Alias",
                              g_variant_new_string(alias)), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (!reply) {
        fprintf(stderr, "ERROR: cannot set Bluetooth adapter name: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

static bool configure_adapter_alias(struct otg_ble_hid *b)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        b->bus, BLUEZ, b->adapter_path, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", "org.bluez.Adapter1", "Alias"), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (!reply) {
        fprintf(stderr, "ERROR: cannot read Bluetooth adapter name: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    GVariant *value = NULL;
    g_variant_get(reply, "(v)", &value);
    const char *alias = value ? g_variant_get_string(value, NULL) : NULL;
    b->original_alias = alias ? g_strdup(alias) : NULL;
    if (value) {
        g_variant_unref(value);
    }
    g_variant_unref(reply);
    if (!b->original_alias) {
        fprintf(stderr, "ERROR: Bluetooth adapter returned no name\n");
        return false;
    }
    if (strcmp(b->original_alias, "Deskflow OTG") == 0) {
        return true;
    }
    if (!set_adapter_alias(b, "Deskflow OTG")) {
        g_clear_pointer(&b->original_alias, g_free);
        return false;
    }
    b->alias_changed = true;
    fprintf(stderr, "INFO: Bluetooth adapter name changed from \"%s\" to \"Deskflow OTG\"\n",
            b->original_alias);
    return true;
}

static bool restore_adapter_alias(struct otg_ble_hid *b)
{
    if (!b->alias_changed || !b->original_alias || !b->bus) {
        return true;
    }
    if (set_adapter_alias(b, b->original_alias)) {
        fprintf(stderr, "INFO: Bluetooth adapter name restored to \"%s\"\n",
                b->original_alias);
        b->alias_changed = false;
        return true;
    }
    return false;
}

static GVariant *props(const struct ble_object *o, const char *interface)
{
    struct otg_ble_hid *b=o->b; GVariantBuilder d; g_variant_builder_init(&d,G_VARIANT_TYPE("a{sv}"));
    if (o->kind==BLE_SERVICE) { g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(HID_UUID)); g_variant_builder_add(&d,"{sv}","Primary",g_variant_new_boolean(TRUE)); }
    else if (o->kind==BLE_MAP) { g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(MAP_UUID)); g_variant_builder_add(&d,"{sv}","Service",g_variant_new_object_path(ROOT "/hid")); g_variant_builder_add(&d,"{sv}","Flags",strv((const char*[]){"read","encrypt-read",NULL})); }
    else if (o->kind==BLE_INFO) { g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(INFO_UUID)); g_variant_builder_add(&d,"{sv}","Service",g_variant_new_object_path(ROOT "/hid")); g_variant_builder_add(&d,"{sv}","Flags",strv((const char*[]){"read",NULL})); }
    else if (o->kind==BLE_REPORT) { g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(CHAR_UUID)); g_variant_builder_add(&d,"{sv}","Service",g_variant_new_object_path(ROOT "/hid")); const char *f[] = {"read","encrypt-read","notify","encrypt-notify",NULL}; g_variant_builder_add(&d,"{sv}","Flags",strv(f)); g_variant_builder_add(&d,"{sv}","Notifying",g_variant_new_boolean(b->notifying[o->report])); }
    else if (o->kind==BLE_REF) { g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(REF_UUID)); g_variant_builder_add(&d,"{sv}","Characteristic",g_variant_new_object_path(path_for(&(struct ble_object){b,o->report == 2 ? BLE_LEDS : BLE_REPORT,o->report}))); g_variant_builder_add(&d,"{sv}","Flags",strv((const char*[]){"read",NULL})); }
    else if (o->kind==BLE_LEDS || o->kind==BLE_CONTROL) {
        g_variant_builder_add(&d,"{sv}","UUID",g_variant_new_string(o->kind==BLE_LEDS ? CHAR_UUID : "00002a4c-0000-1000-8000-00805f9b34fb"));
        g_variant_builder_add(&d,"{sv}","Service",g_variant_new_object_path(ROOT "/hid"));
        g_variant_builder_add(&d,"{sv}","Flags",o->kind==BLE_LEDS ? strv((const char*[]){"read","encrypt-read","write","write-without-response","encrypt-write",NULL}) : strv((const char*[]){"write-without-response","encrypt-write",NULL}));
    }
    else if (o->kind==BLE_ADV) { g_variant_builder_add(&d,"{sv}","Type",g_variant_new_string("peripheral")); g_variant_builder_add(&d,"{sv}","ServiceUUIDs",strv((const char*[]){HID_UUID,NULL})); g_variant_builder_add(&d,"{sv}","LocalName",g_variant_new_string("Deskflow OTG")); g_variant_builder_add(&d,"{sv}","Appearance",g_variant_new_uint16(0x03c0)); }
    if (object_handle(o)) g_variant_builder_add(&d,"{sv}","Handle",g_variant_new_uint16(object_handle(o)));
    (void)interface; return g_variant_builder_end(&d);
}

static void changed(struct otg_ble_hid *b, int report)
{
    char *owner = NULL;
    if (b->devices) {
        refresh_peer(b);
        if (!b->peer_connected) return;
        owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(b->devices));
        if (!owner) return;
    }
    struct ble_object o={b,BLE_REPORT,report};
    GVariantBuilder d; g_variant_builder_init(&d,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&d,"{sv}","Value",bytes(b->values[report],b->value_size[report]));
    /* Input reports are private to BlueZ, never broadcast on the system bus. */
    g_dbus_connection_emit_signal(b->bus,owner,path_for(&o),"org.freedesktop.DBus.Properties",
        "PropertiesChanged",g_variant_new("(s@a{sv}@as)","org.bluez.GattCharacteristic1",
        g_variant_builder_end(&d),g_variant_new_strv(NULL,0)),NULL);
    g_free(owner);
}

static bool target_options(struct otg_ble_hid *b, GVariant *options)
{
    if (!b->peer_path) return true;
    const char *device = NULL;
    return g_variant_lookup(options,"device","&o",&device) && !strcmp(device,b->peer_path);
}

static void method(GDBusConnection *c,const gchar *sender,const gchar *path,const gchar *iface,const gchar *name,GVariant *p,GDBusMethodInvocation *inv,gpointer user)
{
    struct ble_object *o=user; struct otg_ble_hid *b=o->b;
    if (b->devices) {
        char *owner=g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(b->devices));
        bool allowed=owner && sender && !strcmp(owner,sender);
        g_free(owner);
        if(!allowed) {
            g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.NotAuthorized","BlueZ only");return;
        }
    }
    if (o->kind==BLE_ROOT && !strcmp(name,"GetManagedObjects")) { GVariantBuilder top; g_variant_builder_init(&top,G_VARIANT_TYPE("a{oa{sa{sv}}}")); for (int i=1;i<BLE_OBJECT_COUNT-1;i++) { struct ble_object *x=&b->objects[i]; if (!x->b) continue; GVariantBuilder interfaces; g_variant_builder_init(&interfaces,G_VARIANT_TYPE("a{sa{sv}}")); g_variant_builder_add_value(&interfaces,g_variant_new_dict_entry(g_variant_new_string(iface_for(x->kind)),props(x,iface_for(x->kind)))); g_variant_builder_add_value(&top,g_variant_new_dict_entry(g_variant_new_object_path(path_for(x)),g_variant_builder_end(&interfaces))); } g_dbus_method_invocation_return_value(inv,g_variant_new("(@a{oa{sa{sv}}})",g_variant_builder_end(&top))); return; }
    if (!strcmp(iface,"org.freedesktop.DBus.Properties") && !strcmp(name,"GetAll")) { const char *which; g_variant_get(p,"(&s)",&which); g_dbus_method_invocation_return_value(inv,g_variant_new("(@a{sv})",props(o,which))); return; }
    if (o->kind==BLE_ADV && !strcmp(name,"Release")) { g_dbus_method_invocation_return_value(inv,NULL); return; }
    if ((o->kind==BLE_MAP || o->kind==BLE_INFO || o->kind==BLE_REF || o->kind==BLE_REPORT || o->kind==BLE_LEDS) && !strcmp(name,"ReadValue")) {
        const uint8_t info[]={0x11,1,0,2};
        const uint8_t ref[]={ (uint8_t)(o->report==2 ? 1 : o->report+1), (uint8_t)(o->report==2 ? 2 : 1)};
        const uint8_t *value; size_t size;
        if(o->kind==BLE_MAP){value=b->report_map;size=b->report_map_size;}
        else if(o->kind==BLE_INFO){value=info;size=sizeof(info);}
        else if(o->kind==BLE_REF){value=ref;size=sizeof(ref);}
        else if(o->kind==BLE_LEDS){value=&b->leds;size=1;}
        else {value=b->values[o->report];size=b->value_size[o->report];}
        GVariant *options=g_variant_get_child_value(p,0); guint16 offset=0;
        bool allowed=(o->kind!=BLE_REPORT && o->kind!=BLE_LEDS) || target_options(b,options);
        g_variant_lookup(options,"offset","q",&offset); g_variant_unref(options);
        if(!allowed){g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.NotAuthorized","wrong target");return;}
        if(offset>size){g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.InvalidOffset","offset exceeds value");return;}
        g_dbus_method_invocation_return_value(inv,g_variant_new("(@ay)",bytes(value ? value+offset : NULL,size-offset))); return;
    }
    if (o->kind==BLE_REPORT && !strcmp(name,"StartNotify")) { b->notifying[o->report]=true; fprintf(stderr,"INFO: BLE %s report subscribed\n",o->report?"pointer":"keyboard"); changed(b,o->report); g_dbus_method_invocation_return_value(inv,NULL); return; }
    if (o->kind==BLE_REPORT && !strcmp(name,"StopNotify")) { b->notifying[o->report]=false; latency_state(b, false); reset_reports(b); fprintf(stderr,"INFO: BLE %s report unsubscribed\n",o->report?"pointer":"keyboard"); g_dbus_method_invocation_return_value(inv,NULL); return; }
    if ((o->kind==BLE_LEDS || o->kind==BLE_CONTROL) && !strcmp(name,"WriteValue")) {
        GVariant *v,*options; g_variant_get(p,"(@ay@a{sv})",&v,&options);
        gsize size; const uint8_t *value=g_variant_get_fixed_array(v,&size,1);
        guint16 offset=0; g_variant_lookup(options,"offset","q",&offset);
        bool valid=target_options(b,options) && !offset && size==1 && (o->kind==BLE_LEDS || value[0]<=1);
        if(valid){if(o->kind==BLE_LEDS)b->leds=value[0];else {bool suspend=value[0]==0;reset_reports(b);b->suspended=suspend;}}
        g_variant_unref(options);g_variant_unref(v);
        if(!valid){g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.InvalidArguments","expected one valid byte at offset zero");return;}
        g_dbus_method_invocation_return_value(inv,NULL);return;
    }
    g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.NotSupported","unsupported operation");
    (void)c;(void)sender;(void)path;
}

static const GDBusInterfaceVTable vtable={.method_call=method,.get_property=NULL,.set_property=NULL};
static bool register_objects(struct otg_ble_hid *b)
{ reset_reports(b); GError *e=NULL; for(int i=0;i<BLE_OBJECT_COUNT;i++){ b->objects[i]=(struct ble_object){b,(enum ble_kind[]){BLE_ROOT,BLE_SERVICE,BLE_MAP,BLE_INFO,BLE_REPORT,BLE_REPORT,BLE_REF,BLE_REF,BLE_LEDS,BLE_REF,BLE_CONTROL,BLE_ADV}[i],i==9 ? 2 : (i==5||i==7)}; b->registrations[i]=g_dbus_connection_register_object(b->bus,path_for(&b->objects[i]),g_dbus_node_info_lookup_interface(node_info,iface_for(b->objects[i].kind)),&vtable,&b->objects[i],NULL,&e); if(!b->registrations[i]){fprintf(stderr,"ERROR: BLE object registration: %s\n",e->message);g_clear_error(&e);return false;}} return true; }

static bool start_advertisement(struct otg_ble_hid *b)
{
    GError *error=NULL;
    GVariant *reply=call_wait(b,"org.bluez.LEAdvertisingManager1","RegisterAdvertisement",
        g_variant_new("(o@a{sv})",ROOT "/advertisement",g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0)),&error);
    if(!reply){fprintf(stderr,"ERROR: BlueZ RegisterAdvertisement: %s\n",error?error->message:"unknown error");g_clear_error(&error);return false;}
    g_variant_unref(reply);b->adv_registered=true;return true;
}

static bool register_blueZ(struct otg_ble_hid *b)
{
    GError *error=NULL;
    GVariant *reply=call_wait(b,"org.bluez.GattManager1","RegisterApplication",
        g_variant_new("(o@a{sv})",ROOT,g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0)),&error);
    if(!reply){fprintf(stderr,"ERROR: BlueZ RegisterApplication: %s\n",error?error->message:"unknown error");g_clear_error(&error);return false;}
    g_variant_unref(reply);b->app_registered=true;
    return b->resident || start_advertisement(b);
}

bool otg_ble_hid_set_session(struct otg_ble_hid *b, bool enabled)
{
    if(!b->resident)return false;
    if(enabled) {
        if(b->session_enabled)return true;
        if(!b->app_registered || !configure_adapter_alias(b))return false;
        if(!start_advertisement(b)){restore_adapter_alias(b);return false;}
        reset_reports(b);b->pointer_interval_ns=0;b->suspended=false;
        b->session_enabled=true;
        update_channel_state(b);
        return true;
    }
    /* Release before disabling input. Keep genuine StartNotify evidence and
     * the GATT registration; never infer subscriptions from Connected. */
    if(b->session_enabled) {
        reset_reports(b);
        if(b->notifying[0])changed(b,0);
        if(b->notifying[1])changed(b,1);
    }
    b->session_enabled=false;b->pointer_interval_ns=0;
    update_channel_state(b);
    bool ok=true;
    if(b->adv_registered) {
        GError *error=NULL;
        GVariant *reply=call_wait(b,"org.bluez.LEAdvertisingManager1","UnregisterAdvertisement",
            g_variant_new("(o)",ROOT "/advertisement"),&error);
        ok=reply!=NULL;
        if(reply){g_variant_unref(reply);b->adv_registered=false;}
        else fprintf(stderr,"ERROR: cannot stop resident BLE advertisement: %s\n",error?error->message:"unknown error");
        g_clear_error(&error);
    }
    if(!restore_adapter_alias(b))ok=false;
    if(!b->alias_changed)g_clear_pointer(&b->original_alias,g_free);
    fprintf(stderr,"%s: BLE session %s; inputs released, GATT registration retained\n",
            ok?"INFO":"ERROR",ok?"idle":"cleanup incomplete");
    return ok;
}

static bool reg_initial(void *ctx,uint16_t id,const uint8_t *desc,uint16_t size)
{ struct otg_ble_hid*b=ctx; int n=id==AOA_HID_ID_MOUSE; if(b->pending_desc[n])free(b->pending_desc[n]); b->pending_desc[n]=malloc(size+2); if(!b->pending_desc[n])return false; memcpy(b->pending_desc[n],desc,size); b->pending_size[n]=size; if(id==AOA_HID_ID_KEYBOARD){b->pending_desc[n][size]=0;b->pending_desc[n][size+1]=0;} if(b->pending_desc[0]&&b->pending_desc[1]&&!b->app_registered){ size_t total=b->pending_size[0]+b->pending_size[1]+4;b->report_map=malloc(total);if(!b->report_map)return false; size_t p=0; for(int i=0;i<2;i++){const uint8_t*d=b->pending_desc[i];size_t z=b->pending_size[i];size_t at=6;memcpy(b->report_map+p,d,at);p+=at;b->report_map[p++]=0x85;b->report_map[p++]=(uint8_t)(i+1);memcpy(b->report_map+p,d+at,z-at);p+=z-at;}b->report_map_size=p;if(!register_blueZ(b))return false;}return true; }
static bool reg(void *ctx,uint16_t id,const uint8_t *desc,uint16_t size)
{
    struct otg_ble_hid *b=ctx;
    if((id!=AOA_HID_ID_KEYBOARD && id!=AOA_HID_ID_MOUSE) || size<6)return false;
    int n=id==AOA_HID_ID_MOUSE;
    if(b->resident && b->app_registered)
        return b->pending_size[n]==size && !memcmp(b->pending_desc[n],desc,size);
    return reg_initial(ctx,id,desc,size);
}

/* Only coordinates may be replaced. Button, range and wheel transitions are
 * ordering barriers, including a release to zero buttons. Drag motion with
 * unchanged buttons can be coalesced just like hover motion. */
static bool queue_pointer(struct otg_ble_hid *b, const uint8_t *data,
                           uint16_t size, uint64_t now)
{
    bool motion = size == AOA_ABSOLUTE_MOUSE_REPORT_SIZE &&
                  b->value_size[1] == size &&
                  data[0] == b->values[1][0] &&
                  data[1] == b->values[1][1] &&
                  data[6] == 0 && data[7] == 0;
    memcpy(b->values[1], data, size);
    b->value_size[1] = size;
    if (motion && b->pointer_last_sent_ns &&
        now - b->pointer_last_sent_ns < pointer_interval(b)) {
        b->pointer_pending = true;
        return false;
    }
    b->pointer_pending = false;
    b->pointer_last_sent_ns = now;
    return true;
}

static bool pointer_due(struct otg_ble_hid *b, uint64_t now)
{
    if ((b->resident && !b->session_enabled) ||
        !b->app_registered || !b->notifying[0] || !b->notifying[1] ||
        b->suspended) {
        b->pointer_pending = false;
        return false;
    }
    if (!b->pointer_pending ||
        now - b->pointer_last_sent_ns < pointer_interval(b)) {
        return false;
    }
    b->pointer_pending = false;
    b->pointer_last_sent_ns = now;
    return true;
}

static bool send_report(void *ctx, uint16_t id, const uint8_t *data,
                         uint16_t size)
{
    struct otg_ble_hid *b = ctx;
    if (id != AOA_HID_ID_KEYBOARD && id != AOA_HID_ID_MOUSE) {
        return false;
    }
    int i = id == AOA_HID_ID_MOUSE;
    if ((b->resident && !b->session_enabled) ||
        !b->app_registered || !b->notifying[0] || !b->notifying[1] ||
        b->suspended || size > sizeof(b->values[i])) {
        return false;
    }
    if (i) {
        if (!queue_pointer(b, data, size, monotonic_ns())) {
            return true;
        }
    } else {
        memcpy(b->values[0], data, size);
        b->value_size[0] = size;
    }
    changed(b, i);
    return true;
}

static bool poll_ble(void *ctx)
{
    struct otg_ble_hid *b = ctx;
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    refresh_peer(b);
    update_channel_state(b);
    latency_poll(b);
    if (pointer_due(b, monotonic_ns())) {
        changed(b, 1);
    }
    return b->app_registered;
}
static bool ready_ble(void *ctx){struct otg_ble_hid*b=ctx;return b->channel_ready;}
static void close_ble(void *ctx){struct otg_ble_hid*b=ctx;if(!b)return;GError*e=NULL;if(b->adv_registered){GVariant*r=call_wait(b,"org.bluez.LEAdvertisingManager1","UnregisterAdvertisement",g_variant_new("(o)",ROOT "/advertisement"),&e);if(r)g_variant_unref(r);g_clear_error(&e);b->adv_registered=false;}if(b->app_registered){GVariant*r=call_wait(b,"org.bluez.GattManager1","UnregisterApplication",g_variant_new("(o)",ROOT),&e);if(r)g_variant_unref(r);g_clear_error(&e);b->app_registered=false;}for(int i=0;i<BLE_OBJECT_COUNT;i++)if(b->registrations[i]){g_dbus_connection_unregister_object(b->bus,b->registrations[i]);b->registrations[i]=0;}}
static const struct otg_hid_transport_ops OPS={reg,send_report,poll_ble,ready_ble,close_ble};
const struct otg_hid_transport_ops*otg_ble_hid_ops(void){return &OPS;}
static bool init_mode(struct otg_ble_hid *b,const char *adapter,bool resident)
{
    memset(b,0,sizeof(*b));b->resident=resident;
    if(resident)b->latency_fd=-1;else latency_init(b);
    b->adapter=g_strdup(adapter ? adapter : "hci0");
    if(!b->adapter)return false;
    b->adapter_path=g_strdup_printf("/org/bluez/%s",b->adapter);
    GError *error=NULL;
    b->bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&error);
    if(!b->bus){fprintf(stderr,"ERROR: system D-Bus: %s\n",error->message);g_clear_error(&error);return false;}
    b->devices=g_dbus_object_manager_client_new_sync(b->bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,&error);
    if(!b->devices){fprintf(stderr,"ERROR: BlueZ device monitor: %s\n",error->message);g_clear_error(&error);return false;}
    if(!resident && !configure_adapter_alias(b))return false;
    node_info=g_dbus_node_info_new_for_xml(xml,&error);
    if(!node_info){g_clear_error(&error);return false;}
    return register_objects(b);
}
bool otg_ble_hid_init(struct otg_ble_hid *b,const char *adapter){return init_mode(b,adapter,false);}
struct otg_ble_hid *otg_ble_hid_create_resident(const char *adapter){struct otg_ble_hid*b=calloc(1,sizeof(*b));if(b&&!init_mode(b,adapter,true)){otg_ble_hid_destroy(b);return NULL;}return b;}
struct otg_ble_hid *otg_ble_hid_create(const char *adapter){struct otg_ble_hid*b=calloc(1,sizeof(*b));if(b&&!otg_ble_hid_init(b,adapter)){otg_ble_hid_destroy(b);return NULL;}return b;}
void otg_ble_hid_destroy(struct otg_ble_hid*b){if(!b)return;close_ble(b);restore_adapter_alias(b);if(b->latency_fd>=0)close(b->latency_fd);for(int i=0;i<2;i++)free(b->pending_desc[i]);free(b->report_map);if(b->devices)g_object_unref(b->devices);if(b->bus)g_object_unref(b->bus);g_free(b->adapter_path);g_free(b->adapter);g_free(b->peer_path);g_free(b->original_alias);free(b);}
const char*otg_ble_hid_adapter(const struct otg_ble_hid*b){return b?b->adapter:NULL;}
#endif
