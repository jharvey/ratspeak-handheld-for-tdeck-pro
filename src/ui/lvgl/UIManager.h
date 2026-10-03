#pragma once
#include <lvgl.h>

class UIManager {
public:
    enum ScreenId {
        HOME = 0,
        MESSAGES,
        CONTACTS,
        NODES,
        MESSAGE_VIEW,
        SETTINGS,
        COUNT
    };

    static void begin();
    static void show(ScreenId id);
    static void loop();
    static ScreenId current() { return current_; }

private:
    static ScreenId current_;
    static lv_obj_t* tabbar_;
    static void createTabBar();
    static void onTab(lv_event_t* e);
};