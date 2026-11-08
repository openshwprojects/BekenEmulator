#include "emulator_backend.h"
#include "emulator.h"
#include "platform.h"
#include "esp_test_images.h"
#include <stdio.h>

namespace {
void capture_uart(void *context, int port, u8 byte)
{
    if (port == 0) ((Bytes *)context)->push_back((char)byte);
}

struct BackendWorker {
    EmulatorBackend *backend;
    Thread thread;
    explicit BackendWorker(EmulatorBackend *b) : backend(b) {}
    ~BackendWorker() { backend->stop(); thread.join(); }
    static void run(void *context) { ((BackendWorker *)context)->backend->run(); }
};

void instruction(Bytes &code, u32 word)
{
    for (unsigned byte = 0; byte < 3; ++byte) code += (char)(word >> (byte * 8));
}

void literal_load(Bytes &code, unsigned reg, unsigned offset)
{
    const u32 displacement = (offset - (((u32)code.size() + 3) & ~3u)) / 4;
    instruction(code, 1 | (reg << 4) | ((displacement & 0xffff) << 8));
}

Bytes gpio_edge_image(bool esp32)
{
    Bytes code(12, '\0');
    wr32le(&code[0], esp32 ? 0x3ff4409cu : 0x6000033cu); // GPIO5 configuration
    wr32le(&code[4], esp32 ? 0x3ff44044u : 0x6000031cu); // GPIO edge status
    wr32le(&code[8], esp32 ? 0x3ff40000u : 0x60000000u); // UART0 FIFO
    literal_load(code, 3, 0);
    instruction(code, 0x80a022); // MOVI a2,128: rising-edge detection
    instruction(code, 0x006322); // S32I a2,a3,0
    for (unsigned i = 3; i < 1024; ++i) instruction(code, 0x0020f0); // NOP
    literal_load(code, 3, 4);
    instruction(code, 0x002322); // L32I a2,a3,0: read latched status
    literal_load(code, 3, 8);
    instruction(code, 0x006322); // emit the observed status byte
    instruction(code, 0x007000); // WAITI 0
    return esp_test::image(esp32, code, 12);
}

Bytes clock_wait_image(bool esp32)
{
    Bytes code(0x100, '\0');
    wr32le(&code[0], esp32 ? 0x40080000u : 0x40100000u);
    wr32le(&code[4], 80000000); // CCOMPARE0: one second at initial80MHz
    literal_load(code, 2, 0);
    instruction(code, 0x13e720); // WSR a2,VECBASE
    instruction(code, 0x00a022); // MOVI a2,0
    instruction(code, 0x13e620); // WSR a2,PS: kernel interrupt vector
    literal_load(code, 2, 4);
    instruction(code, 0x13f020); // WSR a2,CCOMPARE0
    instruction(code, 0x40a022); // MOVI a2,64
    instruction(code, 0x13e420); // WSR a2,INTENABLE
    instruction(code, 0x007000); // WAITI0 advances time to the enabled compare
    code.resize(0x400, '\0');
    Bytes handler;
    instruction(handler, 0x00a022);
    instruction(handler, 0x13e420); // disable timer interrupts
    instruction(handler, 0x007000); // remain idle after the observation
    code.replace(esp32 ? 0x300 : 0x30, handler.size(), handler);
    return esp_test::image(esp32, code, 0x100);
}
}

int backend_tests()
{
    int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { printf("[FAIL] backends:%d: %s\n", __LINE__, #c); return -1; } } while (0)
    BackendInfo info;
    CHECK(backend_info("  esp8266 ", info));
    CHECK(info.chip == "ESP8266" && info.family == "Espressif" && info.architecture == "Xtensa LX106");
    CHECK(info.gpio_pins == 17 && info.console_uart == 0 && info.uart_rx_mask == 1);
    CHECK(backend_info("ESP32", info));
    CHECK(info.architecture == "Xtensa LX6");
    CHECK(info.gpio_pins == 40 && info.uart_rx_mask == 7);
    CHECK(backend_info("BK7231N", info));
    CHECK(info.family == "Beken" && info.architecture == "ARM");
    CHECK(!backend_info("ESP32-C3", info));
    StrList names = backend_chip_names();
    CHECK(names.size() == chip_names().size() + 2);
    BuildOptions options;
    options.chip = "not-a-chip";
    std::string error;
    EmulatorBackend *backend = build_backend("missing.bin", options, 0, &error);
    CHECK(!backend && contains(error, "Unknown") && contains(error, "ESP32"));
    options.chip = "ESP32";
    KeyCoefs key;
    key.has = true;
    backend = build_backend("missing.bin", options, &key, &error);
    CHECK(!backend && contains(error, "-key"));
    options.wifi = true;
    backend = build_backend("missing.bin", options, 0, &error);
    CHECK(!backend && contains(error, "Wi-Fi"));
    options.wifi = false;
    backend = build_backend("missing.bin", options, 0, &error);
    CHECK(!backend && !error.empty());
    options.chip = "BK7231";
    options.xtal_mhz = 26;
    backend = build_backend("missing.bin", options, 0, &error);
    CHECK(!backend && contains(error, "--xtal-mhz"));
    options.xtal_mhz = 0;
    options.psram_mb = 8;
    backend = build_backend("missing.bin", options, 0, &error);
    CHECK(!backend && contains(error,"--psram-mb"));
    options.psram_mb = 0;
    /* Exercise the old ARM constructor through the same front-end factory. */
    options.chip = "BK7231";
    options.only_uart = true;
    std::string path = make_temp_file("backend_arm_", ".bin");
    CHECK(write_file(path, Bytes(0x22000, (char)0xff)));
    backend = build_backend(path, options, 0, &error);
    remove_file(path);
    CHECK(backend != 0);
    CHECK(backend->beken() != 0 && backend->info().family == "Beken");
    CHECK(backend->setup(&error));
    CHECK(!backend->status().faulted);
    CHECK(!backend->status().elapsed_us_known);
    delete backend;
    const char *esp_chips[] = {"ESP8266", "ESP32"};
    for (unsigned i = 0; i < 2; ++i) {
        options.chip = esp_chips[i];
        path = make_temp_file("backend_xtensa_", ".bin");
        CHECK(write_file(path, esp_test::uart_image(i == 1)));
        options.psram_mb = i ? 3 : 2;
        backend = build_backend(path, options, 0, &error);
        CHECK(!backend && contains(error,"PSRAM"));
        options.psram_mb = i ? 8 : 0;
        options.xtal_mhz = 25;
        backend = build_backend(path, options, 0, &error);
        CHECK(!backend && contains(error, "crystal frequency"));
        options.xtal_mhz = i ? 40 : 24;
        backend = build_backend(path, options, 0, &error);
        remove_file(path);
        CHECK(backend != 0);
        CHECK(backend->beken() == 0);
        Bytes uart;
        backend->set_uart_sink(capture_uart, &uart);
        CHECK(backend->setup(&error));
        backend->run();
        CHECK(!backend->status().faulted);
        CHECK(backend->status().instructions == 4);
        CHECK(uart == "A");
        delete backend;
        path = make_temp_file("backend_thread_", ".bin");
        CHECK(write_file(path, esp_test::uart_image(i == 1, true)));
        backend = build_backend(path, options, 0, &error);
        remove_file(path);
        CHECK(backend != 0);
        uart.clear(); backend->set_uart_sink(capture_uart, &uart);
        /* Pause between publication boundaries, as a real UI Pause can. */
        const std::string old_limit = env_get("EMU_MAX_INSNS");
        env_set("EMU_MAX_INSNS", "1");
        backend->run();
        env_set("EMU_MAX_INSNS", old_limit.empty() ? 0 : old_limit.c_str());
        CHECK(backend->status().instructions == 1);
        backend->pause(true);
        CHECK(backend->is_paused());
        CHECK(backend->feed_uart(0, "hello") && !backend->feed_uart(3, "invalid"));
        CHECK(backend->set_gpio_input(4, 1) && !backend->set_gpio_input(40, 1));
        if(i==1) {
            CHECK(!backend->set_gpio_input(24,1));
            for(int pin=28;pin<=31;++pin)CHECK(!backend->set_gpio_input(pin,1));
            CHECK(backend->set_gpio_input(34,1)); /* input-only pins still accept input */
        }
        {
            BackendWorker worker(backend);
            CHECK(worker.thread.start(BackendWorker::run, &worker));
            double deadline = mono_time() + 5;
            while (!backend->status().gpio[4].input && mono_time() < deadline) sleep_ms(1);
            CHECK(backend->status().gpio[4].input);
            CHECK(backend->status().instructions == 1);
            backend->pause(false);
            while (backend->status().instructions < 4 && mono_time() < deadline) sleep_ms(1);
            CHECK(backend->status().instructions >= 4);
            backend->stop();
            CHECK(worker.thread.join(5000));
        }
        CHECK(!backend->status().faulted && uart == "A");
        u64 stopped = backend->status().instructions;
        backend->run(); /* A stop requested before run must remain effective. */
        CHECK(backend->status().instructions == stopped);
        delete backend;

        /* A pulse entirely within one host-input batch must still latch its
         * edge in hardware, even though the final input level is low. */
        path = make_temp_file("backend_gpio_edge_", ".bin");
        CHECK(write_file(path, gpio_edge_image(i == 1)));
        options.gpio_inputs[5] = 0;
        backend = build_backend(path, options, 0, &error);
        options.gpio_inputs.clear();
        remove_file(path);
        CHECK(backend != 0);
        uart.clear(); backend->set_uart_sink(capture_uart, &uart);
        env_set("EMU_MAX_INSNS", "1024");
        backend->run();
        env_set("EMU_MAX_INSNS", old_limit.empty() ? 0 : old_limit.c_str());
        CHECK(backend->status().instructions == 1024 && !backend->status().faulted);
        CHECK(backend->set_gpio_input(5, 1) && backend->set_gpio_input(5, 0));
        backend->run();
        CHECK(!backend->status().faulted && backend->status().instructions == 1029);
        CHECK(!backend->status().gpio[5].input && uart == Bytes(1, (char)32));
        delete backend;

        path = make_temp_file("backend_clock_", ".bin");
        CHECK(write_file(path, clock_wait_image(i == 1)));
        backend = build_backend(path, options, 0, &error);
        remove_file(path);
        CHECK(backend != 0);
        CHECK(backend->status().elapsed_us_known && backend->status().elapsed_us == 0);
        backend->run();
        const BackendStatus clock = backend->status();
        CHECK(!clock.faulted && clock.instructions == 12);
        CHECK(clock.elapsed_us_known && clock.elapsed_us == 1000000);
        delete backend;
    }
#undef CHECK
    return checks;
}
