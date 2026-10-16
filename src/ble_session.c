/* Native local sessions for the BlueZ GATT application. No root daemon, shell,
 * Python, network listener, bond edits or implicit background process.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ble_session.h"
#include "ble_hid.h"
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SESSION_MAGIC UINT32_C(0x44464253)
#define SESSION_VERSION 2
#define MAX_DESC 1024
#define MAX_REPORTS 64
#define STATUS_INTERVAL_US 5000
#define REQUEST_TIMEOUT_US 3000000
static const char *runtime_base="/run/user";
enum operation { ATTACH=1, REGISTER, REPORT, POLL_SESSION, DETACH, STATUS, REPLY, SELECT_PEER, STOP_SERVICE };
/* Local native ABI only. Version and datagram length are always checked. */
struct message {
    uint32_t magic;
    uint16_t version, operation, id, size;
    uint8_t data[MAX_DESC];
};
#define HEADER_SIZE offsetof(struct message,data)
struct service {
    struct otg_ble_hid *hid;
    int client;
    unsigned registered;
    bool active, clean, shutdown;
    gint64 last_request;
    uint8_t descriptors[2][MAX_DESC];
    uint16_t sizes[2];
    const char *adapter;
    char peer[18];
};
struct otg_ble_session {
    int fd, latency_fd;
    bool ready, notified, alive;
    gint64 last_supervisor_note;
    bool in_flight, have_pointer_state;
    uint8_t pointer_state[2];
    gint64 request_deadline, last_request;
    uint64_t interval_us;
    unsigned head, count;
    struct queued_report {
        uint16_t id;
        uint8_t data[8];
        bool motion;
    } reports[MAX_REPORTS];
};

static bool valid_adapter(const char *name)
{
    if(strncmp(name,"hci",3) || !g_ascii_isdigit(name[3]))return false;
    char *end;unsigned long index=strtoul(name+3,&end,10);
    return !*end && index<UINT16_MAX;
}

static bool socket_path(uid_t uid,const char *adapter,char *dir,size_t dir_size,
                          struct sockaddr_un *address)
{
    if(!valid_adapter(adapter))return false;
    snprintf(dir,dir_size,"%s/%lu/deskflow-otg",runtime_base,(unsigned long)uid);
    memset(address,0,sizeof(*address));address->sun_family=AF_UNIX;
    int n=snprintf(address->sun_path,sizeof(address->sun_path),"%s/ble-%s.sock",dir,adapter);
    return n>0 && (size_t)n<sizeof(address->sun_path);
}

static bool private_directory(const char *dir,uid_t uid,bool create)
{
    if(create && mkdir(dir,0700)<0 && errno!=EEXIST)return false;
    struct stat st;
    return !lstat(dir,&st) && S_ISDIR(st.st_mode) && st.st_uid==uid &&
           !(st.st_mode & 0077);
}

static bool peer_uid(int fd,uid_t uid)
{
    struct ucred cred;socklen_t size=sizeof(cred);
    return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&size) &&
           size==sizeof(cred) && cred.uid==uid;
}

static bool send_message(int fd,uint16_t operation,uint16_t id,
                           const void *data,uint16_t size)
{
    if(size>MAX_DESC)return false;
    struct message m={.magic=SESSION_MAGIC,.version=SESSION_VERSION,
        .operation=operation,.id=id,.size=size};
    if(size)memcpy(m.data,data,size);
    return send(fd,&m,HEADER_SIZE+size,MSG_NOSIGNAL|MSG_DONTWAIT)==(ssize_t)(HEADER_SIZE+size);
}

/* 1: valid packet, 0: retry, -1: EOF/malformed/fatal. MSG_TRUNC rejects an
 * oversized seqpacket instead of treating its truncated prefix as a report. */
static int receive_message(int fd,struct message *m)
{
    ssize_t n=recv(fd,m,sizeof(*m),MSG_DONTWAIT|MSG_TRUNC);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    if(n<(ssize_t)HEADER_SIZE || n>(ssize_t)sizeof(*m) ||
       m->magic!=SESSION_MAGIC || m->version!=SESSION_VERSION ||
       m->size>MAX_DESC || n!=(ssize_t)(HEADER_SIZE+m->size))return -1;
    return 1;
}

static bool idle_service(struct service *s)
{
    bool ok=!s->hid || otg_ble_hid_set_session(s->hid,false);
    s->active=false;s->registered=0;s->clean=ok;
    return ok;
}

static void drop_client(struct service *s)
{
    if(s->client>=0)close(s->client);
    s->client=-1;
    (void)idle_service(s);
}

static void expire_client(struct service *s,gint64 now)
{
    if(s->client>=0 && now-s->last_request>=5000000) {
        fprintf(stderr,"WARN: resident BLE session lease expired; releasing input\n");
        drop_client(s);
    }
}

static bool register_descriptor(struct service *s,const struct message *m)
{
    if(m->id!=AOA_HID_ID_KEYBOARD && m->id!=AOA_HID_ID_MOUSE)return false;
    if(m->size<6 || s->active)return false;
    unsigned index=m->id==AOA_HID_ID_MOUSE;
    if(s->hid) {
        if(s->sizes[index]!=m->size || memcmp(s->descriptors[index],m->data,m->size)) {
            fprintf(stderr,"ERROR: resident BLE report schema differs; stop the service explicitly to change it (CCC continuity will be lost)\n");
            return false;
        }
    } else {
        memcpy(s->descriptors[index],m->data,m->size);s->sizes[index]=m->size;
    }
    s->registered|=1u<<index;
    if(s->registered!=3)return true;
    if(!s->hid) {
        s->hid=otg_ble_hid_create_resident(s->adapter);
        if(!s->hid)return false;
        if(s->peer[0] && !otg_ble_hid_set_peer(s->hid,s->peer))return false;
        const struct otg_hid_transport_ops *ops=otg_ble_hid_ops();
        if(!ops->register_report(s->hid,AOA_HID_ID_KEYBOARD,s->descriptors[0],s->sizes[0]) ||
           !ops->register_report(s->hid,AOA_HID_ID_MOUSE,s->descriptors[1],s->sizes[1])) {
            otg_ble_hid_destroy(s->hid);s->hid=NULL;return false;
        }
    }
    s->active=otg_ble_hid_set_session(s->hid,true);
    return s->active;
}

static bool reply_status(struct service *s,int fd,bool ok)
{
    uint8_t status[2]={s->active && s->hid && otg_ble_hid_ops()->ready(s->hid),
                       s->client<0 && !s->active && s->clean};
    return send_message(fd,REPLY,ok?0:1,status,sizeof(status));
}

static bool handle_session(struct service *s,const struct message *m)
{
    switch(m->operation) {
    case SELECT_PEER:
        if(s->active || s->registered || m->id || m->size!=17)return false;
        for(unsigned i=0;i<17;i++) {
            if(i%3==2 ? m->data[i]!=':' : !g_ascii_isxdigit(m->data[i]))return false;
        }
        char peer[18];memcpy(peer,m->data,17);peer[17]=0;
        for(unsigned i=0;i<17;i++)peer[i]=g_ascii_toupper(peer[i]);
        if((s->peer[0] && strcmp(s->peer,peer)) ||
           (s->hid && !otg_ble_hid_set_peer(s->hid,peer)))return false;
        memcpy(s->peer,peer,sizeof(peer));
        return true;
    case REGISTER:return register_descriptor(s,m);
    case REPORT:
        if(!s->active || m->size!=8 ||
           (m->id!=AOA_HID_ID_KEYBOARD && m->id!=AOA_HID_ID_MOUSE))return false;
        /* The client can have observed readiness just before a disconnect.
         * Drop this stale report without caching it or killing the session;
         * the reply carries ready=false and Deskflow will release its screen. */
        if(!otg_ble_hid_ops()->ready(s->hid))return true;
        return otg_ble_hid_ops()->send(s->hid,m->id,m->data,m->size);
    case POLL_SESSION:
        if(m->size && m->size!=sizeof(uint64_t))return false;
        if(m->size && s->active && otg_ble_hid_ops()->ready(s->hid)) {
            uint64_t us;memcpy(&us,m->data,sizeof(us));
            if(us<7500 || us>4000000)return false;
            otg_ble_hid_set_interval(s->hid,us);
        }
        return true;
    default:return false;
    }
}

struct pending_peer {int fd;gint64 deadline;bool owner;};
static void handle_pending(struct service *s,struct pending_peer *p)
{
    struct message m;int result=receive_message(p->fd,&m);
    if(!result && g_get_monotonic_time()<p->deadline)return;
    if(result==1 && !m.size && !m.id) {
        if(m.operation==STATUS)(void)reply_status(s,p->fd,true);
        else if(m.operation==STOP_SERVICE && p->owner) {
            bool ok=s->client<0 && !s->active && s->clean;
            if(reply_status(s,p->fd,ok) && ok)s->shutdown=true;
        }
        else if(m.operation==ATTACH && p->owner) {
            bool ok=s->client<0 && s->clean && !s->shutdown;
            if(ok){s->client=p->fd;s->registered=0;s->last_request=g_get_monotonic_time();}
            bool sent=reply_status(s,p->fd,ok);
            if(ok && sent){p->fd=-1;return;}
            if(ok)s->client=-1;
        }
    }
    close(p->fd);p->fd=-1;
}

int otg_ble_service_run(const char *adapter,volatile sig_atomic_t *stop)
{
    if(geteuid()==0){fprintf(stderr,"ERROR: run --ble-service as an ordinary user, without sudo\n");return 2;}
    char dir[128],lock_name[160];struct sockaddr_un address;
    if(!socket_path(getuid(),adapter,dir,sizeof(dir),&address) ||
       !private_directory(dir,getuid(),true)) {
        fprintf(stderr,"ERROR: resident BLE requires a private /run/user/UID/deskflow-otg directory\n");return 1;
    }
    snprintf(lock_name,sizeof(lock_name),"%s/ble-%s.lock",dir,adapter);
    int lock=open(lock_name,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    struct stat st;
    if(lock<0 || fstat(lock,&st) || st.st_uid!=getuid() || !S_ISREG(st.st_mode) ||
       (st.st_mode & 0077) || flock(lock,LOCK_EX|LOCK_NB)) {
        if(lock>=0)close(lock);
        fprintf(stderr,"ERROR: another resident BLE service owns this adapter or lock is unsafe\n");return 1;
    }
    int listener=-1,result=1;bool bound=false;
    if(lstat(address.sun_path,&st)==0) {
        if(!S_ISSOCK(st.st_mode) || st.st_uid!=getuid())goto out;
        if(unlink(address.sun_path))goto out;
    } else if(errno!=ENOENT)goto out;
    listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(listener<0 || bind(listener,(struct sockaddr *)&address,sizeof(address)))goto out;
    bound=true;
    if(chmod(address.sun_path,0600) || listen(listener,4))goto out;
    struct service s={.client=-1,.clean=true,.adapter=adapter};
    struct pending_peer pending[4];
    for(size_t i=0;i<G_N_ELEMENTS(pending);i++)pending[i].fd=-1;
    fprintf(stderr,"INFO: resident BLE service listening on %s; idle, no advertisement; keep this process running between bridge sessions\n",address.sun_path);
    result=0;
    while(!*stop && !s.shutdown) {
        if(s.hid && !otg_ble_hid_ops()->poll(s.hid)){result=1;break;}
        if(s.client>=0) {
            struct message m;int n=receive_message(s.client,&m);
            if(n<0)drop_client(&s);
            else if(n>0) {
                s.last_request=g_get_monotonic_time();
                if(m.operation==DETACH && !m.size && !m.id) {
                    bool ok=idle_service(&s);
                    (void)reply_status(&s,s.client,ok);
                    close(s.client);s.client=-1;
                } else if(!reply_status(&s,s.client,handle_session(&s,&m)))drop_client(&s);
            }
            else expire_client(&s,g_get_monotonic_time());
        }
        for(size_t i=0;i<G_N_ELEMENTS(pending);i++) {
            if(pending[i].fd<0) {
                int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
                if(fd<0)continue;
                bool owner=peer_uid(fd,getuid());
                if(!owner && !peer_uid(fd,0)){close(fd);continue;}
                pending[i]=(struct pending_peer){fd,g_get_monotonic_time()+1000000,owner};
            }
            handle_pending(&s,&pending[i]);
        }
        /* IPC wakes us immediately, including the next request from a client
         * that just received a reply. A sleep here caps throughput at 1k RPC/s.
         * The bounded timeout also pumps BlueZ and the last coalesced pointer
         * report when no socket input arrives. Never busy-spin while idle. */
        struct pollfd fds[6]={{listener,POLLIN,0},{s.client,POLLIN,0}};
        for(size_t i=0;i<G_N_ELEMENTS(pending);i++)
            fds[i+2]=(struct pollfd){pending[i].fd,POLLIN,0};
        if(!*stop && !s.shutdown && poll(fds,G_N_ELEMENTS(fds),s.active?1:50)<0 && errno!=EINTR) {
            result=1;break;
        }
    }
    drop_client(&s);
    for(size_t i=0;i<G_N_ELEMENTS(pending);i++)if(pending[i].fd>=0)close(pending[i].fd);
    if(s.hid) {
        fprintf(stderr,"WARN: resident BLE service stopping; unregistering GATT loses CCC continuity\n");
        otg_ble_hid_destroy(s.hid);
    }
out:
    if(listener>=0)close(listener);
    if(bound)unlink(address.sun_path);
    close(lock);
    return result;
}

static int connect_service(uid_t uid,const char *adapter)
{
    char dir[128];struct sockaddr_un address;
    if(!socket_path(uid,adapter,dir,sizeof(dir),&address) ||
       !private_directory(dir,uid,false))return -1;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return -1;
    if(connect(fd,(struct sockaddr *)&address,sizeof(address)) || !peer_uid(fd,uid)) {
        close(fd);return -1;
    }
    return fd;
}

static bool valid_reply(const struct message *reply)
{return reply->operation==REPLY && reply->size==2 && reply->id==0;}

static bool wait_reply(int fd,struct message *reply,gint64 deadline)
{
    struct pollfd pfd={fd,POLLIN,0};
    while(g_get_monotonic_time()<deadline) {
        int n=poll(&pfd,1,50);
        if(n<0 && errno==EINTR)continue;
        if(n<0 || (pfd.revents & (POLLERR|POLLNVAL)))return false;
        if(!n)continue;
        if(receive_message(fd,reply)!=1)return false;
        return valid_reply(reply);
    }
    return false;
}

static bool request(int fd,uint16_t operation,uint16_t id,const void *data,
                       uint16_t size,struct message *reply)
{
    return send_message(fd,operation,id,data,size) &&
        wait_reply(fd,reply,g_get_monotonic_time()+REQUEST_TIMEOUT_US);
}

static void notify_supervisor(struct otg_ble_session *s,const char *adapter,bool ready)
{
    if(s->latency_fd>=0) {
        char message[64];int n=snprintf(message,sizeof(message),"%s:%s\n",ready?"ready":"not-ready",adapter);
        (void)send(s->latency_fd,message,(size_t)n,MSG_DONTWAIT|MSG_NOSIGNAL);
    }
}

/* Adapter is needed for the supervisor notification but never comes from IPC. */
struct session_owner {struct otg_ble_session session;char adapter[16];};
struct otg_ble_session *otg_ble_session_open(const char *adapter)
{
    if(!valid_adapter(adapter))return NULL;
    int fd=connect_service(getuid(),adapter);
    if(fd<0){fprintf(stderr,"ERROR: start deskflow-otg --ble-service --adapter %s as this ordinary user first\n",adapter);return NULL;}
    struct message reply;
    if(!request(fd,ATTACH,0,NULL,0,&reply)) {
        fprintf(stderr,"ERROR: resident BLE service busy, incompatible, or unavailable\n");close(fd);return NULL;
    }
    struct session_owner *owner=g_new0(struct session_owner,1);
    struct otg_ble_session *s=&owner->session;
    s->fd=fd;s->alive=true;s->latency_fd=-1;
    g_strlcpy(owner->adapter,adapter,sizeof(owner->adapter));
    const char *value=getenv("OTG_BLE_LATENCY_FD");
    if(value && *value) {
        char *end;long latency=strtol(value,&end,10);
        int type;socklen_t length=sizeof(type);
        if(!*end && latency>=3 && latency<=INT32_MAX &&
           !getsockopt((int)latency,SOL_SOCKET,SO_TYPE,&type,&length) && type==SOCK_DGRAM)
            s->latency_fd=(int)latency;
    }
    fprintf(stderr,"INFO: attached to resident BLE service; waiting for genuine phone subscriptions\n");
    return s;
}

static bool session_rpc(struct otg_ble_session *s,uint16_t op,uint16_t id,
                          const void *data,uint16_t size)
{
    struct message reply;
    if(!s->alive)return false;
    if(!request(s->fd,op,id,data,size,&reply)) {
        s->alive=false;s->ready=false;return false;
    }
    s->ready=reply.data[0]!=0;return true;
}
bool otg_ble_session_set_peer(struct otg_ble_session *s,const char *address)
{
    return address && strlen(address)==17 && session_rpc(s,SELECT_PEER,0,address,17);
}
static bool session_register(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{return session_rpc(ctx,REGISTER,id,data,size);}

static bool session_fail(struct otg_ble_session *s,const char *reason)
{
    fprintf(stderr,"ERROR: resident BLE %s; releasing session input\n",reason);
    s->alive=s->ready=s->in_flight=false;s->count=0;
    /* The service sees EOF and releases keys even if the caller has not yet
     * reached its normal close path. Never silently lose a state barrier. */
    if(s->fd>=0)(void)shutdown(s->fd,SHUT_RDWR);
    return false;
}

/* One outstanding request keeps IPC v2 reply ordering unambiguous and bounds
 * the kernel queue. Everything behind it stays here, where motion can still
 * be replaced. Frequent stop checks/ticks only drain a nonblocking reply;
 * they do not each initiate a synchronous status round trip. */
static bool session_pump(struct otg_ble_session *s,bool poll_status)
{
    if(!s->alive)return false;
    gint64 now=g_get_monotonic_time();
    if(s->in_flight) {
        struct message reply;int n=receive_message(s->fd,&reply);
        if(n<0 || (n>0 && !valid_reply(&reply)))return session_fail(s,"reply failed");
        if(!n) {
            if(now>=s->request_deadline)return session_fail(s,"reply timed out");
            return true;
        }
        s->in_flight=false;s->ready=reply.data[0]!=0;
        if(!s->ready) {
            s->count=0;s->have_pointer_state=false;s->interval_us=0;
        }
    }
    uint16_t op=POLL_SESSION,id=0,size=0;const void *data=NULL;
    bool report=s->ready && s->count && !s->interval_us;
    if(report) {
        struct queued_report *r=&s->reports[s->head];
        op=REPORT;id=r->id;size=sizeof(r->data);data=r->data;
    } else if(s->interval_us || (poll_status && now-s->last_request>=STATUS_INTERVAL_US)) {
        if(s->interval_us){data=&s->interval_us;size=sizeof(s->interval_us);}
    } else return true;
    if(!send_message(s->fd,op,id,data,size))return session_fail(s,"request failed");
    if(report){s->head=(s->head+1)%MAX_REPORTS;s->count--;}
    else s->interval_us=0;
    s->in_flight=true;s->last_request=now;s->request_deadline=now+REQUEST_TIMEOUT_US;
    return true;
}

static bool session_send(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{
    struct otg_ble_session *s=ctx;
    if(size!=8 || (id!=AOA_HID_ID_KEYBOARD && id!=AOA_HID_ID_MOUSE))return false;
    if(!session_pump(s,false))return false;
    if(!s->ready)return true; /* A reply just reported a phone disconnect. */
    bool pointer=id==AOA_HID_ID_MOUSE;
    bool motion=pointer && s->have_pointer_state &&
        !memcmp(s->pointer_state,data,2) && !data[6] && !data[7];
    struct queued_report *last=s->count ? &s->reports[(s->head+s->count-1)%MAX_REPORTS] : NULL;
    if(motion && last && last->motion && last->id==id && !memcmp(last->data,data,2)) {
        memcpy(last->data,data,size);
    } else {
        if(s->count==MAX_REPORTS)return session_fail(s,"input queue full");
        struct queued_report *r=&s->reports[(s->head+s->count++)%MAX_REPORTS];
        r->id=id;r->motion=motion;memcpy(r->data,data,size);
    }
    if(pointer){memcpy(s->pointer_state,data,2);s->have_pointer_state=true;}
    return session_pump(s,false);
}
static bool session_ready(void *ctx){return ((struct otg_ble_session *)ctx)->ready;}
static bool session_poll(void *ctx)
{
    struct otg_ble_session *s=ctx;
    if(s->latency_fd>=0) {
        char text[96];ssize_t size;
        while((size=recv(s->latency_fd,text,sizeof(text)-1,MSG_DONTWAIT))>0) {
            text[size]=0;
            if(!g_str_has_prefix(text,"interval_us="))continue;
            char *end;uint64_t candidate=g_ascii_strtoull(text+12,&end,10);
            if((*end=='\n' || !*end) && candidate>=7500 && candidate<=4000000)s->interval_us=candidate;
        }
    }
    bool ok=session_pump(s,true);
    gint64 now=g_get_monotonic_time();
    if(s->ready!=s->notified || now-s->last_supervisor_note>=100000) {
        struct session_owner *owner=(struct session_owner *)s;
        /* A rapid HCI reconnect may occur between two local IPC polls. Repeat
         * the current readiness periodically so the supervisor cannot remain
         * stuck at not-ready after observing that disconnect itself. */
        notify_supervisor(s,owner->adapter,s->ready);s->last_supervisor_note=now;
    }
    if(s->ready!=s->notified) {
        s->notified=s->ready;
        fprintf(stderr,"INFO: BLE resident input channel %s\n",s->ready?"ready":"disconnected");
    }
    return ok;
}
static void session_close(void *ctx)
{
    struct otg_ble_session *s=ctx;
    if(s->fd<0)return;
    struct session_owner *owner=(struct session_owner *)s;
    notify_supervisor(s,owner->adapter,false);
    struct message reply;
    /* Discard unsent input on exit; DETACH itself releases keys/range. Consume
     * the outstanding reply first so it cannot be mistaken for DETACH's ACK. */
    s->count=0;
    bool drained=!s->in_flight || wait_reply(s->fd,&reply,s->request_deadline);
    if(!s->alive || !drained || !request(s->fd,DETACH,0,NULL,0,&reply))
        fprintf(stderr,"WARN: resident BLE detach was not acknowledged\n");
    close(s->fd);s->fd=-1;s->alive=s->ready=false;
}
static const struct otg_hid_transport_ops SESSION_OPS={session_register,session_send,session_poll,session_ready,session_close};
const struct otg_hid_transport_ops *otg_ble_session_ops(void){return &SESSION_OPS;}
void otg_ble_session_destroy(struct otg_ble_session *s)
{if(s){session_close(s);if(s->latency_fd>=0)close(s->latency_fd);g_free(s);}}

bool otg_ble_service_wait_idle(uid_t uid,const char *adapter)
{
    gint64 deadline=g_get_monotonic_time()+5000000;
    do {
        int fd=connect_service(uid,adapter);
        if(fd<0)return false;
        struct message reply;bool ok=request(fd,STATUS,0,NULL,0,&reply);close(fd);
        if(!ok)return false;
        if(reply.data[1])return true;
        g_usleep(10000);
    }while(g_get_monotonic_time()<deadline);
    return false;
}

int otg_ble_service_status(const char *adapter)
{
    int fd=connect_service(getuid(),adapter);
    struct message reply;
    bool ok=fd>=0 && request(fd,STATUS,0,NULL,0,&reply);
    if(fd>=0)close(fd);
    if(!ok) {
        puts("{\"version\":2,\"available\":false}");
        return 1;
    }
    printf("{\"version\":2,\"available\":true,\"ready\":%s,\"idle\":%s}\n",
           reply.data[0]?"true":"false",reply.data[1]?"true":"false");
    return 0;
}

int otg_ble_service_stop(const char *adapter)
{
    int fd=connect_service(getuid(),adapter);
    struct message reply;
    bool ok=fd>=0 && request(fd,STOP_SERVICE,0,NULL,0,&reply);
    if(fd>=0)close(fd);
    if(!ok)fprintf(stderr,"ERROR: service unavailable, busy, incompatible or cleanup incomplete; no stop performed\n");
    return ok ? 0 : 1;
}
