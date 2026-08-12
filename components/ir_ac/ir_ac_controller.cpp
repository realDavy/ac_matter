#include "ir_ac_controller.hpp"

#include <string.h>

#include <memory>
#include <vector>

#include "IRac.h"
#include "IRrecv.h"
#include "IRremoteESP8266.h"
#include "IRsend.h"
#include "IRutils.h"
#include "esp_log.h"
#include "rmt_ir.hpp"

/* Provided by IRsend.cpp when SWIGLIB is defined. */
extern std::vector<int> timingList;

namespace ir_ac {
namespace {

static const char *TAG = "ir_ac";

static std::unique_ptr<IRac> s_iac;
static std::unique_ptr<IRrecv> s_irrecv;

/*
 * Protocols commonly useful for manual Alt traversal. Models default to 1
 * where a protocol family has sub-models (Fujitsu/Panasonic/etc.).
 */
struct AltEntry {
    decode_type_t protocol;
    int16_t model;
};

static const AltEntry kAltProtocols[] = {
    {decode_type_t::COOLIX, -1},
    {decode_type_t::DAIKIN, -1},
    {decode_type_t::DAIKIN2, -1},
    {decode_type_t::DAIKIN216, -1},
    {decode_type_t::GREE, 1},
    {decode_type_t::MIDEA, -1},
    {decode_type_t::MITSUBISHI_AC, -1},
    {decode_type_t::MITSUBISHI112, -1},
    {decode_type_t::MITSUBISHI_HEAVY_152, -1},
    {decode_type_t::PANASONIC_AC, 1},
    {decode_type_t::FUJITSU_AC, 1},
    {decode_type_t::LG, 1},
    {decode_type_t::LG2, 1},
    {decode_type_t::SAMSUNG_AC, -1},
    {decode_type_t::TOSHIBA_AC, -1},
    {decode_type_t::HITACHI_AC, -1},
    {decode_type_t::HITACHI_AC1, -1},
    {decode_type_t::HAIER_AC, -1},
    {decode_type_t::HAIER_AC_YRW02, -1},
    {decode_type_t::KELVINATOR, -1},
    {decode_type_t::SHARP_AC, -1},
    {decode_type_t::TCL112AC, -1},
    {decode_type_t::WHIRLPOOL_AC, -1},
    {decode_type_t::CARRIER_AC64, -1},
    {decode_type_t::ELECTRA_AC, -1},
    {decode_type_t::CORONA_AC, -1},
    {decode_type_t::GOODWEATHER, -1},
    {decode_type_t::NEOCLIMA, -1},
    {decode_type_t::VESTEL_AC, -1},
    {decode_type_t::TECO, -1},
    {decode_type_t::TROTEC, -1},
    {decode_type_t::AMCOR, -1},
};

static stdAc::opmode_t to_std_mode(int mode)
{
    switch (mode) {
        case 0:
            return stdAc::opmode_t::kAuto;
        case 1:
            return stdAc::opmode_t::kCool;
        case 2:
            return stdAc::opmode_t::kHeat;
        case 3:
            return stdAc::opmode_t::kDry;
        case 4:
            return stdAc::opmode_t::kFan;
        default:
            return stdAc::opmode_t::kCool;
    }
}

static int from_std_mode(stdAc::opmode_t mode)
{
    switch (mode) {
        case stdAc::opmode_t::kAuto:
            return 0;
        case stdAc::opmode_t::kCool:
            return 1;
        case stdAc::opmode_t::kHeat:
            return 2;
        case stdAc::opmode_t::kDry:
            return 3;
        case stdAc::opmode_t::kFan:
            return 4;
        default:
            return 1;
    }
}

static stdAc::fanspeed_t to_std_fan(int fan)
{
    switch (fan) {
        case 0:
            return stdAc::fanspeed_t::kAuto;
        case 1:
            return stdAc::fanspeed_t::kLow;
        case 2:
            return stdAc::fanspeed_t::kMedium;
        case 3:
            return stdAc::fanspeed_t::kHigh;
        default:
            return stdAc::fanspeed_t::kAuto;
    }
}

static int from_std_fan(stdAc::fanspeed_t fan)
{
    switch (fan) {
        case stdAc::fanspeed_t::kMin:
        case stdAc::fanspeed_t::kLow:
            return 1;
        case stdAc::fanspeed_t::kMedium:
            return 2;
        case stdAc::fanspeed_t::kHigh:
        case stdAc::fanspeed_t::kMax:
            return 3;
        case stdAc::fanspeed_t::kAuto:
        default:
            return 0;
    }
}

static constexpr uint32_t kRawTick = 2; /* IRremoteESP8266 uses 2 us ticks */
/* Leave one spare slot: IRrecv::decode() may write rawbuf[rawlen]=0. */
static constexpr size_t kMaxDecodeTimings = 512;

static bool fill_decode_results(const std::vector<uint32_t> &timings_us,
                                decode_results *results,
                                std::vector<uint16_t> *raw_ticks)
{
    if (timings_us.size() < 4 || results == nullptr || raw_ticks == nullptr) {
        return false;
    }

    size_t count = timings_us.size();
    if (count > kMaxDecodeTimings) {
        ESP_LOGW(TAG, "Truncating IR timings %u -> %u",
                 static_cast<unsigned>(count),
                 static_cast<unsigned>(kMaxDecodeTimings));
        count = kMaxDecodeTimings;
    }

    /*
     * Index 0 unused (IRremote convention). rawlen == count+1 entries used.
     * Allocate +2 so decode()'s optional rawbuf[rawlen]=0 sentinel is in-bounds.
     */
    raw_ticks->assign(count + 2, 0);
    for (size_t i = 0; i < count; ++i) {
        uint32_t ticks = timings_us[i] / kRawTick;
        if (ticks > UINT16_MAX) {
            ticks = UINT16_MAX;
        }
        if (ticks == 0) {
            ticks = 1;
        }
        (*raw_ticks)[i + 1] = static_cast<uint16_t>(ticks);
    }

    memset(results, 0, sizeof(*results));
    results->rawbuf = raw_ticks->data();
    results->rawlen = static_cast<uint16_t>(count + 1);
    results->overflow = false;
    results->decode_type = decode_type_t::UNKNOWN;
    return true;
}

}  // namespace

esp_err_t IrAcController::begin(gpio_num_t tx_gpio, gpio_num_t rx_gpio)
{
    tx_gpio_ = tx_gpio;
    rx_gpio_ = rx_gpio;

    esp_err_t err = rmt_ir_init(tx_gpio, rx_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT init failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * IRac/IRrecv still need GPIO numbers for their constructors, but under
     * SWIGLIB + UNIT_TEST they do not drive those pins directly.
     */
    s_iac = std::make_unique<IRac>(static_cast<uint16_t>(tx_gpio));
    s_irrecv = std::make_unique<IRrecv>(static_cast<uint16_t>(rx_gpio), 512,
                                        90 /* ms timeout for AC frames */);

    IRac::initState(&s_iac->next);
    s_iac->next.protocol = decode_type_t::UNKNOWN;
    s_iac->next.model = -1;
    s_iac->next.celsius = true;
    logical_ = {};
    pairing_ = {};
    init_ok_ = true;
    init_ok = true;

    ESP_LOGI(TAG, "IRremoteESP8266 AC controller ready (%s)", lib_version());
    return ESP_OK;
}

void IrAcController::end()
{
    stop_capture();
    s_iac.reset();
    s_irrecv.reset();
    rmt_ir_deinit();
    init_ok_ = false;
    init_ok = false;
    pairing_ = {};
}

bool IrAcController::apply_pairing(const AcPairingInfo &info)
{
    if (!init_ok_ || !s_iac) {
        return false;
    }
    if (!info.valid || !IRac::isProtocolSupported(
                            static_cast<decode_type_t>(info.protocol))) {
        return false;
    }

    pairing_ = info;
    s_iac->next.protocol = static_cast<decode_type_t>(info.protocol);
    s_iac->next.model = info.model;
    s_iac->markAsSent();
    return true;
}

void IrAcController::clear_pairing()
{
    pairing_ = {};
    if (s_iac) {
        s_iac->next.protocol = decode_type_t::UNKNOWN;
        s_iac->next.model = -1;
    }
    alt_index_ = 0;
}

void IrAcController::start_capture()
{
    if (!init_ok_) {
        return;
    }
    capture_armed_ = (rmt_ir_start_receive() == ESP_OK);
}

void IrAcController::stop_capture()
{
    if (capture_armed_) {
        rmt_ir_stop_receive();
        capture_armed_ = false;
    }
}

bool IrAcController::signal_captured()
{
    return rmt_ir_frame_ready();
}

bool IrAcController::capture_armed() const
{
    return capture_armed_ && rmt_ir_rx_armed();
}

bool IrAcController::is_busy() const
{
    return rmt_ir_is_busy();
}

void IrAcController::sync_iac_from_logical_(const AcLogicalState &state)
{
    if (!s_iac) {
        return;
    }
    s_iac->next.power = state.power;
    s_iac->next.mode = to_std_mode(state.mode);
    s_iac->next.degrees = static_cast<float>(state.temp_c);
    s_iac->next.celsius = true;
    s_iac->next.fanspeed = to_std_fan(state.fan);
    s_iac->next.swingv = stdAc::swingv_t::kOff;
    s_iac->next.swingh = stdAc::swingh_t::kOff;
    s_iac->next.quiet = false;
    s_iac->next.turbo = false;
    s_iac->next.econo = false;
    s_iac->next.light = true;
    s_iac->next.filter = false;
    s_iac->next.clean = false;
    s_iac->next.beep = false;
    s_iac->next.sleep = -1;
    s_iac->next.clock = -1;
}

bool IrAcController::send_current_()
{
    if (!s_iac || !pairing_.valid) {
        return false;
    }

    timingList.clear();
    const bool ok = s_iac->sendAc();
    if (!ok) {
        ESP_LOGE(TAG, "IRac::sendAc failed for protocol %d",
                 static_cast<int>(pairing_.protocol));
        return false;
    }

    if (timingList.empty()) {
        ESP_LOGE(TAG, "No IR timings generated");
        return false;
    }

    esp_err_t err = rmt_ir_transmit(timingList, 38000);
    timingList.clear();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(err));
        return false;
    }

    logical_.power = s_iac->next.power;
    logical_.mode = from_std_mode(s_iac->next.mode);
    logical_.temp_c = static_cast<int>(s_iac->next.degrees + 0.5f);
    logical_.fan = from_std_fan(s_iac->next.fanspeed);
    return true;
}

bool IrAcController::send_power(bool on)
{
    if (!pairing_.valid) {
        return false;
    }
    stop_capture();
    logical_.power = on;
    sync_iac_from_logical_(logical_);
    return send_current_();
}

bool IrAcController::send_state(const AcLogicalState &state)
{
    if (!pairing_.valid) {
        return false;
    }
    stop_capture();
    logical_ = state;
    sync_iac_from_logical_(logical_);
    return send_current_();
}

bool IrAcController::decode_frame_to_state_(AcLogicalState *logical,
                                            AcPairingInfo *pairing_out)
{
    std::vector<uint32_t> timings_us;
    if (!rmt_ir_consume_frame(&timings_us)) {
        return false;
    }

    ESP_LOGI(TAG, "Decoding IR frame: %u timings",
             static_cast<unsigned>(timings_us.size()));

    /*
     * Keep decode_results off the IR worker stack — AC decode walks many
     * protocols and already needs a large stack; a stack-local results
     * object contributed to heap corruption (TLSF assert) on 8KB stacks.
     */
    auto results = std::make_unique<decode_results>();
    std::vector<uint16_t> raw_ticks;
    if (!fill_decode_results(timings_us, results.get(), &raw_ticks)) {
        ESP_LOGW(TAG, "IR frame too short to decode (%u timings)",
                 static_cast<unsigned>(timings_us.size()));
        return false;
    }

    if (!s_irrecv || !s_irrecv->decode(results.get())) {
        ESP_LOGW(TAG, "IR frame not decoded (%u timings)",
                 static_cast<unsigned>(timings_us.size()));
        return false;
    }

    if (!IRac::isProtocolSupported(results->decode_type)) {
        ESP_LOGW(TAG, "Decoded protocol %s is not an AC protocol",
                 typeToString(results->decode_type).c_str());
        return false;
    }

    stdAc::state_t state;
    IRac::initState(&state);
    if (!IRAcUtils::decodeToState(results.get(), &state)) {
        ESP_LOGW(TAG, "decodeToState failed for %s",
                 typeToString(results->decode_type).c_str());
        return false;
    }

    if (pairing_out) {
        pairing_out->protocol = static_cast<int32_t>(state.protocol);
        pairing_out->model = state.model;
        pairing_out->valid = true;
    }

    if (logical) {
        logical->power = state.power;
        logical->mode = from_std_mode(state.mode);
        logical->temp_c = static_cast<int>(state.degrees + 0.5f);
        logical->fan = from_std_fan(state.fanspeed);
    }

    ESP_LOGI(TAG, "Decoded AC protocol=%s model=%d power=%d mode=%d temp=%d fan=%d",
             typeToString(state.protocol).c_str(),
             static_cast<int>(state.model),
             static_cast<int>(state.power),
             from_std_mode(state.mode),
             static_cast<int>(state.degrees + 0.5f),
             from_std_fan(state.fanspeed));
    return true;
}

bool IrAcController::pair_from_capture()
{
    stop_capture();

    AcPairingInfo info{};
    AcLogicalState state{};
    if (!decode_frame_to_state_(&state, &info)) {
        return false;
    }

    pairing_ = info;
    logical_ = state;
    /* Prefer Cool/25C defaults after pairing if remote sent fan-only etc. */
    if (logical_.temp_c < 16 || logical_.temp_c > 30) {
        logical_.temp_c = 25;
    }
    if (logical_.mode < 0 || logical_.mode > 4) {
        logical_.mode = 1;
    }

    if (s_iac) {
        s_iac->next.protocol = static_cast<decode_type_t>(pairing_.protocol);
        s_iac->next.model = pairing_.model;
        sync_iac_from_logical_(logical_);
        s_iac->markAsSent();
    }

    return true;
}

bool IrAcController::parse_capture(AcLogicalState *out)
{
    AcLogicalState state{};
    AcPairingInfo info{};
    if (!decode_frame_to_state_(&state, &info)) {
        return false;
    }

    /*
     * Only accept frames that match the paired protocol so unrelated remotes
     * do not clobber Matter state.
     */
    if (pairing_.valid && info.protocol != pairing_.protocol) {
        ESP_LOGW(TAG, "Ignoring IR from protocol %d (paired=%d)",
                 static_cast<int>(info.protocol),
                 static_cast<int>(pairing_.protocol));
        return false;
    }

    logical_ = state;
    if (out) {
        *out = state;
    }

    if (s_iac) {
        sync_iac_from_logical_(logical_);
        s_iac->markAsSent();
    }
    return true;
}

size_t IrAcController::alt_protocol_count() const
{
    return sizeof(kAltProtocols) / sizeof(kAltProtocols[0]);
}

bool IrAcController::apply_alt_index(size_t index)
{
    if (!s_iac || index >= alt_protocol_count()) {
        return false;
    }

    const AltEntry &entry = kAltProtocols[index];
    if (!IRac::isProtocolSupported(entry.protocol)) {
        return false;
    }

    alt_index_ = index;
    pairing_.protocol = static_cast<int32_t>(entry.protocol);
    pairing_.model = entry.model;
    pairing_.valid = true;

    s_iac->next.protocol = entry.protocol;
    s_iac->next.model = entry.model;

    /* Emit a Cool / 25C / Auto-fan test frame. */
    logical_.power = true;
    logical_.mode = 1;
    logical_.temp_c = 25;
    logical_.fan = 0;
    sync_iac_from_logical_(logical_);
    return send_current_();
}

const char *IrAcController::protocol_name() const
{
    if (!pairing_.valid) {
        return "none";
    }
    static std::string name;
    name = typeToString(static_cast<decode_type_t>(pairing_.protocol));
    return name.c_str();
}

const char *IrAcController::lib_version() const
{
    return _IRREMOTEESP8266_VERSION_STR;
}

}  // namespace ir_ac
