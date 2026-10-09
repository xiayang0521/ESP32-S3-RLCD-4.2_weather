// 验证单键 BOOT 手势（单击移动/双击确认/长按返回）导航、保护规则和超时。
#include "web_demo_state.h"
#include <cassert>
#include <iostream>

static int g_time = 1000;

static void click(WebDemoState &s) {
    int t = g_time;
    s.press(0, true, t);
    s.press(0, false, t + 50);
    s.advance(t + 50 + WebDemoState::kDoubleClickGapMs);
    g_time = t + 500;
}

static void double_click(WebDemoState &s) {
    int t = g_time;
    s.press(0, true, t);
    s.press(0, false, t + 50);
    s.press(0, true, t + 100);
    s.press(0, false, t + 150);
    s.advance(t + 200);
    g_time = t + 500;
}

static void long_press(WebDemoState &s) {
    int t = g_time;
    s.press(0, true, t);
    s.advance(t + WebDemoState::kLongPressMs);
    s.press(0, false, t + WebDemoState::kLongPressMs + 50);
    s.advance(t + WebDemoState::kLongPressMs + 400);
    g_time = t + 2000;
}

int main() {
    WebDemoState s;
    click(s); assert(s.page==1);
    double_click(s); assert(s.scene==WebDemoState::Settings);
    click(s); assert(s.primary==1);
    double_click(s); assert(s.secondary);
    double_click(s); assert(s.volume==100);
    double_click(s); assert(s.volume==20);
    long_press(s); assert(!s.secondary);
    long_press(s); assert(s.scene==WebDemoState::Work);
    double_click(s); assert(s.scene==WebDemoState::Settings);
    s.primary=0; s.secondary=true;
    s.scene=WebDemoState::Pages; s.enabled=1; s.selection=0;
    double_click(s); assert(s.enabled==1);
    s.enabled=65; double_click(s); assert(s.enabled==65);
    s.reset(g_time); s.settings(g_time+1); g_time += 500;
    s.advance(g_time + 30000); assert(s.scene==WebDemoState::Work);
    s.settings(g_time); s.primary=3; s.secondary=true; s.selection=2;
    double_click(s); assert(s.confirming);
    double_click(s); assert(s.scene==WebDemoState::Work&&s.enabled==255);
    s.scene=WebDemoState::Diagnostics; s.start_operation("test",g_time,3000);
    s.advance(g_time + 3000); assert(!s.pending&&s.progress==100);
    g_time += 4000;
    long_press(s); assert(s.scene==WebDemoState::Settings&&s.secondary);
    long_press(s); assert(!s.secondary);
    long_press(s); assert(s.scene==WebDemoState::Work);
    s.scene=WebDemoState::Work;
    long_press(s); assert(s.scene==WebDemoState::Work);
    g_time = 200000;
    for(int i=0;i<10000;++i){
        g_time += 1000;
        if(i%3==0) double_click(s);
        else click(s);
        assert(s.page>=0&&s.page<8);assert(s.enabled!=0);assert(s.ordered_page(0)!=6);
    }
    std::cout<<"Web demo navigation and state tests passed\n";
}
