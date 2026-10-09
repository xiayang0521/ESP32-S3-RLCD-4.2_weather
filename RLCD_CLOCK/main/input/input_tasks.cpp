// 处理 BOOT、SEL、BACK 按键输入、页面切换和设置页操作请求。
// 原 KEY（GPIO18）硬件损坏：短按选择迁至 SEL（GPIO2），返回迁至 BACK（GPIO15）。
#include "input_tasks.h"

#include "active_work_page_state_internal.h"
#include "alarm_services.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "battery_runtime_state.h"
#include "input_button_config.h"
#include "input_button_wait_policy.h"
#include "network_diagnostics_state.h"
#include "network_sync_requests.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "runtime_health.h"
#include "task_notification_target.h"
#include "ui_info_page_state.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_feedback.h"
#include "ui_settings_navigation.h"
#include "ui_runtime_schedule.h"
#include "ui_task_notify.h"
#include "ui_work_page_catalog.h"
#include "wifi_portal_state.h"

#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/task.h"

#define BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT "button gpio config failed: %s"
#define BUTTON_GPIO_CONFIG_RETRY_LOG_FORMAT \
    "button gpio config failed: %s; retrying attempt=%u/%u"
#define BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT "button gpio isr service failed: %s; using polling fallback"
#define BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT "button gpio %d isr handler failed: %s; using polling fallback"
#define BUTTON_WAKEUP_FAILED_LOG_FORMAT "button light sleep wakeup failed: %s; using polling fallback"
#define BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT "button edge wakeup ready"
#define BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT "switch work page: %d"
#define BUTTON_SHOW_SETTINGS_LOG_FORMAT "sel button clicked, showing settings page"

namespace {
constexpr int kButtonDebounceMs = 18;
constexpr int kButtonLongPressMs = 1200;
constexpr int kButtonBusyFeedbackMs = 2000;
constexpr uint64_t kBootButtonPinMask = 1ULL << kBootButtonGpio;
constexpr uint64_t kSelButtonPinMask = 1ULL << kSelButtonGpio;
constexpr uint64_t kBackButtonPinMask = 1ULL << kBackButtonGpio;
constexpr uint64_t kButtonInputPinMask =
    kBootButtonPinMask | kSelButtonPinMask | kBackButtonPinMask;
constexpr TickType_t kButtonDebounceTicks = pdMS_TO_TICKS(kButtonDebounceMs);
constexpr TickType_t kButtonLongPressTicks = pdMS_TO_TICKS(kButtonLongPressMs);
constexpr const char *kSettingsBusyFeedbackText = "请等待操作完成";
TaskNotificationTarget s_button_task_target;

static_assert(kButtonLongPressMs > kButtonDebounceMs,
              "button long-press duration must be longer than debounce duration");
static_assert(kBootButtonPinMask != 0, "BOOT button pin mask must not be empty");
static_assert(kSelButtonPinMask != 0, "SEL button pin mask must not be empty");
static_assert(kBackButtonPinMask != 0, "BACK button pin mask must not be empty");
static_assert(kButtonInputPinMask ==
                  (kBootButtonPinMask | kSelButtonPinMask | kBackButtonPinMask),
              "button input pin mask must include BOOT, SEL and BACK");
static_assert(kButtonLongPressTicks > kButtonDebounceTicks,
              "button long-press tick duration must be longer than debounce duration");
static_assert(kButtonGpioConfigMaxAttempts > 1,
              "button GPIO configuration must retain a retry opportunity");
static_assert(kButtonGpioConfigRetryDelayMs > 0,
              "button GPIO configuration retry delay must be positive");
static_assert(kButtonIdlePollMs <= kButtonLowRefreshIdlePollMs,
              "low-refresh fallback polling must not wake more often than idle polling");
static_assert(kButtonActivePollMs <= kButtonIdlePollMs,
              "active button polling must stay at least as responsive as idle polling");
static_assert(kButtonPressedPollMs <= kButtonActivePollMs,
              "pressed button polling must stay at least as responsive as active polling");

bool button_press_is_short(TickType_t held)
{
    return held >= kButtonDebounceTicks &&
           held < kButtonLongPressTicks;
}

bool button_press_is_long(TickType_t held)
{
    return held >= kButtonLongPressTicks;
}

void return_to_system_settings_item(int selection, TickType_t now)
{
    settings_page_request();
    enter_settings_system_item_navigation(selection);
    settings_activity_record(now);
}

void enter_settings_primary_menu(TickType_t now)
{
    info_page_clear();
    settings_page_request();
    enter_settings_primary_navigation();
    settings_activity_record(now);
}

void handle_settings_back_or_busy(TickType_t now)
{
    settings_activity_record(now);
    if (!is_settings_sync_busy() && !ota_flow_active()) {
        handle_settings_key_long();
        return;
    }
    set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
}

void execute_back_action(TickType_t now)
{
    if (settings_page_requested()) {
        handle_settings_back_or_busy(now);
        return;
    }
    if (info_page_requested()) {
        info_page_clear();
        return_to_system_settings_item(kSystemSettingsInfoItem, now);
        return;
    }
    if (network_diag_page_requested()) {
        cancel_network_diagnostics_sync();
        network_diag_page_clear();
        return_to_system_settings_item(kSystemSettingsNetworkDiagItem, now);
    }
}

void IRAM_ATTR notify_button_edge(void *)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (!s_button_task_target.notify_from_isr(&higher_priority_task_woken)) {
        return;
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void disable_button_interrupts()
{
    (void)gpio_set_intr_type(kBootButtonGpio, GPIO_INTR_DISABLE);
    (void)gpio_set_intr_type(kSelButtonGpio, GPIO_INTR_DISABLE);
    (void)gpio_set_intr_type(kBackButtonGpio, GPIO_INTR_DISABLE);
}

void remove_button_isr_handlers(bool boot_registered,
                                bool sel_registered,
                                bool back_registered)
{
    if (boot_registered) {
        (void)gpio_isr_handler_remove(kBootButtonGpio);
    }
    if (sel_registered) {
        (void)gpio_isr_handler_remove(kSelButtonGpio);
    }
    if (back_registered) {
        (void)gpio_isr_handler_remove(kBackButtonGpio);
    }
    disable_button_interrupts();
}

bool configure_button_gpio_with_retry(const gpio_config_t &config)
{
    for (unsigned attempt = 1;
         attempt <= kButtonGpioConfigMaxAttempts;
         ++attempt) {
        const esp_err_t err = gpio_config(&config);
        if (err == ESP_OK) {
            return true;
        }
        if (!button_gpio_config_retry_due(attempt, err == ESP_OK)) {
            ESP_LOGE(TAG,
                     BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT,
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGW(TAG,
                 BUTTON_GPIO_CONFIG_RETRY_LOG_FORMAT,
                 esp_err_to_name(err),
                 static_cast<unsigned>(attempt + 1U),
                 kButtonGpioConfigMaxAttempts);
        vTaskDelay(pdMS_TO_TICKS(kButtonGpioConfigRetryDelayMs));
    }
    return false;
}

bool add_button_isr_handler(gpio_num_t gpio,
                            bool &registered_flag,
                            bool boot_registered,
                            bool sel_registered,
                            bool back_registered)
{
    esp_err_t err = gpio_isr_handler_add(gpio, notify_button_edge, nullptr);
    if (err == ESP_OK) {
        registered_flag = true;
        return true;
    }
    ESP_LOGW(TAG,
             BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT,
             static_cast<int>(gpio),
             esp_err_to_name(err));
    remove_button_isr_handlers(boot_registered, sel_registered, back_registered);
    return false;
}

bool setup_button_edge_wakeup()
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        disable_button_interrupts();
        return false;
    }

    bool boot_registered = false;
    bool sel_registered = false;
    bool back_registered = false;
    if (!add_button_isr_handler(kBootButtonGpio,
                                boot_registered,
                                boot_registered,
                                sel_registered,
                                back_registered)) {
        return false;
    }
    if (!add_button_isr_handler(kSelButtonGpio,
                                sel_registered,
                                boot_registered,
                                sel_registered,
                                back_registered)) {
        return false;
    }
    if (!add_button_isr_handler(kBackButtonGpio,
                                back_registered,
                                boot_registered,
                                sel_registered,
                                back_registered)) {
        return false;
    }

    err = esp_sleep_enable_ext1_wakeup_io(kButtonInputPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, BUTTON_WAKEUP_FAILED_LOG_FORMAT, esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, sel_registered, back_registered);
        return false;
    }

    ESP_LOGI(TAG, BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT);
    return true;
}
} // namespace

void button_task(void *)
{
    gpio_config_t button = {};
    button.intr_type = GPIO_INTR_ANYEDGE;
    button.mode = GPIO_MODE_INPUT;
    button.pin_bit_mask = kButtonInputPinMask;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    if (!configure_button_gpio_with_retry(button)) {
        vTaskDelete(nullptr);
        return;
    }

    s_button_task_target.publish(xTaskGetCurrentTaskHandle());
    const bool edge_wakeup_ready = setup_button_edge_wakeup();
    regular_app_task_mark_ready(RegularAppTaskId::kButton);

    TickType_t boot_pressed_since = 0;
    TickType_t sel_pressed_since = 0;
    TickType_t back_pressed_since = 0;
    bool sel_press_opened_settings = false;
    bool back_handled = false;
    bool boot_press_stopped_alert = false;
    bool sel_press_stopped_alert = false;
    bool back_press_stopped_alert = false;

    for (;;) {
        runtime_health_record_current_task_stack(RegularAppTaskId::kButton);
        TickType_t now = xTaskGetTickCount();
        bool boot_pressed = gpio_get_level(kBootButtonGpio) == 0;
        bool sel_pressed = gpio_get_level(kSelButtonGpio) == 0;
        bool back_pressed = gpio_get_level(kBackButtonGpio) == 0;

        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
                boot_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                           pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
            }
        } else {
            if (boot_pressed_since != 0 && boot_press_stopped_alert) {
                // 提醒音播放期间任意按键只负责停止音频，不继续执行原按键动作。
            } else if (boot_pressed_since != 0 && settings_page_requested()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    settings_activity_record_action(now);
                    notify_ui_task();
                } else {
                    settings_activity_record(now);
                }
            } else if (boot_pressed_since != 0 &&
                       !info_page_requested() &&
                       !network_diag_page_requested() &&
                       !setup_portal_active_load() &&
                       !battery_low_mode_load()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    int next_page = next_enabled_work_page(active_work_page_load());
                    active_work_page_store(next_page);
                    ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, next_page + 1);
                    notify_ui_task();
                }
            }
            boot_pressed_since = 0;
            boot_press_stopped_alert = false;
        }

        if (sel_pressed) {
            if (sel_pressed_since == 0) {
                sel_pressed_since = now;
                sel_press_opened_settings = false;
                sel_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                          pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
                if (!sel_press_stopped_alert &&
                    !settings_page_requested() && !info_page_requested() && !network_diag_page_requested()) {
                    ESP_LOGI(TAG, BUTTON_SHOW_SETTINGS_LOG_FORMAT);
                    enter_settings_primary_menu(now);
                    sel_press_opened_settings = true;
                    notify_ui_task();
                }
            }
        } else {
            if (sel_pressed_since != 0 &&
                !sel_press_stopped_alert &&
                !sel_press_opened_settings && settings_page_requested()) {
                TickType_t held = now - sel_pressed_since;
                if (button_press_is_short(held)) {
                    settings_activity_record(now);
                    if (!is_settings_sync_busy() && !ota_flow_active()) {
                        handle_settings_key_short();
                    } else {
                        set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
                        notify_ui_task();
                    }
                }
            }
            if (sel_pressed_since != 0 && settings_page_requested()) {
                settings_activity_record(now);
            }
            sel_pressed_since = 0;
            sel_press_opened_settings = false;
            sel_press_stopped_alert = false;
        }

        if (back_pressed) {
            if (back_pressed_since == 0) {
                back_pressed_since = now;
                back_handled = false;
                back_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                           pomodoro_stop_alert_from_button();
            } else if (!back_press_stopped_alert &&
                       !back_handled &&
                       now - back_pressed_since >= kButtonDebounceTicks &&
                       (settings_page_requested() ||
                        info_page_requested() ||
                        network_diag_page_requested())) {
                execute_back_action(now);
                back_handled = true;
                notify_ui_task();
            }
        } else {
            back_pressed_since = 0;
            back_handled = false;
            back_press_stopped_alert = false;
        }

        const bool press_tracking_active =
            boot_pressed_since != 0 || sel_pressed_since != 0 || back_pressed_since != 0;
        if (button_task_can_wait_for_edge(edge_wakeup_ready,
                                          boot_pressed,
                                          sel_pressed,
                                          back_pressed,
                                          press_tracking_active)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const bool any_button_pressed = boot_pressed || sel_pressed || back_pressed;
        bool interactive_surface = false;
        bool low_refresh_surface = false;
        if (!any_button_pressed) {
            const int active_page = active_work_page_load();
            const bool low_battery_mode = battery_low_mode_load();
            const UiRuntimeSurfaceSnapshot interactive_surfaces =
                ui_runtime_surface_snapshot_load();
            interactive_surface =
                interactive_surfaces.interactive_surface_requested();
            low_refresh_surface =
                low_battery_mode ||
                (is_work_page_enabled(active_page) &&
                 work_page_uses_low_refresh_idle(active_page));
        }
        const int delay_ms = button_task_poll_delay_ms(any_button_pressed,
                                                       interactive_surface,
                                                       low_refresh_surface);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
