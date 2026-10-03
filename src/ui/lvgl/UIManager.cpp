#include "UIManager.h"
#include "screens/LvHomeScreen.h"
#include "screens/LvMessagesScreen.h"
#include "screens/LvContactsScreen.h"
#include "screens/LvNodesScreen.h"
#include "screens/LvMessageView.h"
#include "screens/LvSettingsScreen.h"
#include "theme/EinkTheme.h"
#include <Arduino.h>

UIManager::ScreenId UIManager::current_ = UIManager::HOME;
lv_obj_t* UIManager::tabbar_ = nullptr;

void UIManager::begin() {
    EinkTheme::apply();
    createTabBar();
    show(HOME);
}

void UIManager::createTabBar() {
    tabbar_ = lv_btnmatrix_create(lv_layer_top());
    static const char* map[] = {"Home", "Msgs", "Cont", "Nodes", "Set", ""};
    lv_btnmatrix_set_map(tabbar_, map);
    lv_obj_set_size(tabbar_, 320, 36);
    lv_obj_align(tabbar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(tabbar_, onTab, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(tabbar_, lv_color_white(), 0);
    lv_obj_set_style_border_width(tabbar_, 1, 0);
    lv_obj_set_style_border_color(tabbar_, lv_color_black(), 0);
}

void UIManager::onTab(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(obj);
    if (id < SETTINGS) show((ScreenId)id);
    else if (id == 4) show(SETTINGS);
}

void UIManager::show(ScreenId id) {
    current_ = id;
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    switch (id) {
        case HOME:          LvHomeScreen::create(scr); break;
        case MESSAGES:      LvMessagesScreen::create(scr); break;
        case CONTACTS:      LvContactsScreen::create(scr); break;
        case NODES:         LvNodesScreen::create(scr); break;
        case MESSAGE_VIEW:  LvMessageView::create(scr); break;
        case SETTINGS:      LvSettingsScreen::create(scr); break;
        default: break;
    }
    // re-attach tabbar
    if (tabbar_) lv_obj_move_foreground(tabbar_);
}

void UIManager::loop() {
    // nothing extra yet
}