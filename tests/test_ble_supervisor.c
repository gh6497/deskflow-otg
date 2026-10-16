/* No hardware/root: exercise native MGMT transition planning and recovery.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/ble_supervisor.c"

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

int main(void)
{
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
