#include "emulator_backend.h"
#include "beken/emulator.h"

namespace {
class BekenBackend : public EmulatorBackend {
    BekenEmulator *machine_;
    BackendInfo info_;
public:
    explicit BekenBackend(BekenEmulator *machine)
        : machine_(machine) { backend_info(machine->chip->name, info_); }
    ~BekenBackend() { machine_->close_network(); delete machine_; }
    const BackendInfo &info() const { return info_; }
    bool setup(std::string *err) {
        if (machine_->flash_empty()) {
            if (err) *err = "Cannot map an empty flash image.";
            return false;
        }
        machine_->setup();
        return true;
    }
    void run(bool) { machine_->run(); }
    void stop() { machine_->stop(); }
    void pause(bool paused) { machine_->pause(paused); }
    bool is_paused() const { return machine_->is_paused(); }
    BackendStatus status() const {
        BackendStatus out;
        out.pc = machine_->cpu().reg_read(REG_PC);
        out.instructions = machine_->state.insn_count;
        out.faulted = machine_->has_run_error;
        out.diagnostic = machine_->run_error;
        return out;
    }
    void trace_pc(u32 address) { machine_->trace_pc(address); }
    void set_uart_sink(void (*sink)(void *, int, u8), void *ctx) {
        machine_->uart_sink = sink;
        machine_->uart_sink_ctx = ctx;
    }
    BekenEmulator *beken() { return machine_; }
    bool feed_uart(unsigned port, const Bytes &bytes) {
        if (port != 1) return false;
        machine_->feed_uart1(bytes);
        return true;
    }
    bool set_gpio_input(int pin, int level) { return machine_->set_gpio_input(pin, level); }
};
}

EmulatorBackend *build_beken_backend(const std::string &path, const BuildOptions &options,
    const KeyCoefs *key, std::string *err, void (*log)(const std::string &),
    bool save_decrypted, std::string *exception_type)
{
    if (!options.rom_path.empty()) {
        if (err) *err = "--rom is not supported by the Beken backend.";
        return 0;
    }
    if (options.xtal_mhz) {
        if (err) *err = "--xtal-mhz is not supported by the Beken backend.";
        return 0;
    }
    if (options.psram_mb) {
        if (err) *err = "--psram-mb is not supported by the Beken backend.";
        return 0;
    }
    BekenEmulator *machine = build_emulator(path, options, key, err, log, save_decrypted, exception_type);
    return machine ? new BekenBackend(machine) : 0;
}
