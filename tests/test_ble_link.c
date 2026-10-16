/* Anonymous HCI fixtures and real Unix sockets; no Bluetooth/root access.
 * SPDX-License-Identifier: Apache-2.0 */
#include <sys/socket.h>
#include <unistd.h>
static int fake_socket(int domain,int type,int protocol);
static int fake_bind(int fd,const struct sockaddr *address,socklen_t length);
static int fake_setsockopt(int fd,int level,int option,const void *value,socklen_t length);
static int fake_close(int fd);
#define socket fake_socket
#define bind fake_bind
#define setsockopt fake_setsockopt
#define close fake_close
#include "../src/ble_link.c"
#undef socket
#undef bind
#undef setsockopt
#undef close
#include <stdlib.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static int fail_stage,closed,filter_calls;
static int fake_socket(int domain,int type,int protocol)
{
    CHECK(domain==AF_BLUETOOTH && protocol==1);
    CHECK(type==(SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK));
    if(fail_stage==1){errno=EPERM;return -1;}
    return 42;
}
static int fake_bind(int fd,const struct sockaddr *address,socklen_t length)
{
    struct {sa_family_t family;uint16_t index,channel;} expected={AF_BLUETOOTH,7,0};
    CHECK(fd==42 && length==sizeof(expected));
    CHECK(!memcmp(address,&expected,sizeof(expected)));
    if(fail_stage==2){errno=ENODEV;return -1;}
    return 0;
}
static int fake_setsockopt(int fd,int level,int option,const void *value,socklen_t length)
{
    struct {uint32_t type_mask,event_mask[2];uint16_t opcode;} filter;
    CHECK(fd==42 && level==0 && option==2);
    CHECK(length==sizeof(filter)); /* Includes native ABI padding, not 14. */
    memcpy(&filter,value,sizeof(filter));filter_calls++;
    CHECK(filter.type_mask==(1u<<4) && filter.opcode==0);
    CHECK(filter.event_mask[0]==((1u<<5)|(1u<<8)|(1u<<15)|(1u<<25)));
    CHECK(filter.event_mask[1]==(1u<<30));
    if(fail_stage==3){errno=EINVAL;return -1;}
    return 0;
}
static int fake_close(int fd){CHECK(fd==42);closed++;return 0;}

static void test_monitor_open(void)
{
    struct otg_ble_link *b=otg_ble_link_open(7,99);
    CHECK(b && b->index==7 && b->channel==99 && filter_calls==1);
    otg_ble_link_close(b);CHECK(closed==1);
    for(fail_stage=1;fail_stage<=3;fail_stage++) {
        int before=closed;
        CHECK(!otg_ble_link_open(7,99));
        CHECK(closed==before+(fail_stage!=1));
    }
    fail_stage=0;
}
static void connect_event(struct otg_ble_link *b,uint16_t handle)
{
    uint8_t event[34]={4,0x3e,31,10,0};set16(event+5,handle);
    event[7]=1;set16(event+27,36);set16(event+31,500);
    link_event(b,event,sizeof(event));
    uint8_t encrypted[]={4,8,4,0,0,0,1};set16(encrypted+4,handle);
    link_event(b,encrypted,sizeof(encrypted));
}
int main(void)
{
    test_monitor_open();
    int hci[2],ipc[2];
    CHECK(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,hci)==0);
    CHECK(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,ipc)==0);
    struct otg_ble_link *b=g_new0(struct otg_ble_link,1);b->hci=hci[0];b->channel=ipc[0];
    connect_event(b,2048);
    negotiate(b,0);CHECK(!b->waiting);
    b->ready=true;negotiate(b,0);CHECK(b->waiting);
    uint8_t packet[64];ssize_t n=recv(hci[1],packet,sizeof(packet),0);
    CHECK(n==18 && get16(packet+1)==0x2013 && get16(packet+4)==2048);
    CHECK(get16(packet+6)==6 && get16(packet+8)==12 && get16(packet+12)==500);
    char message[64]={0};CHECK(recv(ipc[1],message,sizeof(message),0)>0);
    CHECK(!strcmp(message,"interval_us=45000\n"));
    uint8_t accepted[]={4,15,4,0,1,0x13,0x20};
    link_event(b,accepted,sizeof(accepted));CHECK(b->waiting);
    CHECK(recv(ipc[1],message,sizeof(message),0)<0); /* No false success. */
    uint8_t updated[]={4,0x3e,10,3,0,0,8,6,0,0,0,0xf4,1};
    link_event(b,updated,sizeof(updated));CHECK(!b->waiting);
    memset(message,0,sizeof(message));CHECK(recv(ipc[1],message,sizeof(message),0)>0);
    CHECK(!strcmp(message,"interval_us=7500\n"));
    uint8_t disconnected[]={4,5,4,0,0,8,19};
    link_event(b,disconnected,sizeof(disconnected));CHECK(!b->ready && !b->links[2048].used);
    connect_event(b,2049);b->ready=true;negotiate(b,100);
    CHECK(recv(hci[1],packet,sizeof(packet),0)==18 && get16(packet+4)==2049);
    uint8_t rejected[]={4,15,4,12,1,0x13,0x20};
    link_event(b,rejected,sizeof(rejected));CHECK(!b->waiting);
    negotiate(b,200);CHECK(recv(hci[1],packet,sizeof(packet),0)<0);
    connect_event(b,2050);b->ready=true;negotiate(b,300);
    CHECK(recv(hci[1],packet,sizeof(packet),0)<0); /* Ambiguous peers untouched. */
    close(hci[0]);close(hci[1]);close(ipc[0]);close(ipc[1]);g_free(b);
    puts("PASS: native HCI request, actual confirmation, rejection, reconnection and peer ambiguity");
    return 0;
}
