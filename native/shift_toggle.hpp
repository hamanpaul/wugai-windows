#pragma once

namespace boshiamy {

class ShiftToggleTracker {
public:
    void set_enabled(bool value) {
        enabled_ = value;
        if (!enabled_) reset();
    }

    bool enabled() const { return enabled_; }

    void reset() {
        tracked_[0] = false;
        tracked_[1] = false;
        tracked_[2] = false;
        armed_key_ = 0;
    }

    bool pending() const {
        return tracked_[0] || tracked_[1] || tracked_[2];
    }

    void key_down(unsigned key, bool control, bool alt, bool composing, bool repeated = false) {
        (void)composing; // Composition cancellation is handled by the TSF service.
        const int index = shift_index(key);
        if (index >= 0) {
            if (tracked_[index]) return;
            const bool overlap = pending();
            tracked_[index] = true;
            if (overlap || !enabled_ || control || alt || repeated) {
                armed_key_ = 0;
            } else {
                armed_key_ = key;
            }
            return;
        }
        // Any other key press makes this a chord, even when the key is passed through.
        armed_key_ = 0;
    }

    bool would_toggle(unsigned key, bool control, bool alt, bool composing) const {
        (void)composing; // Composition cancellation is handled by the TSF service.
        const int index = shift_index(key);
        return enabled_ && index >= 0 && tracked_[index] &&
               armed_key_ == key && !control && !alt;
    }

    bool key_up(unsigned key, bool control, bool alt, bool composing) {
        (void)composing; // Composition cancellation is handled by the TSF service.
        const int index = shift_index(key);
        if (index < 0) {
            armed_key_ = 0;
            return false;
        }
        if (!tracked_[index]) {
            armed_key_ = 0;
            return false;
        }
        const bool toggle = enabled_ && armed_key_ == key && !control && !alt;
        tracked_[index] = false;
        armed_key_ = 0;
        return toggle;
    }

private:
    static int shift_index(unsigned key) {
        switch (key) {
        case 0x10: return 0; // generic Shift
        case 0xa0: return 1; // left Shift
        case 0xa1: return 2; // right Shift
        default: return -1;
        }
    }

    bool enabled_ = false;
    bool tracked_[3] = {false, false, false};
    unsigned armed_key_ = 0;
};

} // namespace boshiamy
