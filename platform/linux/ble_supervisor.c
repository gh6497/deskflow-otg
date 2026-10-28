/* Native Linux BLE isolation and recovery; Linux Bluetooth MGMT API.
 * No bluetoothd restart, bond deletion, config edits, Python or libbluetooth.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ble_supervisor.h"
#include "ble_link.h"
#include "ble_session.h"
#ifdef __linux__
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BIT(n) (UINT32_C(1) << (n))
#define RESTORE_MASK (BIT(0)|BIT(1)|BIT(2)|BIT(3)|BIT(4)|BIT(5)|BIT(6)|BIT(7)|BIT(9)|BIT(11))
struct hci_address { sa_family_t family; uint16_t index, channel; };
struct controller {
    int fd;
    uint16_t index;
    uint8_t address[6];
    uint32_t supported, settings;
    char *alias;
    bool (*command)(struct controller *, uint16_t, const void *, uint16_t,
                     uint8_t *, size_t *);
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2) << 16; }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

static bool mgmt_command(struct controller *c, uint16_t opcode,
                           const void *data, uint16_t size,
                           uint8_t *reply, size_t *reply_size)
{
    uint8_t packet[1024];
    if (size > sizeof(packet)-6) return false;
    put16(packet,opcode); put16(packet+2,c->index); put16(packet+4,size);
    if (size) memcpy(packet+6,data,size);
    if (send(c->fd,packet,size+6,0) != size+6) return false;
    gint64 deadline=g_get_monotonic_time()+10000000;
    while (g_get_monotonic_time()<deadline) {
        struct pollfd pfd={c->fd,POLLIN,0};
        int r=poll(&pfd,1,100);
        if (r<0 && errno==EINTR) continue;
        if (r<0) return false;
        if (!r) continue;
        if (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) return false;
        ssize_t n=recv(c->fd,packet,sizeof(packet),0);
        if (n<9 || le16(packet+2)!=c->index || le16(packet+4)!=(size_t)n-6) continue;
        uint16_t event=le16(packet);
        if (event!=1 && event!=2) continue;
        /* MGMT Command Complete and Command Status both carry opcode LE16
         * followed by status. HCI Command Status has a different layout. */
        if (le16(packet+6)!=opcode) continue;
        uint8_t status=packet[8];
        if (status) {
            fprintf(stderr,"ERROR: Bluetooth MGMT 0x%04x status=0x%02x\n",opcode,status);
            return false;
        }
        if (event==2) continue; /* status accepted is not completion */
        if (reply_size) {
            size_t available=(size_t)n-9;
            if (*reply_size<available) return false;
            if (reply) memcpy(reply,packet+9,available);
            *reply_size=available;
        }
        return true;
    }
    fprintf(stderr,"ERROR: Bluetooth MGMT 0x%04x timed out\n",opcode);
    return false;
}

static bool read_info(struct controller *c)
{
    uint8_t reply[512]; size_t size=sizeof(reply);
    if (!c->command(c,4,NULL,0,reply,&size) || size<280) return false;
    memcpy(c->address,reply,6);
    c->supported=le32(reply+9); c->settings=le32(reply+13);
    return true;
}

static bool set_mode(struct controller *c, uint16_t opcode, uint8_t mode)
{
    uint8_t data[3]={mode,0,0};
    return c->command(c,opcode,data,opcode==6 ? 3 : 1,NULL,NULL);
}

/* Restore only the settings that changed. Always attempt the original power
 * state even if one setting failed; retain recovery state on any failure. */
static bool apply_settings(struct controller *c, uint32_t wanted)
{
    if (!read_info(c)) return false;
    bool ok=true;
    /* Discovery must be disabled while still powered, before restoring an
     * originally powered-off/non-discoverable controller. */
    if ((c->settings & (BIT(0)|BIT(3)))==(BIT(0)|BIT(3)) && !(wanted & BIT(3))) {
        if (!set_mode(c,6,0)) ok=false;
    }
    uint32_t modes=RESTORE_MASK & ~(BIT(0)|BIT(3));
    if ((c->settings ^ wanted) & modes) {
        if ((c->settings & BIT(0)) && !set_mode(c,5,0)) ok=false;
        /* Enable desired bearers before disabling an old one, so a BR/EDR-
         * only snapshot can be restored without first disabling the last LE
         * bearer. Unsupported operations are never sent. */
        const struct {unsigned bit;uint16_t opcode;} bearers[]={{9,0x0d},{7,0x2a}};
        for(size_t i=0;i<G_N_ELEMENTS(bearers);i++) {
            if(!read_info(c)){ok=false;continue;}
            uint32_t flag=BIT(bearers[i].bit);
            if((wanted & c->supported & flag) && !(c->settings & flag) &&
               !set_mode(c,bearers[i].opcode,1))ok=false;
        }
        const struct { unsigned bit; uint16_t opcode; } order[]={
            {9,0x0d},{7,0x2a},{6,0x0b},{11,0x2d},
            {5,0x0a},{2,8},{1,7},{4,9}
        };
        for (size_t i=0;i<G_N_ELEMENTS(order);++i) {
            if (!read_info(c)) {ok=false;continue;}
            uint32_t flag=BIT(order[i].bit);
            if ((c->supported & flag) && ((c->settings ^ wanted) & flag) &&
                !set_mode(c,order[i].opcode,!!(wanted & flag))) ok=false;
        }
    }
    bool powered=false;
    for (int attempt=0;attempt<3;++attempt) {
        if (read_info(c) && !((c->settings ^ wanted) & BIT(0))) {powered=true;break;}
        (void)set_mode(c,5,!!(wanted & BIT(0)));
        g_usleep(200000);
    }
    if (!powered && read_info(c)) powered=!((c->settings ^ wanted)&BIT(0));
    if (!powered) ok=false;
    if (wanted & BIT(0)) {
        if (!read_info(c)) ok=false;
        else if (((c->settings ^ wanted) & BIT(3)) &&
                 !set_mode(c,6,!!(wanted & BIT(3)))) ok=false;
    }
    if (!read_info(c) || ((c->settings ^ wanted) & RESTORE_MASK)) ok=false;
    return ok;
}

static GVariant *adapter_properties(GDBusConnection *bus, const char *path,
                                     const char *interface)
{
    GError *error=NULL;
    GVariant *reply=g_dbus_connection_call_sync(bus,"org.bluez",path,
        "org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)",interface),
        G_VARIANT_TYPE("(a{sv})"),0,5000,NULL,&error);
    if (!reply) {
        fprintf(stderr,"ERROR: BlueZ properties: %s\n",error->message);
        g_clear_error(&error); return NULL;
    }
    GVariant *props=g_variant_get_child_value(reply,0);
    g_variant_unref(reply); return props;
}

static bool preflight(GDBusConnection *bus, const char *path, struct controller *c)
{
    GVariant *props=adapter_properties(bus,path,"org.bluez.Adapter1");
    if (!props) return false;
    guint32 discover=0,pair=0; const char *alias=NULL;
    g_variant_lookup(props,"DiscoverableTimeout","u",&discover);
    g_variant_lookup(props,"PairableTimeout","u",&pair);
    g_variant_lookup(props,"Alias","&s",&alias);
    c->alias=g_strdup(alias);
    g_variant_unref(props);
    if (discover || pair || !c->alias) {
        fprintf(stderr,"ERROR: timed discovery/pairing cannot be restored safely\n"); return false;
    }
    props=adapter_properties(bus,path,"org.bluez.LEAdvertisingManager1");
    if (!props) return false;
    guint8 active=0;
    g_variant_lookup(props,"ActiveInstances","y",&active); g_variant_unref(props);
    if (active) fprintf(stderr,"ERROR: stop existing BLE advertisements before isolation\n");
    return active==0;
}

static bool snapshot_save(const char *path, const struct controller *c)
{
    GKeyFile *key=g_key_file_new();
    char *addr=g_strdup_printf("%02x%02x%02x%02x%02x%02x",c->address[0],c->address[1],c->address[2],c->address[3],c->address[4],c->address[5]);
    g_key_file_set_string(key,"BLE","address",addr);
    g_key_file_set_uint64(key,"BLE","settings",c->settings);
    g_key_file_set_string(key,"BLE","alias",c->alias);
    gsize size; char *data=g_key_file_to_data(key,&size,NULL);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    bool ok=false;
    if (fd>=0) {
        ok=write(fd,data,size)==(ssize_t)size && fsync(fd)==0;
        close(fd);
        if (!ok) unlink(path); /* Only the file just created by this call. */
    }
    g_free(data); g_free(addr);g_key_file_unref(key);
    return ok;
}

static bool snapshot_load(const char *path, const struct controller *c,
                           uint32_t *settings, char **alias)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if (fd<0) return false;
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==0 &&
            (st.st_mode & 0777)==0600 && st.st_size>0 && st.st_size<4096;
    char data[4096]; ssize_t size=ok ? read(fd,data,sizeof(data)) : -1;
    close(fd);
    if (size<=0 || size!=st.st_size) return false;
    GKeyFile *key=g_key_file_new(); GError *error=NULL;
    ok=g_key_file_load_from_data(key,data,(gsize)size,0,&error);
    char *addr=ok ? g_key_file_get_string(key,"BLE","address",NULL) : NULL;
    char expected[13];
    snprintf(expected,sizeof(expected),"%02x%02x%02x%02x%02x%02x",c->address[0],c->address[1],c->address[2],c->address[3],c->address[4],c->address[5]);
    guint64 flags=ok ? g_key_file_get_uint64(key,"BLE","settings",&error) : 0;
    *alias=ok ? g_key_file_get_string(key,"BLE","alias",NULL) : NULL;
    ok=ok && !error && addr && !strcmp(addr,expected) && flags<=UINT32_MAX &&
       *alias && strlen(*alias)<=248;
    if (ok) *settings=(uint32_t)flags;
    g_free(addr);g_clear_error(&error);g_key_file_unref(key);
    return ok;
}

static bool restore_alias(GDBusConnection *bus,const char *path,const char *alias)
{
    GVariant *props=adapter_properties(bus,path,"org.bluez.Adapter1");
    if (!props) return false;
    const char *current=NULL;g_variant_lookup(props,"Alias","&s",&current);
    bool change=current && !strcmp(current,"Deskflow OTG") && strcmp(current,alias);
    g_variant_unref(props);
    if (!change) return true; /* Do not overwrite another application's edit. */
    GError *error=NULL;
    GVariant *reply=g_dbus_connection_call_sync(bus,"org.bluez",path,
        "org.freedesktop.DBus.Properties","Set",g_variant_new("(ssv)",
        "org.bluez.Adapter1","Alias",g_variant_new_string(alias)),NULL,0,5000,NULL,&error);
    if (!reply) {g_clear_error(&error);return false;}
    g_variant_unref(reply);return true;
}

static bool parse_index(const char *adapter, uint16_t *index)
{
    if (strncmp(adapter,"hci",3) || !g_ascii_isdigit(adapter[3])) return false;
    char *end; unsigned long n=strtoul(adapter+3,&end,10);
    if (*end || n>=UINT16_MAX) return false;
    *index=(uint16_t)n;return true;
}

static void child_finished(GObject *object,GAsyncResult *result,gpointer user)
{
    (void)g_subprocess_wait_finish(G_SUBPROCESS(object),result,NULL);
    *(bool *)user=true;
}

int otg_ble_supervise(int argc,char **argv,const char *adapter,
                      int restore_only,volatile sig_atomic_t *stop)
{
    struct controller c={.fd=-1,.command=mgmt_command};
    if (!parse_index(adapter,&c.index)) {fprintf(stderr,"ERROR: invalid adapter\n");return 2;}
    if (geteuid()!=0) {
        fprintf(stderr,"ERROR: --ble-isolate/--ble-restore require sudo; isolation disconnects ALL Bluetooth links on %s\n",adapter);
        return 2;
    }
    const char *uid_string=getenv("SUDO_UID"); char *end=NULL;
    if(!uid_string)uid_string=getenv("PKEXEC_UID");
    unsigned long uid=uid_string ? strtoul(uid_string,&end,10) : 0;
    struct passwd *user=uid_string && end && !*end && uid>0 && uid<=UINT32_MAX ? getpwuid((uid_t)uid) : NULL;
    if (!restore_only && !user) {fprintf(stderr,"ERROR: run via sudo or pkexec from an ordinary user account\n");return 2;}
    char *username=user ? g_strdup(user->pw_name) : NULL;
    char *state=g_strdup_printf("/run/deskflow-otg-ble-%s.state",adapter);
    char *lock_path=g_strdup_printf("/run/deskflow-otg-ble-%s.lock",adapter);
    char *object_path=g_strdup_printf("/org/bluez/%s",adapter);
    int lock=open(lock_path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    GDBusConnection *bus=NULL;GSubprocess *child=NULL;int result=1;
    bool saved=false;uint32_t original=0;char *original_alias=NULL;
    struct otg_ble_link *link=NULL;int ipc[2]={-1,-1};
    bool resident_session=false;
    bool gui=false;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--ble-session"))resident_session=true;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--gui"))gui=true;
    struct stat lock_stat;
    if (lock<0 || fstat(lock,&lock_stat) || lock_stat.st_uid!=0 ||
        !S_ISREG(lock_stat.st_mode) || flock(lock,LOCK_EX|LOCK_NB)) {
        fprintf(stderr,"ERROR: another BLE supervisor owns %s or lock is unsafe\n",adapter);goto out;
    }
    c.fd=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_CLOEXEC,1);
    struct hci_address address={AF_BLUETOOTH,UINT16_MAX,3};
    if (c.fd<0 || bind(c.fd,(struct sockaddr *)&address,sizeof(address)) || !read_info(&c)) {
        fprintf(stderr,"ERROR: cannot read Bluetooth controller (no changes made)\n");goto out;
    }
    bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,NULL);
    if (!bus) goto out;
    if (restore_only) {
        if (!snapshot_load(state,&c,&original,&original_alias)) {
            fprintf(stderr,"ERROR: missing/invalid recovery snapshot or adapter identity differs: %s\n",state);goto out;
        }
        saved=true;goto restore;
    }
    if (access(state,F_OK)==0) {
        fprintf(stderr,"ERROR: recovery snapshot exists; first run: sudo %s --ble-restore --adapter %s\n",argv[0],adapter);goto out;
    }
    if (!(c.supported & BIT(9)) || (c.settings & (BIT(8)|BIT(12)))) {
        fprintf(stderr,"ERROR: unsupported LE controller or special debug/high-speed settings; no changes made\n");goto out;
    }
    if(resident_session && !otg_ble_service_wait_idle((uid_t)uid,adapter)) {
        fprintf(stderr,"ERROR: start an idle --ble-service as the sudo caller before isolation (no changes made)\n");goto out;
    }
    if(!preflight(bus,object_path,&c)) goto out;
    if(*stop){result=0;goto out;}
    if(gui) {
        struct pollfd input={STDIN_FILENO,POLLIN,0};
        /* Cancellation while a desktop authentication dialog was open must
         * not change controller modes after the user has already disconnected. */
        if(poll(&input,1,0)>0){result=0;goto out;}
    }
    original=c.settings;original_alias=g_strdup(c.alias);
    if (!snapshot_save(state,&c)) {fprintf(stderr,"ERROR: cannot save recovery snapshot\n");goto out;}
    saved=true;
    fprintf(stderr,"WARN: isolating %s as BLE-only; all its Bluetooth links will disconnect\nINFO: recovery: sudo %s --ble-restore --adapter %s\n",adapter,argv[0],adapter);
    if (!apply_settings(&c,(original|BIT(0)|BIT(1)|BIT(9)|BIT(4)|BIT(3)) & ~(BIT(7)|BIT(6)))) goto restore;
    if (*stop) goto restore;
    /* runuser is a native privilege-drop utility. The bridge never runs as
     * root; no shell, Python, arbitrary supervisor command, or setuid install. */
    char executable[4096];ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if (length<0 || length==(ssize_t)sizeof(executable)-1) goto restore;
    executable[length]='\0';
    char **args=g_new0(char *,argc+6);int n=0;
    args[n++]="/usr/sbin/runuser";args[n++]="-u";args[n++]=username;args[n++]="--";args[n++]=executable;
    for (int i=1;i<argc;++i) if (strcmp(argv[i],"--ble-isolate")) args[n++]=argv[i];
    GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_INHERIT);
    g_subprocess_launcher_unsetenv(launcher,"OTG_BLE_LATENCY_FD");
    if(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0,ipc)==0)
        link=otg_ble_link_open(c.index,ipc[0]);
    if(link) {
        g_subprocess_launcher_take_fd(launcher,ipc[1],3);ipc[1]=-1;
        g_subprocess_launcher_setenv(launcher,"OTG_BLE_LATENCY_FD","3",TRUE);
    } else fprintf(stderr,"WARN: native BLE timing monitor unavailable; keeping normal input channel\n");
    GError *error=NULL;
    child=g_subprocess_launcher_spawnv(launcher,(const char *const *)args,&error);
    g_object_unref(launcher);g_free(args);
    if (!child) {fprintf(stderr,"ERROR: launch bridge: %s\n",error->message);g_clear_error(&error);goto restore;}
    bool done=false;
    g_subprocess_wait_async(child,NULL,child_finished,&done);
    gint64 shutdown=0;
    int stage=0;
    while (!done) {
        while (g_main_context_pending(NULL)) g_main_context_iteration(NULL,FALSE);
        otg_ble_link_poll(link);
        if (*stop && !stage) {
            g_subprocess_send_signal(child,SIGINT);stage=1;
            shutdown=g_get_monotonic_time()+5000000;
        } else if (stage && !done && g_get_monotonic_time()>=shutdown) {
            if (stage==1) g_subprocess_send_signal(child,SIGTERM);
            else g_subprocess_force_exit(child);
            stage++;shutdown=g_get_monotonic_time()+3000000;
        }
        if (!done) g_usleep(10000);
    }
    result=g_subprocess_get_if_exited(child) ? g_subprocess_get_exit_status(child) : 1;
restore:
    otg_ble_link_close(link);link=NULL;
    if(ipc[0]>=0){close(ipc[0]);ipc[0]=-1;}
    if(ipc[1]>=0){close(ipc[1]);ipc[1]=-1;}
    if (saved) {
        /* Cleanup must not be interrupted by repeated Ctrl+C. */
        signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_IGN);
        /* EOF from an abnormal child exit must be consumed and input/advertising
         * disabled by the resident service before changing controller modes. */
        bool idle=!resident_session || otg_ble_service_wait_idle((uid_t)uid,adapter);
        if(!idle) {
            fprintf(stderr,"ERROR: resident BLE idle state not verified; not changing modes; recovery incomplete, snapshot retained: %s\n",state);
            result=1;goto out;
        }
        bool ok=apply_settings(&c,original);
        if (!restore_alias(bus,object_path,original_alias)) ok=false;
        if (ok) {
            if(unlink(state)!=0) {
                fprintf(stderr,"ERROR: settings restored but recovery snapshot could not be removed: %s\n",state);result=1;goto out;
            }
            fprintf(stderr,"INFO: original Bluetooth settings verified and restored; pairing records retained\n");
            if (restore_only) result=0;
        } else {fprintf(stderr,"ERROR: recovery incomplete; snapshot retained: %s\n",state);result=1;}
    }
out:
    otg_ble_link_close(link);
    if(ipc[0]>=0)close(ipc[0]);
    if(ipc[1]>=0)close(ipc[1]);
    if (child) g_object_unref(child);
    if (bus) g_object_unref(bus);
    if (c.fd>=0) close(c.fd);
    if (lock>=0) close(lock);
    g_free(username);g_free(state);g_free(lock_path);g_free(object_path);
    g_free(c.alias);g_free(original_alias);
    return result;
}
#endif
