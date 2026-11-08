#include "esp32/esp_psram.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks;if(!(x)){printf("[FAIL] ESP PSRAM line %d: %s\n",__LINE__,#x);return false;} } while(0)

bool protocol_and_capacity()
{
    for(unsigned mb=2;mb<=8;mb*=2) {
        std::map<u32,u32> regs;Bytes memory(mb*0x100000,0),rx;std::string error;
        CHECK(esp_psram::command(regs,memory,0x9f,0,false,Bytes(),8,rx,&error));
        CHECK(rx.size()==8 && (u8)rx[0]==0x0d && (u8)rx[1]==0x5d);
        CHECK((u8)rx[2]==(mb==2?0:mb==4?0x20:0x40));
        CHECK(esp_psram::command(regs,memory,2,0x3fe,false,Bytes("abcd"),0,rx));
        CHECK(esp_psram::command(regs,memory,3,0x3fe,false,Bytes(),4,rx) && rx=="abcd");
        CHECK(esp_psram::command(regs,memory,2,0x3fe,false,Bytes(4,'\xff'),0,rx));
        CHECK(memory.substr(0x3fe,4)==Bytes(4,'\xff')); /* RAM writes can set bits without WEL or erase */
        CHECK(esp_psram::command(regs,memory,0x35,0,false,Bytes(),0,rx));
        CHECK(esp_psram::qpi_enabled(regs));
        CHECK(esp_psram::command(regs,memory,0xeb,0x3fe,true,Bytes(),4,rx) && rx==Bytes(4,'\xff'));
        CHECK(esp_psram::command(regs,memory,0x9f,0,false,Bytes(),3,rx) && rx==Bytes(3,'\xff')); /* width mismatch */
        CHECK(esp_psram::command(regs,memory,0xf5,0,true,Bytes(),0,rx));
        CHECK(!esp_psram::qpi_enabled(regs));
        CHECK(!esp_psram::command(regs,memory,0xff,0,false,Bytes(),0,rx,&error));
        CHECK(!error.empty());
    }
    return true;
}

bool wrap_and_reset()
{
    std::map<u32,u32> regs;Bytes memory(0x200000,0),rx;
    CHECK(esp_psram::command(regs,memory,0xc0,0,false,Bytes(),0,rx));
    CHECK(esp_psram::command(regs,memory,2,31,false,Bytes("abcd"),0,rx));
    CHECK(memory[31]=='a' && memory.substr(0,3)=="bcd");
    CHECK(esp_psram::command(regs,memory,0x35,0,false,Bytes(),0,rx));
    CHECK(esp_psram::command(regs,memory,0x99,0,true,Bytes(),0,rx));
    CHECK(esp_psram::qpi_enabled(regs)); /* reset requires preceding reset-enable */
    CHECK(esp_psram::command(regs,memory,0x66,0,true,Bytes(),0,rx));
    CHECK(esp_psram::command(regs,memory,0x99,0,true,Bytes(),0,rx));
    CHECK(!esp_psram::qpi_enabled(regs) && memory[31]=='a'); /* reset keeps RAM contents */
    CHECK(esp_psram::command(regs,memory,2,31,false,Bytes("1234"),0,rx));
    CHECK(memory.substr(31,4)=="1234"); /* reset restores linear burst */
    CHECK(esp_psram::command(regs,memory,2,0x1fffff,false,Bytes("xy"),0,rx));
    CHECK(memory[0x1fffff]=='x' && memory[0]=='y'); /* physical address lines wrap */
    CHECK(esp_psram::command(regs,memory,3,0x200000,false,Bytes(),1,rx) && rx=="y");
    return true;
}
}

int esp_psram_tests()
{
    checks=0;if(!protocol_and_capacity() || !wrap_and_reset())return -1;return checks;
}
#ifdef ESP_PSRAM_TEST_MAIN
int main(){int n=esp_psram_tests();printf("ESP PSRAM: %d checks\n",n);return n<0?1:0;}
#endif
