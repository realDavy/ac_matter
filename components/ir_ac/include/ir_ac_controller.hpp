#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

namespace ir_ac {

/*
 * Logical AC state used by the Matter driver.
 * Mode: 0=Auto, 1=Cool, 2=Heat, 3=Dry, 4=Fan
 * Fan:  0=Auto, 1=Low, 2=Medium, 3=High
 */
struct AcLogicalState {
    int temp_c = 25;
    int mode = 1;
    int fan = 1;
    bool power = false;
};

struct AcPairingInfo {
    int32_t protocol = 0;  /* decode_type_t */
    int16_t model = -1;
    bool valid = false;
};

/*
 * High-level air-conditioner IR controller built on IRremoteESP8266 (IRac)
 * for protocol encode/decode and ESP-IDF RMT for GPIO transmit/receive.
 *
 * Coverage is limited to protocols implemented by IRremoteESP8266.
 */
class IrAcController {
 public:
    esp_err_t begin(gpio_num_t tx_gpio, gpio_num_t rx_gpio);
    void end();

    bool ready() const { return init_ok_; }
    bool paired() const { return pairing_.valid; }
    AcPairingInfo pairing() const { return pairing_; }

    bool apply_pairing(const AcPairingInfo &info);
    void clear_pairing();

    void start_capture();
    void stop_capture();
    bool signal_captured();
    /* True while RMT RX is armed waiting for a frame. */
    bool capture_armed() const;
    bool is_busy() const;
    gpio_num_t rx_gpio() const { return rx_gpio_; }

    /* Decode one captured frame into a supported AC protocol and pair. */
    bool pair_from_capture();

    /* Decode capture into logical state (for Matter sync while paired). */
    bool parse_capture(AcLogicalState *out);

    bool send_power(bool on);
    bool send_state(const AcLogicalState &state);

    /* Alt / manual protocol traversal helpers. */
    size_t alt_protocol_count() const;
    bool apply_alt_index(size_t index);
    size_t current_alt_index() const { return alt_index_; }
    const char *protocol_name() const;

    const char *lib_version() const;

    bool init_ok = false;

 private:
    bool send_current_();
    void sync_iac_from_logical_(const AcLogicalState &state);
    bool decode_frame_to_state_(AcLogicalState *logical,
                                AcPairingInfo *pairing_out);

    gpio_num_t tx_gpio_ = GPIO_NUM_NC;
    gpio_num_t rx_gpio_ = GPIO_NUM_NC;
    bool init_ok_ = false;
    bool capture_armed_ = false;
    AcPairingInfo pairing_{};
    AcLogicalState logical_{};
    size_t alt_index_ = 0;
};

}  // namespace ir_ac
