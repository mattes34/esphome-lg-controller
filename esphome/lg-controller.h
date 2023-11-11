#include "esphome.h"

static const char* const TAG = "lg-controller";

#define MIN_TEMP_SETPOINT 16
#define MAX_TEMP_SETPOINT 30

class LgController;

// Custom switch. Notifies the controller when state changes in HA.
class LgSwitch final : public Switch {
  LgController* controller_;

 public:
  explicit LgSwitch(LgController* controller) : controller_(controller) {}

  void write_state(bool state) override;

  void restore_and_set_mode(SwitchRestoreMode mode) {
    set_restore_mode(mode);
    if (auto state = get_initial_state_with_restore_mode()) {
        write_state(*state);
    }
  }
};

// Installer settings that can be set in the YAML file.
struct InstallerSettings {
    // Installer setting 15.
    uint8_t over_heating = 0;
};

class LgController final : public climate::Climate, public Component {
    static constexpr size_t MsgLen = 13;
    static constexpr int RxPin = 26; // Keep in sync with rx_pin in base.yaml.

    climate::ClimateTraits supported_traits_;    

    esphome::uart::UARTComponent* serial_;
    esphome::sensor::Sensor* temperature_sensor_;
    esphome::template_::TemplateSelect* vane_select_1_;
    esphome::template_::TemplateSelect* vane_select_2_;
    esphome::template_::TemplateSelect* vane_select_3_;
    esphome::template_::TemplateSelect* vane_select_4_;

    esphome::template_::TemplateNumber* fan_speed_slow_;
    esphome::template_::TemplateNumber* fan_speed_low_;
    esphome::template_::TemplateNumber* fan_speed_medium_;
    esphome::template_::TemplateNumber* fan_speed_high_;

    esphome::sensor::Sensor error_code_;
    esphome::binary_sensor::BinarySensor defrost_;
    esphome::binary_sensor::BinarySensor preheat_;
    esphome::binary_sensor::BinarySensor outdoor_;
    uint32_t last_outdoor_change_millis_ = 0;

    LgSwitch purifier_;
    LgSwitch internal_thermistor_;

    uint8_t recv_buf_[MsgLen] = {};
    uint32_t recv_buf_len_ = 0;
    uint32_t last_recv_millis_ = 0;

    // Last received 0xC8 message.
    uint8_t last_recv_status_[MsgLen] = {};

    // Last received 0xCA message.
    uint8_t last_recv_type_a_settings_[MsgLen] = {};

    // Last received 0xCB message.
    uint8_t last_recv_type_b_settings_[MsgLen] = {};

    uint8_t send_buf_[MsgLen] = {};
    uint32_t last_sent_status_millis_ = 0;

    enum class PendingSendKind : uint8_t { None, Status, TypeA, TypeB };
    PendingSendKind pending_send_ = PendingSendKind::None;

    bool pending_status_change_ = false;
    bool pending_type_a_settings_change_ = false;
    bool pending_type_b_settings_change_ = false;

    bool is_initializing_ = true;

    InstallerSettings installer_settings_;
    uint8_t vane_position_[4] = {0,0,0,0};
    uint8_t fan_speed_[4] = {0,0,0,0};


    uint32_t NVS_STORAGE_VERSION = 2843654U; // Change version if the NVSStorage struct changes
    struct NVSStorage {
        uint8_t capabilities_message[13] = {};
    };
    NVSStorage nvs_storage_;

    enum LgCapability {
    PURIFIER,
    FAN_AUTO,
    FAN_SLOW,
    FAN_LOW,
    FAN_LOW_MEDIUM,
    FAN_MEDIUM,
    FAN_MEDIUM_HIGH,
    FAN_HIGH,
    MODE_HEATING,
    MODE_FAN,
    MODE_AUTO,
    MODE_DEHUMIDIFY,
    HAS_ONE_VANE,
    HAS_TWO_VANES,
    HAS_FOUR_VANES,
    VERTICAL_SWING,
    HORIZONTAL_SWING,
    HAS_ESP_VALUE_SETTING,
    };

    bool parse_capability(LgCapability capability) {
        switch (capability) {
            case PURIFIER:
                return (nvs_storage_.capabilities_message[2] & 0x02) != 0;
            case FAN_AUTO:
                return (nvs_storage_.capabilities_message[3] & 0x01) != 0;
            case FAN_SLOW:
                return (nvs_storage_.capabilities_message[3] & 0x20) != 0;
            case FAN_LOW:
                return (nvs_storage_.capabilities_message[3] & 0x10) != 0;
            case FAN_LOW_MEDIUM:
                return (nvs_storage_.capabilities_message[6] & 0x08) != 0;
            case FAN_MEDIUM:
                return (nvs_storage_.capabilities_message[3] & 0x08) != 0;
            case FAN_MEDIUM_HIGH:
                return (nvs_storage_.capabilities_message[6] & 0x10) != 0;
            case FAN_HIGH:
                return true;
            case MODE_HEATING:
                return (nvs_storage_.capabilities_message[2] & 0x40) != 0;
            case MODE_FAN:
                return (nvs_storage_.capabilities_message[2] & 0x80) != 0;
            case MODE_AUTO:
                return (nvs_storage_.capabilities_message[2] & 0x08) != 0;
            case MODE_DEHUMIDIFY:
                return (nvs_storage_.capabilities_message[2] & 0x80) != 0;
            case HAS_ONE_VANE:
                return (nvs_storage_.capabilities_message[5] & 0x40) != 0;
            case HAS_TWO_VANES:
                return (nvs_storage_.capabilities_message[5] & 0x80) != 0;
            case HAS_FOUR_VANES:
                // actual flag is unknown, assume 4 vanes if neither 1 nor 2 vanes are supported
                return (nvs_storage_.capabilities_message[5] & 0x40) == 0 && (nvs_storage_.capabilities_message[5] & 0x80) == 0;
            case VERTICAL_SWING:
                return (nvs_storage_.capabilities_message[1] & 0x80) != 0;
            case HORIZONTAL_SWING:
                return (nvs_storage_.capabilities_message[1] & 0x40) != 0;
            case HAS_ESP_VALUE_SETTING:
                return (nvs_storage_.capabilities_message[4] & 0x02) != 0;
            default:
                return false;
        }
    }

    void configure_capabilities() {
        // Default traits
        supported_traits_.set_supported_modes({
            climate::CLIMATE_MODE_OFF,
            climate::CLIMATE_MODE_COOL,
            climate::CLIMATE_MODE_HEAT,
            climate::CLIMATE_MODE_DRY,
            climate::CLIMATE_MODE_FAN_ONLY,
            climate::CLIMATE_MODE_HEAT_COOL,
        });
        supported_traits_.set_supported_fan_modes({
            climate::CLIMATE_FAN_LOW,
            climate::CLIMATE_FAN_MEDIUM,
            climate::CLIMATE_FAN_HIGH,
            climate::CLIMATE_FAN_AUTO,
        });
        supported_traits_.set_supported_swing_modes({
            climate::CLIMATE_SWING_OFF,
            climate::CLIMATE_SWING_BOTH,
            climate::CLIMATE_SWING_VERTICAL,
            climate::CLIMATE_SWING_HORIZONTAL,
        });
        supported_traits_.set_supports_current_temperature(true);
        supported_traits_.set_supports_two_point_target_temperature(false);
        supported_traits_.set_supports_action(false);
        supported_traits_.set_visual_min_temperature(MIN_TEMP_SETPOINT);
        supported_traits_.set_visual_max_temperature(MAX_TEMP_SETPOINT);
        supported_traits_.set_visual_current_temperature_step(0.5);
        supported_traits_.set_visual_target_temperature_step(0.5);

        // Only override defaults if the capabilities are known
        if (nvs_storage_.capabilities_message[0] != 0) {
            // Configure the climate traits
            std::set<climate::ClimateMode> device_modes;
            device_modes.insert(climate::CLIMATE_MODE_OFF);
            device_modes.insert(climate::CLIMATE_MODE_COOL);
            if (parse_capability(LgCapability::MODE_HEATING))
                device_modes.insert(climate::CLIMATE_MODE_HEAT);
            if (parse_capability(LgCapability::MODE_FAN))
                device_modes.insert(climate::CLIMATE_MODE_FAN_ONLY);
            if (parse_capability(LgCapability::MODE_AUTO))
                device_modes.insert(climate::CLIMATE_MODE_HEAT_COOL);
            if (parse_capability(LgCapability::MODE_DEHUMIDIFY))
                device_modes.insert(climate::CLIMATE_MODE_DRY);
            supported_traits_.set_supported_modes(device_modes);
            
            std::set<climate::ClimateFanMode> fan_modes;
            if (parse_capability(LgCapability::FAN_AUTO))
                fan_modes.insert(climate::CLIMATE_FAN_AUTO);
            if (parse_capability(LgCapability::FAN_SLOW))
                fan_modes.insert(climate::CLIMATE_FAN_QUIET);
            if (parse_capability(LgCapability::FAN_LOW))
                fan_modes.insert(climate::CLIMATE_FAN_LOW);
            if (parse_capability(LgCapability::FAN_MEDIUM))
                fan_modes.insert(climate::CLIMATE_FAN_MEDIUM);
            if (parse_capability(LgCapability::FAN_HIGH))
                fan_modes.insert(climate::CLIMATE_FAN_HIGH);
            supported_traits_.set_supported_fan_modes(fan_modes);

            std::set<climate::ClimateSwingMode> swing_modes;
            swing_modes.insert(climate::CLIMATE_SWING_OFF);
            if (parse_capability(LgCapability::VERTICAL_SWING) && parse_capability(LgCapability::HORIZONTAL_SWING))
                swing_modes.insert(climate::CLIMATE_SWING_BOTH);
            if (parse_capability(LgCapability::VERTICAL_SWING))
                swing_modes.insert(climate::CLIMATE_SWING_VERTICAL);
            if (parse_capability(LgCapability::HORIZONTAL_SWING))
                swing_modes.insert(climate::CLIMATE_SWING_HORIZONTAL);
            supported_traits_.set_supported_swing_modes(swing_modes);
            

            // Disable unsupported entities
            vane_select_1_->set_internal(true);
            vane_select_2_->set_internal(true);
            vane_select_3_->set_internal(true);
            vane_select_4_->set_internal(true);

            if (parse_capability(LgCapability::HAS_ONE_VANE)) {
                vane_select_1_->set_internal(false);
            }
            else if (parse_capability(LgCapability::HAS_TWO_VANES)) {
                vane_select_1_->set_internal(false);
                vane_select_2_->set_internal(false);
            }
            else if (parse_capability(LgCapability::HAS_FOUR_VANES)) {
                vane_select_1_->set_internal(false);
                vane_select_2_->set_internal(false);
                vane_select_3_->set_internal(false);
                vane_select_4_->set_internal(false);
            }

            fan_speed_slow_->set_internal(true);
            fan_speed_low_->set_internal(true);
            fan_speed_medium_->set_internal(true);
            fan_speed_high_->set_internal(true);

            if (parse_capability(LgCapability::HAS_ESP_VALUE_SETTING)) {
                if (parse_capability(LgCapability::FAN_SLOW)) {
                    fan_speed_slow_->set_internal(false);
                }
                if (parse_capability(LgCapability::FAN_LOW)) {
                    fan_speed_low_->set_internal(false);
                }
                if (parse_capability(LgCapability::FAN_MEDIUM)) {
                    fan_speed_medium_->set_internal(false);
                }
                if (parse_capability(LgCapability::FAN_HIGH)) {
                    fan_speed_high_->set_internal(false);
                }
            }

            purifier_.set_internal(!parse_capability(LgCapability::PURIFIER));        
        }
    }  

public:
    LgController(esphome::uart::UARTComponent* serial,
                 esphome::sensor::Sensor* temperature_sensor,
                 esphome::template_::TemplateSelect* vane_select_1, 
                 esphome::template_::TemplateSelect* vane_select_2, 
                 esphome::template_::TemplateSelect* vane_select_3, 
                 esphome::template_::TemplateSelect* vane_select_4,
                 esphome::template_::TemplateNumber* fan_speed_slow,
                 esphome::template_::TemplateNumber* fan_speed_low,
                 esphome::template_::TemplateNumber* fan_speed_medium,
                 esphome::template_::TemplateNumber* fan_speed_high
                 )
      : serial_(serial),
        temperature_sensor_(temperature_sensor),
        vane_select_1_(vane_select_1),
        vane_select_2_(vane_select_2),
        vane_select_3_(vane_select_3),
        vane_select_4_(vane_select_4),
        fan_speed_slow_(fan_speed_slow),
        fan_speed_low_(fan_speed_low),
        fan_speed_medium_(fan_speed_medium),
        fan_speed_high_(fan_speed_high),
        purifier_(this),
        internal_thermistor_(this)
    {
    }

    float get_setup_priority() const override {
        return esphome::setup_priority::BUS;
    }

    void setup() override {
        // Load our custom NVS storage to get the capabilities message
        ESPPreferenceObject pref = global_preferences->make_preference<NVSStorage>(this->get_object_id_hash() ^ NVS_STORAGE_VERSION);
        pref.load(&nvs_storage_);

        auto restore = this->restore_state_();
        if (restore.has_value()) {
            restore->apply(this);
        } else {
            this->mode = climate::CLIMATE_MODE_OFF;
            this->target_temperature = 20;
            this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
            this->swing_mode = climate::CLIMATE_SWING_OFF;
            this->publish_state();
        }

        error_code_.set_icon("mdi:alert-circle-outline");
        defrost_.set_icon("mdi:snowflake-melt");
        preheat_.set_icon("mdi:heat-wave");
        outdoor_.set_icon("mdi:fan");
        purifier_.set_icon("mdi:pine-tree");

        internal_thermistor_.set_icon("mdi:thermometer");
        internal_thermistor_.restore_and_set_mode(esphome::switch_::SWITCH_RESTORE_DEFAULT_OFF);


        // Configure climate traits and entities based on the capabilities message (if available)
        configure_capabilities();
        

        while (serial_->available() > 0) {
            uint8_t b;
            serial_->read_byte(&b);
        }
        set_changed();

        // Call `update` every 6 seconds, but first wait 10 seconds.
        set_timeout("initial_send", 10000, [this]() {
            set_interval("update", 6000, [this]() { update(); });
        });
    }

    // Process changes from HA.
    void control(const climate::ClimateCall &call) override {
        if (call.get_mode().has_value()) {
            this->mode = *call.get_mode();
        }
        if (call.get_target_temperature().has_value()) {
            this->target_temperature = *call.get_target_temperature();
        }
        if (call.get_fan_mode().has_value()) {
            this->fan_mode = *call.get_fan_mode();
        }
        if (call.get_swing_mode().has_value()) {
            set_swing_mode(*call.get_swing_mode());
        }
        this->set_changed();
        this->publish_state();
    }

    climate::ClimateTraits traits() override {
        return supported_traits_;
    }

    void set_changed() {
        pending_status_change_ = true;
    }

    // Sets position of vane index (1-4) to position (0-6).
    void set_vane_position(int index, int position) {
        if (index < 1 || index > 4) {
            ESP_LOGE(TAG, "Unexpected vane index: %d", index);
            return;
        }
        if (position < 0 || position > 6) {
            ESP_LOGE(TAG, "Unexpected vane position: %d", position);
            return;
        }
        if (vane_position_[index-1] == position) {
            return;
        }
        ESP_LOGD(TAG, "Setting vane %d position: %d", index, position);
        vane_position_[index-1] = position;
        if (!is_initializing_) {
            pending_type_a_settings_change_ = true;
        }
    }

    // Sets installer setting fan speed index (0-3 for slow-high) to value (0-255), with "0" being the factory default
    void set_fan_speed(int index, int value) {
        if (index < 0 || index > 3) {
            ESP_LOGE(TAG, "Unexpected fan speed index: %d", index);
            return;
        }
        if (value<0 || value>255) {
            ESP_LOGE(TAG, "Unexpected fan speed: %d", value);
            return;
        }

        if (fan_speed_[index] == value) {
            return;
        }

        fan_speed_[index] = value;
        if (!is_initializing_) {
            pending_type_a_settings_change_ = true;
        }
    }

    InstallerSettings& installer_settings() {
        return installer_settings_;
    }

    // Note: sensors and switches returned here must match the names in base.yaml.
    std::vector<Sensor*> get_sensors() {
        return {
            &error_code_,
        };
    }
    std::vector<BinarySensor*> get_binary_sensors() {
        return {
            &defrost_,
            &preheat_,
            &outdoor_,
        };
    }
    std::vector<Switch*> get_switches() {
        return {
            &purifier_,
            &internal_thermistor_,
        };
    }

private:
    optional<float> get_room_temp() const {
        float temp = temperature_sensor_->get_state();
        if (isnan(temp) || temp == 0) {
            return {};
        }
        if (temp < 11) {
            return 11;
        }
        if (temp > 35) {
            return 35;
        }
        // Round to nearest 0.5 degrees.
        return round(temp * 2) / 2;
    }

    static uint8_t calc_checksum(const uint8_t* buffer) {
        size_t result = 0;
        for (size_t i = 0; i < 12; i++) {
            result += buffer[i];
        }
        return (result & 0xff) ^ 0x55;
    }

    void set_swing_mode(ClimateSwingMode mode) {
        if (this->swing_mode != mode) {
            // If vertical swing is off, send a 0xAA message to restore the vane position.
            if (mode == climate::CLIMATE_SWING_OFF || mode == climate::CLIMATE_SWING_HORIZONTAL) {
                pending_type_a_settings_change_ = true;
            }
        }
        this->swing_mode = mode;
    }

    void send_status_message() {
        // Byte 0: status message (0x8) from the controller (0xA0).
        send_buf_[0] = 0xA8;

        // Byte 1: changed flag (0x1), power on (0x2), mode (0x1C), fan speed (0x70).
        uint8_t b = 0;
        if (pending_status_change_) {
            b |= 0x1;
        }
        switch (this->mode) {
            case climate::CLIMATE_MODE_COOL:
                b |= (0 << 2) | 0x2;
                break;
            case climate::CLIMATE_MODE_DRY:
                b |= (1 << 2) | 0x2;
                break;
            case climate::CLIMATE_MODE_FAN_ONLY:
                b |= (2 << 2) | 0x2;
                break;
            case climate::CLIMATE_MODE_HEAT_COOL:
                b |= (3 << 2) | 0x2;
                break;
            case climate::CLIMATE_MODE_HEAT:
                b |= (4 << 2) | 0x2;
                break;
            case climate::CLIMATE_MODE_OFF:
                b |= (2 << 2);
                break;
            default:
                ESP_LOGE(TAG, "unknown operation mode, turning off");
                b |= (2 << 2);
                break;
        }
        switch (*this->fan_mode) {
            case climate::CLIMATE_FAN_LOW:
                b |= 0 << 5;
                break;
            case climate::CLIMATE_FAN_MEDIUM:
                b |= 1 << 5;
                break;
            case climate::CLIMATE_FAN_HIGH:
                b |= 2 << 5;
                break;
            case climate::CLIMATE_FAN_AUTO:
                b |= 3 << 5;
                break;
            case climate::CLIMATE_FAN_QUIET:
                b |= 4 << 5;
                break;
            default:
                ESP_LOGE(TAG, "unknown fan mode, using Medium");
                b |= 1 << 5;
                break;
        }
        send_buf_[1] = b;

        // Byte 2: swing mode and purifier/plasma setting. Preserve the other bits.
        b = last_recv_status_[2] & ~(0x4|0x40|0x80);
        if (purifier_.state) {
            b |= 0x4;
        }
        switch (this->swing_mode) {
            case climate::CLIMATE_SWING_OFF:
                break;
            case climate::CLIMATE_SWING_HORIZONTAL:
                b |= 0x40;
                break;
            case climate::CLIMATE_SWING_VERTICAL:
                b |= 0x80;
                break;
            case climate::CLIMATE_SWING_BOTH:
                b |= 0x40 | 0x80;
                break;
            default:
                ESP_LOGE(TAG, "unknown swing mode");
                break;
        }
        send_buf_[2] = b;

        // Bytes 3-4.
        send_buf_[3] = last_recv_status_[3];
        send_buf_[4] = last_recv_status_[4];

        float target = this->target_temperature;
        if (target < MIN_TEMP_SETPOINT) {
            target = MIN_TEMP_SETPOINT;
        } else if (target > MAX_TEMP_SETPOINT) {
            target = MAX_TEMP_SETPOINT;
        }

        // Byte 5. Unchanged except for the low bit which indicates the target temperature has a
        // 0.5 fractional part.
        send_buf_[5] = last_recv_status_[5] & ~0x1;
        if (target - uint8_t(target) == 0.5) {
            send_buf_[5] |= 0x1;
        }

        // Byte 6: thermistor setting and target temperature (fractional part in byte 5).
        // Byte 7: room temperature. Preserve the (unknown) upper two bits.
        enum ThermistorSetting { Unit = 0, Controller = 1, TwoTH = 2 };
        ThermistorSetting thermistor =
            internal_thermistor_.state ? ThermistorSetting::Unit : ThermistorSetting::Controller;
        float temp;
        if (auto maybe_temp = get_room_temp()) {
            temp = *maybe_temp;
        } else {
            // Room temperature isn't available. Use the unit's thermistor and send something
            // reasonable.
            thermistor = ThermistorSetting::Unit;
            temp = 20;
        }
        send_buf_[6] = (thermistor << 4) | ((uint8_t(target) - 15) & 0xf);
        send_buf_[7] = (last_recv_status_[7] & 0xC0) | uint8_t((temp - 10) * 2);

        // Byte 8. Request settings when controller turns on.
        if (is_initializing_) {
            send_buf_[8] = 0x40;
        } else {
            send_buf_[8] = last_recv_status_[8];
        }

        // Bytes 9-11.
        send_buf_[9] = last_recv_status_[9];
        send_buf_[10] = last_recv_status_[10];
        send_buf_[11] = last_recv_status_[11];

        // Byte 12.
        send_buf_[12] = calc_checksum(send_buf_);

        ESP_LOGD(TAG, "sending %s", format_hex_pretty(send_buf_, MsgLen).c_str());
        serial_->write_array(send_buf_, MsgLen);

        pending_status_change_ = false;
        pending_send_ = PendingSendKind::Status;
        last_sent_status_millis_ = millis();

        // If we sent an updated temperature to the AC, update temperature in HA too.
        if (thermistor == ThermistorSetting::Controller && this->current_temperature != temp) {
            this->current_temperature = temp;
            publish_state();
        }
    }

    void send_type_a_settings_message() {
        if (last_recv_type_a_settings_[0] != 0xCA) {
            ESP_LOGE(TAG, "Unexpected missing CA message");
            pending_type_a_settings_change_ = false;
            return;
        }

        // Copy settings from the CA message we received.
        memcpy(send_buf_, last_recv_type_a_settings_, MsgLen);
        send_buf_[0] = 0xAA;

        // Bytes 2-6 store the installer fan speeds
        send_buf_[2] = fan_speed_[0];
        send_buf_[3] = fan_speed_[1];
        send_buf_[4] = fan_speed_[2];
        send_buf_[5] = fan_speed_[3];

        // Bytes 7-8 store vane positions.
        send_buf_[7] = (send_buf_[7] & 0xf0) | (vane_position_[0] & 0x0f); // Set vane 1
        send_buf_[7] = (send_buf_[7] & 0x0f) | ((vane_position_[1] & 0x0f) << 4); // Set vane 2
        send_buf_[8] = (send_buf_[8] & 0xf0) | (vane_position_[2] & 0x0f); // Set vane 3
        send_buf_[8] = (send_buf_[8] & 0x0f) | ((vane_position_[3] & 0x0f) << 4); // Set vane 4


        send_buf_[12] = calc_checksum(send_buf_);

        ESP_LOGD(TAG, "sending %s", format_hex_pretty(send_buf_, MsgLen).c_str());
        serial_->write_array(send_buf_, MsgLen);

        pending_type_a_settings_change_ = false;
        pending_send_ = PendingSendKind::TypeA;
    }

    void send_type_b_settings_message() {
        if (last_recv_type_b_settings_[0] != 0xCB) {
            ESP_LOGE(TAG, "Unexpected missing CB message");
            pending_type_b_settings_change_ = false;
            return;
        }

        // Copy settings from the CB message we received.
        memcpy(send_buf_, last_recv_type_b_settings_, MsgLen);
        send_buf_[0] = 0xAB;

        // Byte 2 stores installer setting 15. Set to value from the YAML file.
        send_buf_[2] = (send_buf_[2] & 0xC7) | ((installer_settings_.over_heating & 0xf) << 3);

        send_buf_[12] = calc_checksum(send_buf_);

        ESP_LOGD(TAG, "sending %s", format_hex_pretty(send_buf_, MsgLen).c_str());
        serial_->write_array(send_buf_, MsgLen);

        pending_type_b_settings_change_ = false;
        pending_send_ = PendingSendKind::TypeB;
    }

    void process_message(const uint8_t* buffer, bool* had_error) {
        ESP_LOGD(TAG, "received %s", format_hex_pretty(buffer, MsgLen).c_str());

        if (calc_checksum(buffer) != buffer[12]) {
            // When initializing, the unit sends an all-zeroes message as padding between
            // messages. Ignore those false checksum failures.
            auto is_zero = [](uint8_t b) { return b == 0; };
            if (std::all_of(buffer, buffer + MsgLen, is_zero)) {
                ESP_LOGD(TAG, "Ignoring padding message sent by unit");
                return;
            }
            ESP_LOGE(TAG, "invalid checksum %s", format_hex_pretty(buffer, MsgLen).c_str());
            *had_error = true;
            return;
        }

        if (pending_send_ != PendingSendKind::None && memcmp(send_buf_, buffer, MsgLen) == 0) {
            ESP_LOGD(TAG, "verified send");
            pending_send_ = PendingSendKind::None;
            return;
        }

        // We're only interested in status or settings messages from the AC.
        if (buffer[0] == 0xC8) {
            process_status_message(buffer, had_error);
            return;
        }
        if (buffer[0] == 0xCA) {
            process_type_a_settings_message(buffer);
            return;
        }
        if (buffer[0] == 0xCB) {
            process_type_b_settings_message(buffer);
            return;
        }
        if (buffer[0] == 0xC9) {
            // Capabilities message. The unit sends this with the other settings so we're now
            // initialized.

            // Check if we need to update the capabilities message.
            if (nvs_storage_.capabilities_message[0] == 0 || 
                std::memcmp(nvs_storage_.capabilities_message, buffer, MsgLen - 1) != 0) {
                
                bool needsRestart = (nvs_storage_.capabilities_message[0] == 0);

                ESPPreferenceObject pref = global_preferences->make_preference<NVSStorage>(this->get_object_id_hash() ^ NVS_STORAGE_VERSION);
                memcpy(nvs_storage_.capabilities_message, buffer, MsgLen);

                // If no capabilities were stored in NVS before, restart to make sure we get the correct traits for climate
                // No restart just on changes (which is unlikely anyway) to make sure we don't end up in a bootloop for faulty devices / communication
                if (needsRestart) {
                    pref.save(&nvs_storage_);
                    global_preferences->sync();
                    ESP_LOGD(TAG, "restarting to apply initial capabilities");
                    ESP.restart();
                }
                else {
                    ESP_LOGD(TAG, "updated device capabilities, manual restart required to take effect");
                }
            }

            is_initializing_ = false;
            return;
        }
    }

    void process_status_message(const uint8_t* buffer, bool* had_error) {
        // If we just had a failure, ignore this messsage because it might be invalid too.
        if (*had_error) {
            ESP_LOGE(TAG, "ignoring due to previous error %s",
                     format_hex_pretty(buffer, MsgLen).c_str());
            return;
        }

        // Handle simple input sensors first. These are safe to update even if we have a pending
        // change.

        defrost_.publish_state(buffer[3] & 0x4);
        preheat_.publish_state(buffer[3] & 0x8);
        error_code_.publish_state(buffer[11]);

        // When turning on the outdoor unit, the AC sometimes reports ON => OFF => ON within a
        // few seconds. No big deal but it causes noisy state changes in HA. Only report OFF if
        // the last state change was at least 8 seconds ago.
        bool outdoor_on = buffer[5] & 0x4;
        bool outdoor_changed = outdoor_.state != outdoor_on;
        if (outdoor_on) {
            outdoor_.publish_state(true);
        } else if (millis() - last_outdoor_change_millis_ > 8000) {
            outdoor_.publish_state(false);
        }
        if (outdoor_changed) {
            last_outdoor_change_millis_ = millis();
        }

        // Report the unit's room temperature only if we're using the internal thermistor. With an
        // external temperature sensor, some units report the temperature we sent and others
        // always send the internal temperature.
        if (internal_thermistor_.state) {
            float unit_temp = float(buffer[7] & 0x3F) / 2 + 10;
            if (this->current_temperature != unit_temp) {
                this->current_temperature = unit_temp;
                publish_state();
            }
        }

        // Don't update our settings if we have a pending change/send, because else we overwrite
        // changes we still have to send (or are sending) to the AC.
        if (pending_status_change_) {
            ESP_LOGD(TAG, "ignoring because pending change");
            return;
        }
        if (pending_send_ == PendingSendKind::Status) {
            ESP_LOGD(TAG, "ignoring because pending send");
            return;
        }

        memcpy(last_recv_status_, buffer, MsgLen);

        uint8_t b = buffer[1];
        if ((b & 0x2) == 0) {
            this->mode = climate::CLIMATE_MODE_OFF;
        } else {
            uint8_t mode_val = (b >> 2) & 0b111;
            switch (mode_val) {
                case 0:
                    this->mode = climate::CLIMATE_MODE_COOL;
                    break;
                case 1:
                    this->mode = climate::CLIMATE_MODE_DRY;
                    break;
                case 2:
                    this->mode = climate::CLIMATE_MODE_FAN_ONLY;
                    break;
                case 3:
                    this->mode = climate::CLIMATE_MODE_HEAT_COOL;
                    break;
                case 4:
                    this->mode = climate::CLIMATE_MODE_HEAT;
                    break;
                default:
                    ESP_LOGE(TAG, "received invalid operation mode from AC (%u)", mode_val);
                    *had_error = true;
                    return;
            }
        }

        uint8_t fan_val = b >> 5;
        switch (fan_val) {
            case 0:
                this->fan_mode = climate::CLIMATE_FAN_LOW;
                break;
            case 1:
                this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
                break;
            case 2:
                this->fan_mode = climate::CLIMATE_FAN_HIGH;
                break;
            case 3:
                this->fan_mode = climate::CLIMATE_FAN_AUTO;
                break;
            case 4:
                this->fan_mode = climate::CLIMATE_FAN_QUIET;
                break;
            default:
                ESP_LOGE(TAG, "received unexpected fan mode from AC (%u)", fan_val);
                *had_error = true;
                return;
        }

        purifier_.publish_state(buffer[2] & 0x4);

        bool horiz_swing = buffer[2] & 0x40;
        bool vert_swing = buffer[2] & 0x80;
        if (horiz_swing && vert_swing) {
            set_swing_mode(climate::CLIMATE_SWING_BOTH);
        } else if (horiz_swing) {
            set_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
        } else if (vert_swing) {
            set_swing_mode(climate::CLIMATE_SWING_VERTICAL);
        } else {
            set_swing_mode(climate::CLIMATE_SWING_OFF);
        }

        this->target_temperature = float((buffer[6] & 0xf) + 15);
        if (buffer[5] & 0x1) {
            this->target_temperature += 0.5;
        }

        publish_state();
    }

    void process_type_a_settings_message(const uint8_t* buffer) {
        // Send settings the first time we receive a 0xCA message.
        bool first_time = last_recv_type_a_settings_[0] == 0;
        memcpy(last_recv_type_a_settings_, buffer, MsgLen);
        if (first_time) {
            pending_type_a_settings_change_ = true;
        }

        // Handle vane 1 position change
        uint8_t vane1 = buffer[7] & 0x0F;
        if (vane1 <= 6) {
            vane_position_[0] = vane1;
            vane_select_1_->publish_state(*vane_select_1_->at(vane1));
        } else {
            ESP_LOGE(TAG, "Unexpected vane 1 position: %u", vane1);
        }

        // Handle vane 2 position change
        uint8_t vane2 = (buffer[7] >> 4) & 0x0F;
        if (vane2 <= 6) {
            vane_position_[1] = vane2;
            vane_select_2_->publish_state(*vane_select_2_->at(vane2));
        } else {
            ESP_LOGE(TAG, "Unexpected vane 2 position: %u", vane2);
        }

        // Handle vane 3 position change
        uint8_t vane3 = buffer[8] & 0x0F;
        if (vane3 <= 6) {
            vane_position_[2] = vane3;
            vane_select_3_->publish_state(*vane_select_3_->at(vane3));
        } else {
            ESP_LOGE(TAG, "Unexpected vane 3 position: %u", vane3);
        }

        // Handle vane 4 position change
        uint8_t vane4 = (buffer[8] >> 4) & 0x0F;
        if (vane4 <= 6) {
            vane_position_[3] = vane4;
            vane_select_4_->publish_state(*vane_select_4_->at(vane4));
        } else {
            ESP_LOGE(TAG, "Unexpected vane 4 position: %u", vane4);
        }

        // Handle fan speed 0 (slow) change
        fan_speed_[0] = buffer[2];
        fan_speed_slow_->publish_state(fan_speed_[0]);
        
        // Handle fan speed 1 (low) change
        fan_speed_[1] = buffer[3];
        fan_speed_low_->publish_state(fan_speed_[1]);
        
        // Handle fan speed 2 (medium) change
        fan_speed_[2] = buffer[4];
        fan_speed_medium_->publish_state(fan_speed_[2]);

        // Handle fan speed 3 (high) change
        fan_speed_[3] = buffer[5];
        fan_speed_high_->publish_state(fan_speed_[3]);

    }

    void process_type_b_settings_message(const uint8_t* buffer) {
        // Send installer settings the first time we receive a 0xCB message.
        bool first_time = last_recv_type_b_settings_[0] == 0;
        memcpy(last_recv_type_b_settings_, buffer, MsgLen);
        if (first_time) {
            pending_type_b_settings_change_ = true;
        }
    }

    void update() {
        ESP_LOGD(TAG, "update");

        bool had_error = false;
        while (serial_->available() > 0) {
            if (!serial_->read_byte(&recv_buf_[recv_buf_len_])) {
                break;
            }
            last_recv_millis_ = millis();
            recv_buf_len_++;
            if (recv_buf_len_ == MsgLen) {
                process_message(recv_buf_, &had_error);
                recv_buf_len_ = 0;
            }
        }

        // If we did not receive the message we sent last time, try to send it again next time.
        // Ignore this when we're initializing because the unit then immediately responds by
        // sending a lot of messages and this introduces a delay.
        if (pending_send_ != PendingSendKind::None && !is_initializing_) {
            ESP_LOGE(TAG, "did not receive message we just sent");
            switch (pending_send_) {
                case PendingSendKind::Status:
                    pending_status_change_ = true;
                    break;
                case PendingSendKind::TypeA:
                    pending_type_a_settings_change_ = true;
                    break;
                case PendingSendKind::TypeB:
                    pending_type_b_settings_change_ = true;
                    break;
                case PendingSendKind::None:
                    ESP_LOGE(TAG, "unreachable");
                    break;
            }
            pending_send_ = PendingSendKind::None;
            return;
        }

        if (recv_buf_len_ > 0) {
            if (millis() - last_recv_millis_ > 15 * 1000) {
                ESP_LOGE(TAG, "discarding incomplete data %s",
                         format_hex_pretty(recv_buf_, recv_buf_len_).c_str());
                recv_buf_len_ = 0;
            }
            return;
        }

        if (had_error) {
            return;
        }

        // Send a status message every 20 seconds, or now if we have a pending change.
        if (!pending_status_change_ &&
            !pending_type_a_settings_change_ &&
            !pending_type_b_settings_change_ &&
            millis() - last_sent_status_millis_ < 20 * 1000) {
            return;
        }

        // Make sure the RX pin is idle for at least 500 ms to avoid collisions on the bus as much
        // as possible. If there is still a collision, we'll likely both start sending at
        // approximately the same time and the message will hopefully be corrupt (and ignored)
        // anyway. Else the pending_send_/send_buf_ mechanism should catch it and we try again.
        //
        // Note: using digitalRead is *much* better for this than using serial_ because that
        // interface has significant delays. It has to wait for a full byte to arrive and this
        // takes about 9-10 ms with our slow baud rate. There are also various buffers and
        // timeouts before incoming bytes reach us.
        //
        // 500 ms might be overkill, but the device usually sends the same message twice with a
        // short delay (about 200 ms?) between them so let's not send there either to avoid
        // collisions.
        uint32_t millis_now = millis();
        while (true) {
            if (serial_->available() > 0 || digitalRead(RxPin) == LOW) {
                ESP_LOGD(TAG, "line busy, not sending yet");
                return;
            }
            if (millis() - millis_now > 500) {
                break;
            }
            delay(5);
        }

        if (pending_type_a_settings_change_) {
            send_type_a_settings_message();
        } else if (pending_type_b_settings_change_) {
            send_type_b_settings_message();
        } else {
            send_status_message();
        }
    }
};

void LgSwitch::write_state(bool state) {
    publish_state(state);
    controller_->set_changed();
}
