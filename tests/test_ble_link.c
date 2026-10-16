/* Anonymous HCI fixtures and real Unix sockets; no Bluetooth/root access.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/ble_link.c"
#include <stdlib.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
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
