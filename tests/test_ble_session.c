/* Real Unix seqpacket IPC, fake HID backend; no root or system Bluetooth.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/ble_session.c"
#include <sys/wait.h>
static volatile sig_atomic_t stop_service;
static void stop_signal(int signal_number){(void)signal_number;stop_service=1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)

struct otg_ble_hid {
    bool application,advertising,active,connected,subscribed[2];
    uint8_t report[2][8];
    uint64_t interval;
};
static int created,destroyed,activated,deactivated;
static bool fail_idle;
static bool fake_register(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{
    struct otg_ble_hid *b=ctx;
    CHECK((id==1 || id==2) && size==8 && data[0]==5);
    if(id==2)b->application=true;
    return true;
}
static bool fake_ready(void *ctx)
{
    struct otg_ble_hid *b=ctx;
    return b->active && b->connected && b->subscribed[0] && b->subscribed[1];
}
static bool fake_send(void *ctx,uint16_t id,const uint8_t *data,uint16_t size)
{
    CHECK(size==8 && (id==1 || id==2));
    if(!fake_ready(ctx))return false;
    memcpy(((struct otg_ble_hid *)ctx)->report[id-1],data,size);return true;
}
static bool fake_poll(void *ctx){return ((struct otg_ble_hid *)ctx)->application;}
static const struct otg_hid_transport_ops FAKE_OPS={fake_register,fake_send,fake_poll,fake_ready,NULL};
const struct otg_hid_transport_ops *otg_ble_hid_ops(void){return &FAKE_OPS;}
struct otg_ble_hid *otg_ble_hid_create_resident(const char *adapter)
{
    CHECK(!strcmp(adapter,"hci0"));created++;
    return g_new0(struct otg_ble_hid,1);
}
void otg_ble_hid_destroy(struct otg_ble_hid *b){destroyed++;g_free(b);}
bool otg_ble_hid_set_session(struct otg_ble_hid *b,bool enabled)
{
    CHECK(b->application);
    if(enabled)activated++;
    else {deactivated++;memset(b->report,0,sizeof(b->report));}
    b->active=b->advertising=enabled;
    return enabled || !fail_idle;
}
void otg_ble_hid_set_interval(struct otg_ble_hid *b,uint64_t us){b->interval=us;}
bool otg_ble_hid_set_peer(struct otg_ble_hid *b,const char *address)
{(void)b;return strlen(address)==17;}

static void pair_socket(int *fds)
{CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fds));}
static struct message descriptor(unsigned id)
{
    struct message m={.operation=REGISTER,.id=id,.size=8,.data={5,1,9,6,0xa1,1,0xc0,0}};
    return m;
}
static void test_packets(void)
{
    int fds[2];pair_socket(fds);struct message m;
    CHECK(!receive_message(fds[0],&m));
    CHECK(send_message(fds[1],REGISTER,1,"12345678",8));
    CHECK(receive_message(fds[0],&m)==1 && m.id==1 && m.size==8);
    CHECK(!memcmp(m.data,"12345678",8));
    CHECK(peer_uid(fds[0],getuid()));CHECK(!peer_uid(fds[0],getuid()+1));
    m.version++;
    CHECK(send(fds[1],&m,HEADER_SIZE+8,0)==HEADER_SIZE+8);
    CHECK(receive_message(fds[0],&m)==-1);
    uint8_t oversized[sizeof(m)+1]={0};
    CHECK(send(fds[1],oversized,sizeof(oversized),0)==sizeof(oversized));
    CHECK(receive_message(fds[0],&m)==-1);
    CHECK(send_message(fds[1],STATUS,0,NULL,0));
    CHECK(receive_message(fds[0],&m)==1);
    m.size=1;CHECK(send(fds[1],&m,HEADER_SIZE,0)==HEADER_SIZE);
    CHECK(receive_message(fds[0],&m)==-1);
    close(fds[1]);CHECK(receive_message(fds[0],&m)==-1);close(fds[0]);
    char dir[128];struct sockaddr_un address;
    CHECK(socket_path(getuid(),"hci0",dir,sizeof(dir),&address));
    CHECK(!socket_path(getuid(),"hci0/../../x",dir,sizeof(dir),&address));
    CHECK(!valid_adapter("hci65535") && !valid_adapter("hci-1"));
}

static void test_sessions(void)
{
    int fds[2];pair_socket(fds);
    struct service s={.client=fds[0],.clean=true,.adapter="hci0"};
    struct message keys=descriptor(1),pointer=descriptor(2),reply;
    struct message peer={.operation=SELECT_PEER,.size=17,.data="aa:bb:cc:dd:ee:ff"};
    CHECK(handle_session(&s,&peer));
    CHECK(!strcmp(s.peer,"AA:BB:CC:DD:EE:FF"));
    peer.data[0]='Z';CHECK(!handle_session(&s,&peer));peer.data[0]='a';
    CHECK(handle_session(&s,&keys));CHECK(!s.hid && !s.active);
    CHECK(handle_session(&s,&pointer));
    CHECK(created==1 && activated==1 && s.hid->application && s.hid->advertising);
    s.hid->connected=true;
    CHECK(reply_status(&s,fds[0],true));
    CHECK(receive_message(fds[1],&reply)==1 && !reply.data[0]);
    s.hid->subscribed[0]=true;
    CHECK(!fake_ready(s.hid)); /* One CCC is not enough. */
    s.hid->subscribed[1]=true;CHECK(fake_ready(s.hid));
    struct message report={.operation=REPORT,.id=1,.size=8,.data={2,0,4}};
    CHECK(handle_session(&s,&report) && s.hid->report[0][2]==4);
    report.id=2;report.data[0]=3;report.data[1]=1;
    CHECK(handle_session(&s,&report) && s.hid->report[1][1]==1);
    struct message interval={.operation=POLL_SESSION,.size=8};uint64_t us=7500;
    memcpy(interval.data,&us,8);CHECK(handle_session(&s,&interval));CHECK(s.hid->interval==7500);

    /* SIGKILL/EOF equivalent: the socket vanishes with held input. The service
     * must release it and stop advertising without destroying GATT or CCCs. */
    close(fds[1]);CHECK(receive_message(fds[0],&reply)==-1);drop_client(&s);
    CHECK(!s.active && s.clean && !s.hid->advertising);
    CHECK(!s.hid->report[0][2] && !s.hid->report[1][1]);
    CHECK(s.hid->application && s.hid->subscribed[0] && s.hid->subscribed[1]);
    CHECK(!fake_ready(s.hid) && destroyed==0);

    pair_socket(fds);
    struct pending_peer pending={fds[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(fds[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(pending.fd==-1 && s.client==fds[0]);
    CHECK(receive_message(fds[1],&reply)==1 && !reply.id);
    CHECK(handle_session(&s,&peer));
    peer.data[0]='1';CHECK(!handle_session(&s,&peer));peer.data[0]='a';
    CHECK(handle_session(&s,&keys) && handle_session(&s,&pointer));
    CHECK(created==1 && activated==2 && fake_ready(s.hid));
    CHECK(!s.hid->report[0][2] && !s.hid->report[1][1]);
    s.hid->connected=false;CHECK(!fake_ready(s.hid));
    report.id=1;report.data[2]=7;
    CHECK(handle_session(&s,&report) && !s.hid->report[0][2]);
    s.hid->connected=true;CHECK(fake_ready(s.hid));
    s.hid->subscribed[1]=false;CHECK(!fake_ready(s.hid));

    int other[2];pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(other[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==1 && reply.id==1);close(other[1]);
    pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,true};
    CHECK(send_message(other[1],STOP_SERVICE,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==1 && reply.id==1 && !s.shutdown);close(other[1]);
    pair_socket(other);
    pending=(struct pending_peer){other[0],g_get_monotonic_time()+1000000,false};
    CHECK(send_message(other[1],ATTACH,0,NULL,0));handle_pending(&s,&pending);
    CHECK(receive_message(other[1],&reply)==-1);close(other[1]);

    CHECK(idle_service(&s));close(s.client);s.client=-1;close(fds[1]);
    keys.data[0]=6;CHECK(!handle_session(&s,&keys));keys.data[0]=5;
    CHECK(created==1 && destroyed==0); /* Schema changes never replace GATT. */
    pair_socket(fds);s.client=fds[0];s.last_request=100;
    s.active=s.hid->active=s.hid->advertising=true;s.hid->report[0][2]=4;
    expire_client(&s,5000099);CHECK(s.client==fds[0]);
    expire_client(&s,5000100);CHECK(s.client==-1 && s.clean && !s.active);
    CHECK(!s.hid->report[0][2] && s.hid->application && !s.hid->advertising);
    close(fds[1]);
    fail_idle=true;CHECK(!idle_service(&s) && !s.clean);
    fail_idle=false;CHECK(idle_service(&s) && s.clean);
    CHECK(deactivated>=3);
    otg_ble_hid_destroy(s.hid);
}

static void test_client_rpc(bool abrupt)
{
    int fds[2],ipc[2];pair_socket(fds);
    CHECK(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,ipc));
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        close(fds[1]);close(ipc[0]);close(ipc[1]);
        struct service s={.client=fds[0],.clean=true,.adapter="hci0"};
        bool received_report=false;
        for(;;) {
            struct message m;int n=receive_message(s.client,&m);
            if(!n){g_usleep(1000);continue;}
            if(n<0){drop_client(&s);break;}
            if(m.operation==DETACH) {
                CHECK(idle_service(&s));CHECK(reply_status(&s,s.client,true));
                close(s.client);s.client=-1;break;
            }
            CHECK(handle_session(&s,&m));
            if(s.active) {
                /* Simulate genuine BlueZ callbacks, not client assertions. */
                s.hid->connected=s.hid->subscribed[0]=s.hid->subscribed[1]=true;
            }
            if(m.operation==REPORT)received_report=true;
            CHECK(reply_status(&s,s.client,true));
        }
        CHECK(received_report && s.clean && !s.active && s.hid->application);
        CHECK(!s.hid->advertising && !s.hid->report[0][2]);
        CHECK(s.hid->subscribed[0] && s.hid->subscribed[1]);
        otg_ble_hid_destroy(s.hid);_exit(0);
    }
    close(fds[0]);
    struct session_owner *owner=g_new0(struct session_owner,1);
    struct otg_ble_session *s=&owner->session;
    s->fd=fds[1];s->alive=true;s->latency_fd=ipc[0];g_strlcpy(owner->adapter,"hci0",sizeof(owner->adapter));
    struct message keys=descriptor(1),pointer=descriptor(2);
    CHECK(session_register(s,1,keys.data,keys.size));
    CHECK(session_register(s,2,pointer.data,pointer.size));
    CHECK(session_poll(s) && session_ready(s));
    char text[64]={0};CHECK(recv(ipc[1],text,sizeof(text),0)==11);
    CHECK(!strcmp(text,"ready:hci0\n"));
    CHECK(send(ipc[1],"interval_us=7500\n",17,0)==17);
    CHECK(session_poll(s));
    uint8_t report[8]={0,0,4};CHECK(session_send(s,1,report,8));
    if(abrupt) {
        close(s->fd);s->fd=-1; /* No DETACH; service must react to EOF. */
    }
    otg_ble_session_destroy(s);close(ipc[1]);
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

static void test_service_process(void)
{
    /* Exercise the actual accept loop/lock/client API in an isolated runtime
     * tree. The backend is still fake; never touch /run/user or system BlueZ. */
    if(!geteuid())return; /* Production explicitly refuses a root daemon. */
    char *tmp=g_dir_make_tmp("otg-ble-session-XXXXXX",NULL);CHECK(tmp);
    runtime_base=tmp;
    char *user_dir=g_strdup_printf("%s/%lu",tmp,(unsigned long)getuid());
    CHECK(!mkdir(user_dir,0700));
    int initial_created=created,initial_activated=activated;
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        signal(SIGTERM,stop_signal);alarm(10);
        CHECK(otg_ble_service_run("hci0",&stop_service)==0);
        CHECK(created==initial_created+1 && activated==initial_activated+2);
        _exit(0);
    }
    bool listening=false;
    for(int i=0;i<200;i++) {
        if(otg_ble_service_wait_idle(getuid(),"hci0")){listening=true;break;}
        g_usleep(10000);
    }
    CHECK(listening);
    for(int session=0;session<2;session++) {
        struct otg_ble_session *client=otg_ble_session_open("hci0");CHECK(client);
        struct message keys=descriptor(1),pointer=descriptor(2);
        CHECK(session_register(client,1,keys.data,keys.size));
        CHECK(session_register(client,2,pointer.data,pointer.size));
        CHECK(session_poll(client) && !session_ready(client));
        CHECK(!otg_ble_session_open("hci0")); /* Exclusive owner. */
        otg_ble_session_destroy(client);
        CHECK(otg_ble_service_wait_idle(getuid(),"hci0"));
    }
    CHECK(otg_ble_service_status("hci0")==0);
    CHECK(otg_ble_service_stop("hci0")==0);
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    char dir[128];struct sockaddr_un address;
    CHECK(socket_path(getuid(),"hci0",dir,sizeof(dir),&address));
    CHECK(access(address.sun_path,F_OK)<0);
    char *lock=g_strdup_printf("%s/ble-hci0.lock",dir);
    CHECK(!unlink(lock));CHECK(!rmdir(dir));CHECK(!rmdir(user_dir));CHECK(!rmdir(tmp));
    g_free(lock);g_free(user_dir);runtime_base="/run/user";g_free(tmp);
}

int main(void)
{
    test_packets();test_sessions();test_client_rpc(false);test_client_rpc(true);test_service_process();
    puts("PASS: resident GATT sessions, EOF release, retained CCCs, schema/busy/UID guards and real seqpacket framing");
    return 0;
}
