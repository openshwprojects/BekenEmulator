#include "esp/esp_acceptance.h"
#include <stdio.h>

int esp_acceptance_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP acceptance line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspAcceptance monitor("boot ready","app ready",true,false);
    std::string output="boot rea";
    monitor.observe(output,10,1,0);
    CHECK(!monitor.boot_seen() && !monitor.app_seen());
    output+="dy\napp re";monitor.observe(output,20,2,0);
    CHECK(monitor.boot_seen() && !monitor.app_seen());
    output+="ady\n";monitor.observe(output,30,3,0);
    CHECK(monitor.app_seen() && !monitor.complete(100029,1000003));
    CHECK(!monitor.complete(100030,1000002));
    CHECK(monitor.complete(100030,1000003));
    CHECK(!monitor.complete(29,2)); /* no unsigned-underflow acceptance */
    monitor.observe(output,200000,2000000,1);
    CHECK(!monitor.boot_seen() && !monitor.app_seen());
    CHECK(!monitor.complete(300000,3000000));
    output+="app ready\n";monitor.observe(output,300000,3000000,1);
    CHECK(!monitor.boot_seen() && monitor.app_seen());
    CHECK(!monitor.complete(400000,4000000));
    output+="boot ready\n";monitor.observe(output,400000,4000000,1);
    CHECK(monitor.complete(400000,4000000));

    EspAcceptance protocol("boot","AT+VER=",false,true);
    output="AT+VER=1\n";protocol.observe(output,20,10,0);
    CHECK(protocol.boot_seen() && !protocol.app_seen());
    CHECK(!protocol.complete(1000020,1000010));
    protocol.query_sent(output.size());
    protocol.observe(output,30,20,0);
    CHECK(!protocol.app_seen()); /* a stale banner cannot answer the query */
    output+="AT+V";protocol.observe(output,40,30,0);
    CHECK(!protocol.app_seen());
    output+="ER=1\n";protocol.observe(output,50,40,0);
    CHECK(protocol.app_seen() && protocol.complete(100050,1000040));
    protocol.observe(output,200000,2000000,1);
    output+="AT+VER=1\n";protocol.observe(output,200010,2000010,1);
    CHECK(!protocol.app_seen()); /* reset discards the outstanding query */
    protocol.query_sent(output.size());
    output+="AT+VER=1\n";protocol.observe(output,200020,2000020,1);
    CHECK(protocol.app_seen() && protocol.complete(300020,3000020));

    /* A firmware that only prints the marker in response to a command also
     * works: the first reply discovers it, the second proves live handling. */
    EspAcceptance replies("boot","reply",false,true);
    output.clear();replies.query_sent(0);
    output="reply";replies.observe(output,10,10,0);
    CHECK(!replies.app_seen());
    replies.query_sent(output.size());output+="reply";
    replies.observe(output,20,20,0);
    CHECK(replies.app_seen());
    output.clear();replies.observe(output,30,30,0);
    CHECK(!replies.app_seen() && !replies.complete(1000020,1000020));

    /* Board acceptance must ignore initialization transitions and send one
     * short press, then require an enabled relay response after release. */
    EspButtonAcceptance button;
    CHECK(button.observe(false,true,true,false,0,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,false,true,false,10,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,true,true,false,20,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,true,true,true,100020,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,true,true,true,300019,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,true,true,true,300020,0)==EspButtonAcceptance::PRESS);
    CHECK(button.observe(true,false,true,true,300021,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,false,true,false,500019,0)==EspButtonAcceptance::NONE);
    CHECK(!button.complete());
    CHECK(button.observe(true,false,true,false,500020,0)==EspButtonAcceptance::RELEASE);
    CHECK(!button.complete());
    CHECK(button.observe(true,true,true,false,500021,0)==EspButtonAcceptance::NONE);
    CHECK(!button.complete() && !button.failed());
    CHECK(button.observe(true,true,true,false,900000,0)==EspButtonAcceptance::NONE);
    CHECK(button.observe(true,true,true,false,1500019,0)==EspButtonAcceptance::NONE);
    CHECK(!button.complete());
    CHECK(button.observe(true,true,true,false,1500020,0)==EspButtonAcceptance::NONE);
    CHECK(button.complete() && !button.failed());
    button.observe(false,true,true,false,1500021,1);
    CHECK(!button.complete());

    EspButtonAcceptance timeout;
    timeout.observe(true,true,true,false,0,0);
    CHECK(timeout.observe(true,true,true,false,200000,0)==EspButtonAcceptance::PRESS);
    CHECK(timeout.observe(true,false,true,false,400000,0)==EspButtonAcceptance::RELEASE);
    timeout.observe(true,true,true,false,1399999,0);
    CHECK(!timeout.complete() && !timeout.failed());
    timeout.observe(true,true,true,false,1400000,0);
    CHECK(!timeout.complete() && timeout.failed());

    EspButtonAcceptance reset;
    reset.observe(true,true,true,false,0,0);
    CHECK(reset.observe(true,true,true,false,200000,0)==EspButtonAcceptance::PRESS);
    CHECK(reset.observe(false,false,true,true,200010,1)==EspButtonAcceptance::RELEASE);
    CHECK(!reset.complete() && !reset.failed());
    reset.observe(true,true,true,true,300000,1);
    CHECK(reset.observe(true,true,true,true,500000,1)==EspButtonAcceptance::PRESS);
    CHECK(reset.observe(true,false,true,true,10,1)==EspButtonAcceptance::RELEASE);
    CHECK(!reset.complete()); /* clock rollback cannot become a long press */
#undef CHECK
    return checks;
}
