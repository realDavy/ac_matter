#include "rmt_ir.hpp"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace ir_ac {
namespace {

static const char *TAG = "rmt_ir";

static constexpr size_t kMaxSymbols = 512;
/*
 * TX and RX must share the same resolution on ESP32-S3 (one RMT group
 * prescale). 250 kHz RX + 1 MHz TX caused: channel prescale out of range.
 * At 1 MHz, signal_range_max_ns must stay under ~32767000 (15-bit ticks).
 */
static constexpr uint32_t kResolutionHz = 1000000; /* 1 us / tick */
static constexpr uint32_t kRxMinNs = 2000;
static constexpr uint32_t kRxMaxNs = 32000000; /* ~32 ms idle ends a frame */
static rmt_channel_handle_t s_tx = nullptr;
static rmt_channel_handle_t s_rx = nullptr;
static rmt_encoder_handle_t s_copy_encoder = nullptr;
static rmt_carrier_config_t s_carrier = {};
static QueueHandle_t s_rx_queue = nullptr;
static rmt_symbol_word_t s_rx_symbols[kMaxSymbols];
static std::vector<uint32_t> s_last_frame_us;
static bool s_frame_ready = false;
static bool s_rx_armed = false;
static bool s_busy = false;

struct RxDoneMsg {
    size_t num_symbols;
};

static bool IRAM_ATTR rx_done_callback(rmt_channel_handle_t,
                                       const rmt_rx_done_event_data_t *edata,
                                       void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    auto *queue = static_cast<QueueHandle_t>(user_data);
    if (queue == nullptr || edata == nullptr) {
        return false;
    }
    RxDoneMsg msg = {};
    msg.num_symbols = edata->num_symbols;
    xQueueSendFromISR(queue, &msg, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void symbols_to_timings(const rmt_symbol_word_t *symbols,
                               size_t count,
                               std::vector<uint32_t> *out)
{
    out->clear();
    out->reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        const auto &sym = symbols[i];
        /* resolution is 1 MHz → duration ticks == microseconds */
        if (sym.duration0 > 0) {
            out->push_back(sym.duration0);
        }
        if (sym.duration1 > 0) {
            out->push_back(sym.duration1);
        }
    }
}

}  // namespace

void rmt_ir_deinit();

esp_err_t rmt_ir_init(gpio_num_t tx_gpio, gpio_num_t rx_gpio)
{
    if (s_tx != nullptr && s_rx != nullptr) {
        return ESP_OK;
    }

    /* Partial init from a previous failure — tear down and retry cleanly. */
    if (s_tx != nullptr || s_rx != nullptr || s_copy_encoder != nullptr ||
        s_rx_queue != nullptr) {
        ESP_LOGW(TAG, "Cleaning partial RMT IR state before re-init");
        rmt_ir_deinit();
    }

    s_rx_queue = xQueueCreate(4, sizeof(RxDoneMsg));
    if (s_rx_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = tx_gpio;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = kResolutionHz;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 4;

    rmt_copy_encoder_config_t copy_cfg = {};

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = rx_gpio;
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = kResolutionHz;
    rx_cfg.mem_block_symbols = 128;

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rx_done_callback,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tx channel: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy encoder: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    s_carrier = {};
    s_carrier.frequency_hz = 38000;
    s_carrier.duty_cycle = 0.33f;
    err = rmt_apply_carrier(s_tx, &s_carrier);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "carrier: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    err = rmt_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable tx: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    err = rmt_new_rx_channel(&rx_cfg, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx channel: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    /*
     * Demodulating receivers (VS1838 / HS0038 / RM Mini IR) idle HIGH and
     * pull LOW during marks. Enable a weak pull-up so a disconnected or
     * open-drain OUT does not float and block RMT edge detection.
     */
    (void)gpio_set_pull_mode(rx_gpio, GPIO_PULLUP_ONLY);

    err = rmt_rx_register_event_callbacks(s_rx, &cbs, s_rx_queue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx callbacks: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    err = rmt_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable rx: %s", esp_err_to_name(err));
        rmt_ir_deinit();
        return err;
    }

    ESP_LOGI(TAG, "RMT IR ready: TX=GPIO%d RX=GPIO%d @%uHz "
                  "(idle level=%d, expect 1)",
             static_cast<int>(tx_gpio), static_cast<int>(rx_gpio),
             static_cast<unsigned>(kResolutionHz), gpio_get_level(rx_gpio));
    return ESP_OK;
}

void rmt_ir_deinit()
{
    rmt_ir_stop_receive();

    if (s_rx) {
        rmt_disable(s_rx);
        rmt_del_channel(s_rx);
        s_rx = nullptr;
    }
    if (s_tx) {
        rmt_disable(s_tx);
        rmt_del_channel(s_tx);
        s_tx = nullptr;
    }
    if (s_copy_encoder) {
        rmt_del_encoder(s_copy_encoder);
        s_copy_encoder = nullptr;
    }
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = nullptr;
    }
    s_frame_ready = false;
    s_last_frame_us.clear();
}

esp_err_t rmt_ir_transmit(const std::vector<int> &timings_us,
                          uint32_t carrier_hz)
{
    if (s_tx == nullptr || s_copy_encoder == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timings_us.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    if (carrier_hz == 0) {
        carrier_hz = 38000;
    }
    if (s_carrier.frequency_hz != carrier_hz) {
        s_carrier.frequency_hz = carrier_hz;
        ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx, &s_carrier), TAG,
                            "update carrier");
    }

    /*
     * Pack consecutive mark/space pairs into RMT symbols.
     * timingList from SWIGLIB IRsend is [mark, space, mark, space, ...].
     */
    std::vector<rmt_symbol_word_t> symbols;
    symbols.reserve((timings_us.size() + 1) / 2);

    for (size_t i = 0; i < timings_us.size(); i += 2) {
        rmt_symbol_word_t sym = {};
        const uint32_t mark =
            static_cast<uint32_t>(timings_us[i] > 0 ? timings_us[i] : 1);
        const uint32_t space =
            (i + 1 < timings_us.size())
                ? static_cast<uint32_t>(
                      timings_us[i + 1] > 0 ? timings_us[i + 1] : 1)
                : 0;
        sym.level0 = 1;
        sym.duration0 = mark > 0x7FFF ? 0x7FFF : mark;
        sym.level1 = 0;
        sym.duration1 = space > 0x7FFF ? 0x7FFF : space;
        symbols.push_back(sym);
    }

    s_busy = true;

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;

    esp_err_t err =
        rmt_transmit(s_tx, s_copy_encoder, symbols.data(),
                     symbols.size() * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_tx, pdMS_TO_TICKS(2000));
    }

    s_busy = false;
    return err;
}

esp_err_t rmt_ir_start_receive()
{
    if (s_rx == nullptr || s_rx_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Already waiting for a frame — do not call rmt_receive again. */
    if (s_rx_armed) {
        return ESP_OK;
    }

    s_frame_ready = false;
    s_last_frame_us.clear();
    xQueueReset(s_rx_queue);

    rmt_receive_config_t cfg = {};
    cfg.signal_range_min_ns = kRxMinNs;
    /* Must stay under 32767 RX ticks (~131 ms at 250 kHz). */
    cfg.signal_range_max_ns = kRxMaxNs;

    esp_err_t err =
        rmt_receive(s_rx, s_rx_symbols, sizeof(s_rx_symbols), &cfg);
    if (err == ESP_OK) {
        s_rx_armed = true;
    } else {
        ESP_LOGW(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool rmt_ir_rx_armed()
{
    return s_rx_armed;
}

void rmt_ir_stop_receive()
{
    s_rx_armed = false;
    /* Disabling/enabling clears an in-flight receive cleanly. */
    if (s_rx) {
        rmt_disable(s_rx);
        rmt_enable(s_rx);
    }
}

bool rmt_ir_frame_ready()
{
    if (s_rx_queue == nullptr) {
        return s_frame_ready;
    }

    RxDoneMsg msg = {};
    while (xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
        if (msg.num_symbols > 0 && msg.num_symbols <= kMaxSymbols) {
            symbols_to_timings(s_rx_symbols, msg.num_symbols, &s_last_frame_us);
            s_frame_ready = !s_last_frame_us.empty();
        }
        s_rx_armed = false;
    }

    return s_frame_ready;
}

bool rmt_ir_consume_frame(std::vector<uint32_t> *out_us)
{
    if (!rmt_ir_frame_ready() || out_us == nullptr) {
        return false;
    }
    *out_us = s_last_frame_us;
    s_frame_ready = false;
    s_last_frame_us.clear();
    return !out_us->empty();
}

bool rmt_ir_is_busy()
{
    return s_busy;
}

}  // namespace ir_ac
