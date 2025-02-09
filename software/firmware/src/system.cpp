#include "system.hpp"

#include "events.hpp"

// #include "accelerometer.hpp"
// #include "buttons.hpp"
#include "compile_time_config.hpp"
#include "fan.hpp"

#include "indicator.hpp"
#include "strobe.hpp"

#include <boost/sml.hpp>
#include <fmt/core.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>

#include <algorithm>
#include <tuple>
#include <utility>


static auto& indicator = indicator_instance();
static auto& strobe    = strobe_instance();
static auto& fan       = fan_instance();


auto uptime_clock::now() noexcept -> time_point {
    return time_point{duration{k_uptime_get()}};
}


void system_power_off() noexcept {
    k_sched_lock();

    std::ignore = irq_lock();
    indicator.set_color(Colors::Cyan);
    strobe.off();
    fan.set_speed(0);

    for (volatile int i = 0; i != 10000000; ++i) {
        // do nothing
    }
    enable_wake_from_buttons();

    indicator.off();
    sys_poweroff();
}


namespace sml = boost::sml;
using namespace events;
struct my_logger {
    template<class SM, class TEvent> void log_process_event(const TEvent&) {
        printk("[%s][process_event] %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TEvent>());
    }

    template<class SM, class TGuard, class TEvent> void log_guard(const TGuard&, const TEvent&, bool result) {
        // printk("[%s][guard] %s %s %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TGuard>(), sml::aux::get_type_name<TEvent>(), (result ? "[OK]" : "[Reject]"));
    }

    template<class SM, class TAction, class TEvent> void log_action(const TAction&, const TEvent&) {
        // printk("[%s][action] %s %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TAction>(), sml::aux::get_type_name<TEvent>());
    }

    template<class SM, class TSrcState, class TDstState> void log_state_change(const TSrcState& src, const TDstState& dst) {
        // printk("[%s][transition] %s -> %s\n", sml::aux::get_type_name<SM>(), src.c_str(), dst.c_str());
    }
};


static auto hold_long_enough_for_power_off = [](both_buttons_pressed const& event) {
    bool right_press_kind     = (button_press_kind::long_press == event.plus.kind) && (button_press_kind::long_press == event.minus.kind);
    bool right_press_duration = (event.plus.press_duration >= config::both_buttons_power_off) && (event.minus.press_duration >= config::both_buttons_power_off);

    return right_press_kind && right_press_duration;
};
static auto hold_long_enough_for_toggle_strobe = [](both_buttons_pressed const& event) {
    bool right_press_kind     = (button_press_kind::long_press == event.plus.kind) && (button_press_kind::long_press == event.minus.kind);
    bool right_press_duration = (event.plus.press_duration >= config::both_buttons_toggle_strobe) && (event.minus.press_duration >= config::both_buttons_toggle_strobe);

    return right_press_kind && right_press_duration && !hold_long_enough_for_power_off(event);
};


auto        wtf                = 0;
static auto increase_fan_speed = [](plus_button_pressed const& event) {
    unsigned change_rate = (event.press_duration / config::button_change_rate_threshold) + 1;

    change_rate = std::min(change_rate, config::button_max_change_rate);
    wtf += change_rate;

    if (wtf < 100) {
        fan.set_speed(wtf);
        indicator.set_color(Colors::Green);
        k_msleep(25);
    } else {
        indicator.set_color(Colors::Magenta);
        wtf = 100;
        fan.set_speed(wtf);
        k_msleep(10);
    }

    indicator.set_color(Colors::Black);
};


static auto decrease_fan_speed = []() {
    wtf--;
    if (wtf > 0) {
        fan.set_speed(wtf);
        indicator.set_color(Colors::Yellow);
        k_msleep(25);
    } else {
        indicator.set_color(Colors::Magenta);

        wtf = 0;
        fan.set_speed(wtf);
        k_msleep(10);
    }
        indicator.set_color(Colors::Black);

};

static auto indicator_startup_sequence() {
    for (auto c : {Colors::Green, Colors::Green, Colors::Green}) {
        indicator.set_color(c);
        k_msleep(50);
        indicator.set_color(Colors::Black);
        k_msleep(50);
    }
}


static auto power_on = []() {
    indicator_startup_sequence();
    fan.set_limits(50, 255);
    fan.set_speed(0);
};


static auto toggle_strobe = []() {
    if (!strobe.is_on()) {
        strobe.on(255, 100ms);
    } else {
        strobe.off();
    }
    k_msleep(1000);
};


// static auto update_signaling_scheme = [](charger_status_changed const& event) { printk("Charger status changed\n"); };


struct system_state {
    auto operator()() const {
        using namespace sml;
        /**
         * Initial state: *initial_state
         * Transition DSL: src_state + event [ guard ] / action = dst_state
         */
        return make_transition_table(
            *"off"_s + event<request_power_on> / power_on = "on"_s,          //
            "on"_s                                        = "manual_mode"_s, //

            "manual_mode"_s + event<plus_button_pressed> / increase_fan_speed,  //
            "manual_mode"_s + event<minus_button_pressed> / decrease_fan_speed, //
                                                                                // "manual_mode"_s + event<minus_button_pressed> [hold_long_enough_for_power_off)] / do_power_off = X, //

            // state<_> + event<charger_status_changed> / update_signaling_scheme,
            state<_> + event<both_buttons_pressed>[hold_long_enough_for_toggle_strobe] / toggle_strobe,               //
            state<_> + event<both_buttons_pressed>[hold_long_enough_for_power_off] / []() { system_power_off(); } = X //
        );
    }
};

static my_logger                                     logger;
static sml::sm<system_state, sml::logger<my_logger>> sm{
    logger,
};


K_MUTEX_DEFINE(fsm_mutex);

template<typename T> void system_process_event(T event) {
    k_mutex_lock(&fsm_mutex, K_FOREVER);
    sm.process_event(event);
    k_mutex_unlock(&fsm_mutex);
}

template void system_process_event(events::request_power_on);
// template void system_process_event(events::charger_status_changed);
template void system_process_event(events::plus_button_pressed);
template void system_process_event(events::minus_button_pressed);
template void system_process_event(events::both_buttons_pressed);
