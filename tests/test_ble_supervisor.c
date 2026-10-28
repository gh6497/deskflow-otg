/* No hardware/root: exercise native MGMT transition planning and recovery.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../platform/linux/ble_supervisor.c"

bool otg_ble_service_wait_idle(uid_t uid,const char *adapter)
{(void)uid;(void)adapter;return true;}

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static uint32_t flags;
static int writes,power_failures,power_attempts;
static uint16_t rejected;

static bool fake_command(struct controller *c,uint16_t opcode,const void *data,
                          uint16_t size,uint8_t *reply,size_t *reply_size)
{
    (void)c;
    if(opcode==4) {
        CHECK(reply && reply_size && *reply_size>=280);
        memset(reply,0,280);
        uint32_t supported=RESTORE_MASK;
        for(int i=0;i<4;i++){reply[9+i]=(uint8_t)(supported>>(8*i));reply[13+i]=(uint8_t)(flags>>(8*i));}
        *reply_size=280;return true;
    }
    CHECK(size==(opcode==6 ? 3 : 1));
    const uint8_t mode=*(const uint8_t *)data;
    writes++;
    if(opcode==5 && mode){power_attempts++;if(power_failures-->0)return false;}
    if(opcode==rejected)return false;
    unsigned bit;
    switch(opcode){
    case 5:bit=0;break;case 6:bit=3;break;case 7:bit=1;break;case 8:bit=2;break;
    case 9:bit=4;break;case 0x0a:bit=5;break;case 0x0b:bit=6;break;
    case 0x0d:bit=9;break;case 0x2a:bit=7;break;case 0x2d:bit=11;break;
    default:CHECK(false);return false;
    }
    if(mode)flags|=BIT(bit);else flags&=~BIT(bit);
    if(opcode==0x2a && !mode)flags&=~BIT(6); /* BR/EDR clears SSP */
    return true;
}

/* Exercise the real wire parser, not fake_command: MGMT status errors must
 * not be silently discarded until the command deadline expires. */
static void test_mgmt_wire(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,fds)==0);
    struct controller c={.fd=fds[0],.index=0x1234};
    const uint8_t failed[]={2,0,0x34,0x12,3,0,5,0,3};
    CHECK(send(fds[1],failed,sizeof(failed),0)==sizeof(failed));
    /* A trailing success makes the old parser fail immediately, rather than
     * forcing this regression test to wait ten seconds for its timeout. */
    const uint8_t complete[]={1,0,0x34,0x12,7,0,5,0,0,1,2,3,4};
    CHECK(send(fds[1],complete,sizeof(complete),0)==sizeof(complete));
    uint8_t mode=1;
    CHECK(!mgmt_command(&c,5,&mode,1,NULL,NULL));
    uint8_t packet[32];
    CHECK(recv(fds[0],packet,sizeof(packet),0)==sizeof(complete));
    CHECK(recv(fds[1],packet,sizeof(packet),0)==7);
    CHECK(le16(packet)==5 && le16(packet+2)==c.index &&
          le16(packet+4)==1 && packet[6]==mode);

    const uint8_t wrong_index[]={2,0,0x35,0x12,3,0,5,0,3};
    const uint8_t wrong_opcode[]={2,0,0x34,0x12,3,0,6,0,3};
    const uint8_t accepted[]={2,0,0x34,0x12,3,0,5,0,0};
    const uint8_t malformed[]={2,0,0x34,0x12,4,0,5,0,3};
    CHECK(send(fds[1],wrong_index,sizeof(wrong_index),0)==sizeof(wrong_index));
    CHECK(send(fds[1],wrong_opcode,sizeof(wrong_opcode),0)==sizeof(wrong_opcode));
    CHECK(send(fds[1],malformed,sizeof(malformed),0)==sizeof(malformed));
    CHECK(send(fds[1],accepted,sizeof(accepted),0)==sizeof(accepted));
    CHECK(send(fds[1],complete,sizeof(complete),0)==sizeof(complete));
    uint8_t reply[4];size_t size=sizeof(reply);
    CHECK(mgmt_command(&c,5,&mode,1,reply,&size));
    CHECK(size==4 && !memcmp(reply,complete+9,4));
    CHECK(recv(fds[1],packet,sizeof(packet),0)==7);
    close(fds[0]);close(fds[1]);
}

int main(void)
{
    test_mgmt_wire();
    struct controller c={.command=fake_command};
    uint32_t original=BIT(0)|BIT(1)|BIT(3)|BIT(4)|BIT(6)|BIT(7)|BIT(9)|BIT(11);
    flags=original;
    CHECK(apply_settings(&c,original));CHECK(writes==0);
    uint32_t isolated=original & ~(BIT(6)|BIT(7));
    CHECK(apply_settings(&c,isolated));CHECK(flags==isolated);
    CHECK(apply_settings(&c,original));CHECK(flags==original);
    /* Initial BLE-only/off is valid and restored, rather than trapped behind
     * the prototype's 'originally dual mode' gate. */
    CHECK(apply_settings(&c,BIT(9)|BIT(4)));CHECK(flags==(BIT(9)|BIT(4)));
    CHECK(apply_settings(&c,original));CHECK(flags==original);
    CHECK(apply_settings(&c,isolated));
    power_attempts=0;power_failures=1;
    CHECK(apply_settings(&c,original));CHECK(power_attempts==2);
    CHECK(flags==original);
    CHECK(apply_settings(&c,isolated));rejected=0x0b;
    CHECK(!apply_settings(&c,original));CHECK(flags & BIT(0));
    CHECK(!(flags & BIT(6))); /* Failure cannot be reported as restored. */
    rejected=0;
    CHECK(apply_settings(&c,original));
    uint16_t index;
    CHECK(parse_index("hci0",&index) && index==0);
    CHECK(parse_index("hci12",&index) && index==12);
    CHECK(!parse_index("hci65535",&index));CHECK(!parse_index("hci-1",&index));
    CHECK(!parse_index("hci0/../../tmp",&index));
    puts("PASS: native BLE isolation, repeat start, power retries, failed restore and adapter validation");
    return 0;
}
