#include "esp8266/esp_wifi_acceptance.h"
#include <stdio.h>

int esp_wifi_acceptance_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP Wi-Fi acceptance line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspWifiRxMonitor::Observation sample;
    sample.eof=0x3ffe8000;sample.flags=0x80080080;
    EspWifiRxMonitor idle;idle.observe(sample);
    CHECK(!idle.armed() && !idle.complete());
    EspWifiRxMonitor monitor;monitor.arm(sample);
    CHECK(monitor.armed() && !monitor.received());
    sample.flags=0xc0028080;sample.raw_events=1u<<8;
    monitor.observe(sample);
    CHECK(!monitor.received()); // stale EOF without a newly completed RX
    sample.received=1;sample.instructions=100;sample.microseconds=10;
    monitor.observe(sample);
    CHECK(monitor.received() && !monitor.recycled() && !monitor.failed());
    sample.raw_events=0;monitor.observe(sample);
    CHECK(!monitor.recycled()); // IRQ acknowledgement alone does not recycle
    sample.raw_events=1u<<8;sample.flags=0x80080080;monitor.observe(sample);
    CHECK(!monitor.recycled()); // descriptor recycling alone does not acknowledge
    sample.raw_events=0;sample.instructions=200;sample.microseconds=20;
    monitor.observe(sample);
    CHECK(monitor.recycled() && !monitor.complete() && !monitor.needs_descriptor());
    sample.instructions=100199;sample.microseconds=10020;monitor.observe(sample);
    CHECK(!monitor.complete());
    sample.instructions=100200;sample.microseconds=10019;monitor.observe(sample);
    CHECK(monitor.failed()); // time regression cannot be used as elapsed time

    EspWifiRxMonitor success;sample=EspWifiRxMonitor::Observation();
    sample.received=7;success.arm(sample);
    sample.received=8;sample.eof=0x3ffec2b0;sample.flags=0xdf028640;
    sample.raw_events=1u<<8;sample.instructions=500;sample.microseconds=100;
    success.observe(sample);
    CHECK(success.received() && !success.complete());
    sample.flags=0x9f640640;sample.raw_events=0;success.observe(sample);
    CHECK(success.recycled()); // ignored software bits and owner do not matter
    sample.instructions=100500;sample.microseconds=10099;success.observe(sample);
    CHECK(!success.complete());
    sample.microseconds=10100;success.observe(sample);
    CHECK(success.complete() && !success.failed());
    sample.resets=1;success.observe(sample);
    CHECK(!success.complete() && success.failed()); // reset invalidates an old pass

    EspWifiRxMonitor missing;sample=EspWifiRxMonitor::Observation();missing.arm(sample);
    sample.received=1;sample.eof=0x3ffe8000;sample.flags=0x80080080;
    missing.observe(sample);
    CHECK(missing.failed() && !missing.error().empty()); // publication was never observed

    EspWifiRxMonitor replaced;sample=EspWifiRxMonitor::Observation();replaced.arm(sample);
    sample.received=1;sample.eof=0x3ffe8000;sample.flags=0xc0028080;replaced.observe(sample);
    sample.eof+=16;sample.flags=0x80080080;replaced.observe(sample);
    CHECK(replaced.failed() && !replaced.recycled());

    EspWifiRxMonitor length;sample=EspWifiRxMonitor::Observation();length.arm(sample);
    sample.received=1;sample.eof=0x3ffe8000;sample.flags=0xc0028080;length.observe(sample);
    sample.flags=0x80028080;length.observe(sample);
    CHECK(!length.recycled() && !length.complete()); // clearing EOF without returning capacity

    EspWifiRxMonitor overflow;sample=EspWifiRxMonitor::Observation();overflow.arm(sample);
    sample.received=2;overflow.observe(sample);
    CHECK(overflow.failed()); // one injected frame cannot certify two receptions

    EspWifiRxMonitor malformed;sample=EspWifiRxMonitor::Observation();malformed.arm(sample);
    sample.received=1;sample.eof=0x3ffe8001;sample.flags=0xc0028080;malformed.observe(sample);
    CHECK(malformed.failed());
    EspWifiRxMonitor empty;sample=EspWifiRxMonitor::Observation();empty.arm(sample);
    sample.received=1;sample.eof=0x3ffe8000;sample.flags=0xc0000080;empty.observe(sample);
    CHECK(empty.failed()); // an empty EOF cannot represent the injected frame
    EspWifiRxMonitor armed;sample=EspWifiRxMonitor::Observation();armed.arm(sample);armed.arm(sample);
    CHECK(armed.failed());

    Bytes request(24,'\0'),response;request[0]=0x40;
    request.replace(4,6,6,(char)0xff);
    request.replace(10,6,"\x02\x11\x22\x33\x44\x55",6);
    CHECK(EspWifiRxAcceptance::response_for(request,response));
    CHECK(response.size()==55 && u8(response[0])==0x50);
    CHECK(response.substr(4,6)==request.substr(10,6)); // actual station, not a fixture-private MAC
    CHECK(response.substr(10,6)==Bytes("\x02\x77\x88\x99\xaa\xbb",6));
    CHECK(response.substr(16,6)==response.substr(10,6));
    CHECK(response.substr(36,10)==Bytes("\0\x08sim-test",10));
    CHECK(response.substr(52,3)==Bytes("\x03\x01\x01",3));
    request[0]=0x80;CHECK(!EspWifiRxAcceptance::response_for(request,response));
    request[0]=0x40;request[1]=0x40;CHECK(!EspWifiRxAcceptance::response_for(request,response));
    request[1]=1;CHECK(!EspWifiRxAcceptance::response_for(request,response));
    request.resize(23);CHECK(!EspWifiRxAcceptance::response_for(request,response));
#undef CHECK
    return checks;
}

#ifdef ESP_WIFI_ACCEPTANCE_TEST_MAIN
int main(){int checks=esp_wifi_acceptance_tests();printf("Wi-Fi acceptance checks: %d\n",checks);return checks<0;}
#endif
