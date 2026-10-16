/* Native counterpart of the experimental ble_latency.py. HCI events only;
 * never receives/logs ACL input or keys. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ble_link.h"
#ifdef __linux__
#include <glib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct link_state {bool used,encrypted,attempted;uint8_t role;uint16_t interval,latency;};
struct otg_ble_link {
    int hci,channel;uint16_t index,pending;
    bool ready,waiting;gint64 deadline;
    struct link_state links[4096];
};
static uint16_t get16(const uint8_t *p){return (uint16_t)(p[0]|p[1]<<8);}
static void set16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static unsigned single_link(struct otg_ble_link *b,uint16_t *handle)
{
    unsigned count=0;
    for(unsigned i=0;i<G_N_ELEMENTS(b->links);i++)if(b->links[i].used){count++;*handle=(uint16_t)i;}
    return count;
}
static void publish(struct otg_ble_link *b,uint16_t handle)
{
    struct link_state *link=&b->links[handle];char text[64];
    int size=snprintf(text,sizeof(text),"interval_us=%u\n",link->interval*1250u);
    (void)send(b->channel,text,(size_t)size,MSG_DONTWAIT|MSG_NOSIGNAL);
    fprintf(stderr,"INFO: BLE actual interval=%.2fms latency=%u (controller event)\n",link->interval*1.25,link->latency);
}
static void link_event(struct otg_ble_link *b,const uint8_t *packet,size_t size)
{
    if(size<3 || packet[0]!=4 || packet[2]!=size-3)return;
    uint8_t event=packet[1];const uint8_t *p=packet+3;size-=3;
    if(event==5 && size==4 && !p[0]) {
        uint16_t handle=get16(p+1);
        if(handle>=4096 || !b->links[handle].used)return;
        memset(&b->links[handle],0,sizeof(b->links[handle]));b->ready=false;
        if(b->waiting && b->pending==handle)b->waiting=false;
    } else if((event==8 || event==0x59) && size>=4) {
        uint16_t handle=get16(p+1);
        if(handle<4096)b->links[handle].encrypted=!p[0] && p[3];
    } else if(event==15 && size==4 && get16(p+2)==0x2013 && p[0] && b->waiting) {
        fprintf(stderr,"WARN: BLE interval request rejected: status=0x%02x\n",p[0]);b->waiting=false;
    } else if(event==0x3e && size) {
        if((p[0]==1 && size==19) || (p[0]==10 && size==31)) {
            uint16_t handle=get16(p+2);if(p[1] || handle>=4096)return;
            unsigned offset=p[0]==1 ? 12 : 24;
            b->links[handle]=(struct link_state){.used=true,.role=p[4],.interval=get16(p+offset),.latency=get16(p+offset+2)};
            b->ready=false;
        } else if(p[0]==3 && size==10) {
            uint16_t handle=get16(p+2);if(handle>=4096 || !b->links[handle].used)return;
            if(p[1])fprintf(stderr,"WARN: BLE interval update failed: status=0x%02x\n",p[1]);
            else {
                b->links[handle].interval=get16(p+4);b->links[handle].latency=get16(p+6);
                uint16_t only=0;if(b->ready && single_link(b,&only)==1 && only==handle)publish(b,handle);
            }
            if(b->waiting && b->pending==handle)b->waiting=false;
        }
    }
}
static void negotiate(struct otg_ble_link *b,gint64 now)
{
    if(b->waiting && now>=b->deadline){fprintf(stderr,"WARN: BLE interval update not confirmed within 5s\n");b->waiting=false;}
    uint16_t handle=0;
    if(!b->ready || b->waiting || single_link(b,&handle)!=1)return;
    struct link_state *link=&b->links[handle];
    if(link->attempted || !link->encrypted || link->role!=1)return;
    link->attempted=true;publish(b,handle);
    if(link->interval>=6 && link->interval<=12 && !link->latency)return;
    uint8_t cmd[18]={1,0x13,0x20,14};set16(cmd+4,handle);set16(cmd+6,6);set16(cmd+8,12);set16(cmd+12,500);
    if(send(b->hci,cmd,sizeof(cmd),MSG_DONTWAIT)!=(ssize_t)sizeof(cmd)) {
        fprintf(stderr,"WARN: cannot send BLE interval request\n");return;
    }
    b->waiting=true;b->pending=handle;b->deadline=now+5000000;
    fprintf(stderr,"INFO: BLE requesting interval 7.5–15ms; waiting for controller confirmation\n");
}
struct otg_ble_link *otg_ble_link_open(uint16_t index,int channel)
{
    struct otg_ble_link *b=g_new0(struct otg_ble_link,1);b->channel=channel;b->index=index;
    b->hci=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,1);
    struct {sa_family_t family;uint16_t index,channel;} address={AF_BLUETOOTH,index,0};
    /* Match the native HCI_FILTER ABI, including its trailing padding. New
     * kernels reject the former hard-coded 14-byte (unpadded) length. */
    struct {uint32_t type_mask,event_mask[2];uint16_t opcode;} filter={
        .type_mask=1u<<4,
        .event_mask={(1u<<5)|(1u<<8)|(1u<<15)|(1u<<25),1u<<30},
        .opcode=0
    };
    const char *operation="socket";
    if(b->hci<0)goto fail;
    operation="bind";
    if(bind(b->hci,(struct sockaddr *)&address,sizeof(address)))goto fail;
    operation="HCI_FILTER";
    if(setsockopt(b->hci,0,2,&filter,sizeof(filter)))goto fail;
    return b;
fail:
    {
        int saved_errno=errno;
        fprintf(stderr,"WARN: BLE timing monitor %s failed on hci%u: %s (errno=%d)\n",
                operation,index,strerror(saved_errno),saved_errno);
        if(b->hci>=0)close(b->hci);
        g_free(b);return NULL;
    }
}
void otg_ble_link_poll(struct otg_ble_link *b)
{
    if(!b)return;
    uint8_t packet[260];ssize_t size;
    while((size=recv(b->hci,packet,sizeof(packet),MSG_DONTWAIT))>0)link_event(b,packet,(size_t)size);
    char ready[48],not_ready[48];
    snprintf(ready,sizeof(ready),"ready:hci%u\n",b->index);
    snprintf(not_ready,sizeof(not_ready),"not-ready:hci%u\n",b->index);
    while((size=recv(b->channel,packet,sizeof(packet)-1,MSG_DONTWAIT))>0) {
        packet[size]=0;
        if(!strcmp((char *)packet,ready))b->ready=true;
        else if(!strcmp((char *)packet,not_ready))b->ready=false;
    }
    negotiate(b,g_get_monotonic_time());
}
void otg_ble_link_close(struct otg_ble_link *b)
{
    if(!b)return;
    close(b->hci);g_free(b);
}
#endif
