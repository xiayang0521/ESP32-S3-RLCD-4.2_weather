// 验证单键 BOOT 在无按压、无待判定单击且 GPIO 唤醒可用时进入事件等待。
#include "input_button_wait_policy.h"

#include <assert.h>

int main()
{
    assert(button_task_can_wait_for_edge(true, false, false));
    assert(!button_task_can_wait_for_edge(false, false, false));
    assert(!button_task_can_wait_for_edge(true, true, false));
    assert(!button_task_can_wait_for_edge(true, false, true));

    assert(button_task_poll_delay_ms(true, false, false, false) ==
           kButtonPressedPollMs);
    assert(button_task_poll_delay_ms(false, true, false, false) ==
           kButtonPressedPollMs);
    assert(button_task_poll_delay_ms(false, false, true, false) ==
           kButtonActivePollMs);
    assert(button_task_poll_delay_ms(false, false, false, true) ==
           kButtonLowRefreshIdlePollMs);
    assert(button_task_poll_delay_ms(false, false, false, false) ==
           kButtonIdlePollMs);

    static_assert(kButtonPressedPollMs == 20);
    static_assert(kButtonActivePollMs == 50);
    static_assert(kButtonIdlePollMs == 250);
    static_assert(kButtonLowRefreshIdlePollMs == 500);
    static_assert(kButtonDoubleClickGapMs == 350);

    assert(button_gpio_config_retry_due(1, false));
    assert(button_gpio_config_retry_due(2, false));
    assert(!button_gpio_config_retry_due(3, false));
    assert(!button_gpio_config_retry_due(1, true));
    static_assert(kButtonGpioConfigMaxAttempts == 3);
    static_assert(kButtonGpioConfigRetryDelayMs == 100);
    return 0;
}
