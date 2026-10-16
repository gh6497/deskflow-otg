/* Real D-Bus dispatch on a private bus, not system Bluetooth.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/ble_hid.c"

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
struct mock_bluez {
    const char *address;
    GMainContext *context;
    GMainLoop *loop;
    int ready,registered,unregistered,advertised,stopped,reject_stop;
    char alias[64];
    GDBusConnection *bus;
    int connected_mask, unpaired_mask, private_path;
};
static const char *manager_xml=
"<node><interface name='org.freedesktop.DBus.Properties'>"
"<method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
"<method name='Set'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/></method></interface>"
"<interface name='org.bluez.GattManager1'>"
"<method name='RegisterApplication'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterApplication'><arg type='o' direction='in'/></method></interface>"
"<interface name='org.bluez.LEAdvertisingManager1'>"
"<method name='RegisterAdvertisement'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterAdvertisement'><arg type='o' direction='in'/></method></interface>"
"<interface name='org.freedesktop.DBus.ObjectManager'><method name='GetManagedObjects'>"
"<arg type='a{oa{sa{sv}}}' direction='out'/></method></interface></node>";
static void manager_method(GDBusConnection *bus,const char *sender,const char *path,
    const char *interface,const char *name,GVariant *args,GDBusMethodInvocation *inv,void *user)
{
    (void)bus;(void)sender;(void)path;(void)interface;
    struct mock_bluez *mock=user;
    if(!strcmp(name,"GetManagedObjects")) {
        GVariantBuilder objects;g_variant_builder_init(&objects,G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        bool privacy=g_atomic_int_get(&mock->private_path)!=0;
        const char *paths[]={privacy ? "/org/bluez/hci0/dev_52_00_00_00_00_01" :
            "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF","/org/bluez/hci0/dev_11_22_33_44_55_66",
            "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"};
        for(int i=0;i<(privacy?3:2);i++) {
            GVariantBuilder interfaces,properties;
            g_variant_builder_init(&interfaces,G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_init(&properties,G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&properties,"{sv}","Address",g_variant_new_string(i==1 ?
                "11:22:33:44:55:66" : "AA:BB:CC:DD:EE:FF"));
            g_variant_builder_add(&properties,"{sv}","Connected",g_variant_new_boolean(!!(g_atomic_int_get(&mock->connected_mask)&(1<<i))));
            g_variant_builder_add(&properties,"{sv}","Paired",g_variant_new_boolean(!(g_atomic_int_get(&mock->unpaired_mask)&(1<<i))));
            g_variant_builder_add(&interfaces,"{s@a{sv}}","org.bluez.Device1",g_variant_builder_end(&properties));
            g_variant_builder_add(&objects,"{o@a{sa{sv}}}",paths[i],g_variant_builder_end(&interfaces));
        }
        g_dbus_method_invocation_return_value(inv,g_variant_new("(@a{oa{sa{sv}}})",g_variant_builder_end(&objects)));return;
    }
    if(!strcmp(name,"Get")) {
        g_dbus_method_invocation_return_value(inv,g_variant_new("(v)",g_variant_new_string(mock->alias)));return;
    }
    if(!strcmp(name,"Set")) {
        const char *iface,*property;GVariant *value;
        g_variant_get(args,"(&s&sv)",&iface,&property,&value);
        CHECK(!strcmp(iface,"org.bluez.Adapter1") && !strcmp(property,"Alias"));
        g_strlcpy(mock->alias,g_variant_get_string(value,NULL),sizeof(mock->alias));g_variant_unref(value);
    } else if(!strcmp(name,"RegisterApplication"))g_atomic_int_inc(&mock->registered);
    else if(!strcmp(name,"UnregisterApplication"))g_atomic_int_inc(&mock->unregistered);
    else if(!strcmp(name,"RegisterAdvertisement"))g_atomic_int_inc(&mock->advertised);
    else if(!strcmp(name,"UnregisterAdvertisement")) {
        if(g_atomic_int_get(&mock->reject_stop)) {
            g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Failed","injected stop failure");return;
        }
        g_atomic_int_inc(&mock->stopped);
    } else CHECK(false);
    g_dbus_method_invocation_return_value(inv,NULL);
}
static gpointer manager_thread(gpointer data)
{
    struct mock_bluez *mock=data;
    mock->context=g_main_context_new();g_main_context_push_thread_default(mock->context);
    mock->loop=g_main_loop_new(mock->context,FALSE);
    GDBusConnection *bus=g_dbus_connection_new_for_address_sync(mock->address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,NULL);
    CHECK(bus);
    mock->bus=bus;
    GVariant *reply=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus",
        "org.freedesktop.DBus","RequestName",g_variant_new("(su)",BLUEZ,0),NULL,0,1000,NULL,NULL);
    CHECK(reply);g_variant_unref(reply);
    GDBusNodeInfo *info=g_dbus_node_info_new_for_xml(manager_xml,NULL);
    const GDBusInterfaceVTable table={.method_call=manager_method};guint ids[4];
    for(int i=0;i<4;i++) {
        ids[i]=g_dbus_connection_register_object(bus,i==3?"/":"/org/bluez/hci0",info->interfaces[i],&table,mock,NULL,NULL);
        CHECK(ids[i]);
    }
    g_atomic_int_set(&mock->ready,1);g_main_loop_run(mock->loop);
    for(int i=0;i<4;i++)g_dbus_connection_unregister_object(bus,ids[i]);
    g_dbus_node_info_unref(info);g_dbus_connection_close_sync(bus,NULL,NULL);g_object_unref(bus);
    g_main_loop_unref(mock->loop);g_main_context_pop_thread_default(mock->context);g_main_context_unref(mock->context);
    return NULL;
}
static gboolean stop_manager(gpointer data){g_main_loop_quit(data);return G_SOURCE_REMOVE;}
static GVariant *invoke(struct otg_ble_hid *b,const char *path,const char *iface,
                         const char *method_name,GVariant *args,bool success)
{
    struct pending_call call={0};
    g_dbus_connection_call(b->bus,g_dbus_connection_get_unique_name(b->bus),path,
        iface,method_name,args,NULL,0,1000,NULL,call_finished,&call);
    while(!call.done)g_main_context_iteration(NULL,TRUE);
    CHECK(success ? call.result!=NULL : call.error!=NULL);
    g_clear_error(&call.error);
    return call.result;
}
static GVariant *read_at(struct otg_ble_hid *b,const char *path,guint16 offset,bool ok)
{
    GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","offset",g_variant_new_uint16(offset));
    return invoke(b,path,"org.bluez.GattCharacteristic1","ReadValue",
                  g_variant_new("(a{sv})",&options),ok);
}
static void write_byte(struct otg_ble_hid *b,const char *path,uint8_t value,bool ok)
{
    GVariant *result=invoke(b,path,"org.bluez.GattCharacteristic1","WriteValue",
        g_variant_new("(@ay@a{sv})",bytes(&value,1),g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0)),ok);
    if(result)g_variant_unref(result);
}
int main(void)
{
    char *daemon=g_find_program_in_path("dbus-daemon");
    if(!daemon)return 77;
    g_free(daemon);
    GTestDBus *test=g_test_dbus_new(G_TEST_DBUS_NONE);g_test_dbus_up(test);
    struct mock_bluez mock={.address=g_test_dbus_get_bus_address(test),.alias="original-host"};
    GThread *thread=g_thread_new("mock-bluez",manager_thread,&mock);
    while(!g_atomic_int_get(&mock.ready))g_usleep(1000);
    struct otg_ble_hid b={.latency_fd=-1};
    b.bus=g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(test),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,NULL,NULL);
    CHECK(b.bus);
    node_info=g_dbus_node_info_new_for_xml(xml,NULL);CHECK(node_info);
    CHECK(register_objects(&b));
    uint8_t map[]={1,2,3,4,5,6,7,8};b.report_map=map;b.report_map_size=sizeof(map);
    GVariant *result=invoke(&b,ROOT,"org.freedesktop.DBus.ObjectManager","GetManagedObjects",NULL,true);
    GVariant *objects=g_variant_get_child_value(result,0);
    CHECK(g_variant_n_children(objects)==10);g_variant_unref(objects);g_variant_unref(result);
    result=invoke(&b,ROOT "/hid/keys","org.freedesktop.DBus.Properties","GetAll",
        g_variant_new("(s)","org.bluez.GattCharacteristic1"),true);
    GVariant *props_value=g_variant_get_child_value(result,0);guint16 handle=0;
    CHECK(g_variant_lookup(props_value,"Handle","q",&handle) && handle==0x106);
    g_variant_unref(props_value);g_variant_unref(result);
    result=read_at(&b,ROOT "/hid/map",3,true);
    GVariant *value=g_variant_get_child_value(result,0);gsize size;
    const uint8_t *data=g_variant_get_fixed_array(value,&size,1);
    CHECK(size==5 && !memcmp(data,map+3,5));g_variant_unref(value);g_variant_unref(result);
    result=read_at(&b,ROOT "/hid/map",8,true);g_variant_unref(result);
    CHECK(!read_at(&b,ROOT "/hid/map",9,false));
    result=read_at(&b,ROOT "/hid/keys",0,true);
    value=g_variant_get_child_value(result,0);data=g_variant_get_fixed_array(value,&size,1);
    CHECK(size==8 && data[2]==0);g_variant_unref(value);g_variant_unref(result);
    write_byte(&b,ROOT "/hid/leds",2,true);CHECK(b.leds==2);
    write_byte(&b,ROOT "/hid/control",0,true);CHECK(b.suspended);
    write_byte(&b,ROOT "/hid/control",1,true);CHECK(!b.suspended);
    write_byte(&b,ROOT "/hid/control",3,false);CHECK(!b.suspended);
    write_byte(&b,ROOT "/hid/keys",4,false);
    /* Real private D-Bus lifecycle: session teardown removes advertising, not
     * the registered GATT service; a second session needs no new StartNotify. */
    b.resident=true;b.adapter="hci0";b.adapter_path="/org/bluez/hci0";
    CHECK(register_blueZ(&b));CHECK(b.app_registered && !b.adv_registered);
    CHECK(otg_ble_hid_set_session(&b,true));
    b.peer_connected=true;update_channel_state(&b);CHECK(!b.channel_ready);
    result=invoke(&b,ROOT "/hid/keys","org.bluez.GattCharacteristic1","StartNotify",NULL,true);g_variant_unref(result);
    result=invoke(&b,ROOT "/hid/pointer","org.bluez.GattCharacteristic1","StartNotify",NULL,true);g_variant_unref(result);
    update_channel_state(&b);CHECK(b.channel_ready);
    b.values[0][2]=4;b.values[1][1]=1;b.pointer_pending=true;
    CHECK(otg_ble_hid_set_session(&b,false));
    CHECK(b.app_registered && !b.adv_registered && !b.channel_ready);
    CHECK(b.notifying[0] && b.notifying[1]);
    CHECK(!b.values[0][2] && !b.values[1][1] && !b.pointer_pending);
    CHECK(!g_atomic_int_get(&mock.unregistered));
    CHECK(otg_ble_hid_set_session(&b,true));CHECK(b.channel_ready);
    CHECK(g_atomic_int_get(&mock.registered)==1 && g_atomic_int_get(&mock.advertised)==2);
    g_atomic_int_set(&mock.reject_stop,1);
    CHECK(!otg_ble_hid_set_session(&b,false));CHECK(!b.channel_ready && b.adv_registered);
    g_atomic_int_set(&mock.reject_stop,0);
    CHECK(otg_ble_hid_set_session(&b,false));CHECK(!b.adv_registered);
    /* A real ObjectManager snapshot on the private bus exercises identity,
     * pairing and multi-peer fail-closed behavior, without system Bluetooth. */
    CHECK(otg_ble_hid_set_peer(&b,"AA:BB:CC:DD:EE:FF"));
    for(int mask=0;mask<4;mask++) {
        g_atomic_int_set(&mock.connected_mask,mask);
        g_clear_object(&b.devices);
        b.devices=g_dbus_object_manager_client_new_sync(b.bus,
            G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
        CHECK(b.devices);
        refresh_peer(&b);
        CHECK(b.peer_connected==(mask==1));
        b.session_enabled=true;update_channel_state(&b);
        CHECK(b.channel_ready==(mask==1));
    }
    /* Identity Address changes need not rename an object's original private
     * path. An offline bonded alias must never authenticate the live link. */
    g_atomic_int_set(&mock.private_path,1);g_atomic_int_set(&mock.connected_mask,1);
    g_clear_object(&b.devices);
    b.devices=g_dbus_object_manager_client_new_sync(b.bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
    CHECK(b.devices);refresh_peer(&b);update_channel_state(&b);
    CHECK(b.channel_ready && !strcmp(b.peer_path,"/org/bluez/hci0/dev_52_00_00_00_00_01"));
    GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path(b.peer_path));
    GVariant *target=g_variant_ref_sink(g_variant_builder_end(&options));
    CHECK(target_options(&b,target));g_variant_unref(target);
    g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options,"{sv}","device",g_variant_new_object_path("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"));
    target=g_variant_ref_sink(g_variant_builder_end(&options));
    CHECK(!target_options(&b,target));g_variant_unref(target);
    g_atomic_int_set(&mock.connected_mask,1);g_atomic_int_set(&mock.unpaired_mask,1);
    g_clear_object(&b.devices);
    b.devices=g_dbus_object_manager_client_new_sync(b.bus,
        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,BLUEZ,"/",NULL,NULL,NULL,NULL,NULL);
    CHECK(b.devices);refresh_peer(&b);update_channel_state(&b);
    CHECK(!b.peer_connected && !b.peer_path && !b.channel_ready);
    CHECK(strstr(b.wait_reason,"not paired"));
    CHECK(!read_at(&b,ROOT "/hid/keys",0,false)); // Non-BlueZ D-Bus caller rejected.
    result=invoke(&b,ROOT "/hid/keys","org.bluez.GattCharacteristic1","StartNotify",NULL,false);
    CHECK(!result);
    close_ble(&b);
    CHECK(g_atomic_int_get(&mock.unregistered)==1);
    g_free(b.original_alias);
    g_clear_object(&b.devices);g_free(b.peer_path);
    g_dbus_node_info_unref(node_info);node_info=NULL;
    g_dbus_connection_close_sync(b.bus,NULL,NULL);g_object_unref(b.bus);
    g_main_context_invoke(mock.context,stop_manager,mock.loop);g_thread_join(thread);
    g_test_dbus_down(test);g_object_unref(test);
    puts("PASS: GATT export, stable handles, long-read offsets, neutral reports and control point");
    return 0;
}
