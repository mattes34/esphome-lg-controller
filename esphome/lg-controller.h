#include "esphome.h"

static const char* const TAG = "lg-controller";

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

class LgController final : public climate::Climate, public Component {
    static constexpr size_t MsgLen = 13;
    static constexpr int RxPin = 26; // Keep in sync with rx_pin in base.yaml.

    esphome::uart::UARTComponent* serial_;
    esphome::sensor::Sensor* temperature_sensor_;

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
    uint8_t last_recv_status_[MsgLen] = {};

    uint8_t send_buf_[MsgLen] = {};
    uint32_t last_sent_millis_ = 0;
    bool pending_send_ = false;

    bool pending_change_ = false;

public:
    LgController(esphome::uart::UARTComponent* serial,
                 esphome::sensor::Sensor* temperature_sensor)
      : serial_(serial),
        temperature_sensor_(temperature_sensor),
        purifier_(this),
        internal_thermistor_(this)
    {}

    float get_setup_priority() const override {
        return esphome::setup_priority::BUS;
    }

    void setup() override {
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
            this->swing_mode = *call.get_swing_mode();
        }
        this->set_changed();
        this->publish_state();
    }

    climate::ClimateTraits traits() override {
        climate::ClimateTraits traits;
        traits.set_supported_modes({
            climate::CLIMATE_MODE_OFF,
            climate::CLIMATE_MODE_COOL,
            climate::CLIMATE_MODE_HEAT,
            climate::CLIMATE_MODE_DRY,
            climate::CLIMATE_MODE_FAN_ONLY,
            climate::CLIMATE_MODE_HEAT_COOL,
        });
        traits.set_supported_fan_modes({
            climate::CLIMATE_FAN_LOW,
            climate::CLIMATE_FAN_MEDIUM,
            climate::CLIMATE_FAN_HIGH,
            climate::CLIMATE_FAN_AUTO,
        });
        traits.set_supported_swing_modes({
            climate::CLIMATE_SWING_OFF,
            climate::CLIMATE_SWING_BOTH,
            climate::CLIMATE_SWING_VERTICAL,
            climate::CLIMATE_SWING_HORIZONTAL,
        });
        traits.set_supports_current_temperature(true);
        traits.set_supports_two_point_target_temperature(false);
        traits.set_supports_action(false);
        traits.set_visual_min_temperature(18);
        traits.set_visual_max_temperature(30);
        traits.set_visual_current_temperature_step(0.5);
        traits.set_visual_target_temperature_step(0.5);
        return traits;
    }

    void set_changed() {
        pending_change_ = true;
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

    void send_status_message() {
        // Byte 0: status message (0x8) from the controller (0xA0).
        send_buf_[0] = 0xA8;

        // Byte 1: changed flag (0x1), power on (0x2), mode (0x1C), fan speed (0x70).
        uint8_t b = 0;
        if (pending_change_) {
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
        if (target < 18) {
            target = 18;
        } else if (target > 30) {
            target = 30;
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

        // Bytes 8-11.
        send_buf_[8] = last_recv_status_[8];
        send_buf_[9] = last_recv_status_[9];
        send_buf_[10] = last_recv_status_[10];
        send_buf_[11] = last_recv_status_[11];

        // Byte 12.
        send_buf_[12] = calc_checksum(send_buf_);

        ESP_LOGD(TAG, "sending %s", format_hex_pretty(send_buf_, MsgLen).c_str());
        serial_->write_array(send_buf_, MsgLen);

        pending_change_ = false;
        pending_send_ = true;
        last_sent_millis_ = millis();

        // If we sent an updated temperature to the AC, update temperature in HA too instead of
        // waiting for the next status message from the AC (it can take up to a minute).
        if (thermistor == ThermistorSetting::Controller && this->current_temperature != temp) {
            this->current_temperature = temp;
            publish_state();
        }
    }

    void process_message(const uint8_t* buffer, bool* had_error) {
        ESP_LOGD(TAG, "received %s", format_hex_pretty(buffer, MsgLen).c_str());

        if (calc_checksum(buffer) != buffer[12]) {
            ESP_LOGE(TAG, "invalid checksum %s", format_hex_pretty(buffer, MsgLen).c_str());
            *had_error = true;
            return;
        }

        if (pending_send_ && memcmp(send_buf_, buffer, MsgLen) == 0) {
            ESP_LOGD(TAG, "verified send");
            pending_send_ = false;
            return;
        }

        // We're only interested in status messages (0x8) from the AC (0xC0).
        if (buffer[0] != 0xC8) {
            return;
        }

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

        float unit_temp = float(buffer[7] & 0x3F) / 2 + 10;
        if (this->current_temperature != unit_temp) {
            this->current_temperature = unit_temp;
            publish_state();
        }

        // Don't update our settings if we have a pending change/send, because else we overwrite
        // changes we still have to send (or are sending) to the AC.
        if (pending_change_) {
            ESP_LOGD(TAG, "ignoring because pending change");
            return;
        }
        if (pending_send_) {
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
            default:
                ESP_LOGE(TAG, "received unexpected fan mode from AC (%u)", fan_val);
                *had_error = true;
                return;
        }

        purifier_.publish_state(buffer[2] & 0x4);

        bool horiz_swing = buffer[2] & 0x40;
        bool vert_swing = buffer[2] & 0x80;
        if (horiz_swing && vert_swing) {
            this->swing_mode = climate::CLIMATE_SWING_BOTH;
        } else if (horiz_swing) {
            this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
        } else if (vert_swing) {
            this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
        } else {
            this->swing_mode = climate::CLIMATE_SWING_OFF;
        }

        this->target_temperature = float((buffer[6] & 0xf) + 15);
        if (buffer[5] & 0x1) {
            this->target_temperature += 0.5;
        }

        publish_state();
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
        if (pending_send_) {
            ESP_LOGE(TAG, "did not receive message we just sent");
            pending_send_ = false;
            pending_change_ = true;
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
        if (!pending_change_ && millis() - last_sent_millis_ < 20 * 1000) {
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

        send_status_message();
    }
};

void LgSwitch::write_state(bool state) {
    publish_state(state);
    controller_->set_changed();
}
