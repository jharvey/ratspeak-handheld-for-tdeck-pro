#pragma once

#include "hal/Keyboard.h"
#include "hal/Trackball.h"
#include "hal/TouchInput.h"
#include "hal/Power.h"
#include "input/PointerInput.h"

class InputManager {
public:
    void begin(Keyboard* kb, Trackball* pointer, TouchInput* touch);
    void setPowerMgr(Power* pm) { _powerMgr = pm; }
    void setTrackballSpeed(uint8_t speed) { _speed = speed < 1 ? 1 : (speed > 5 ? 5 : speed); }
    // A full renderer queue leaves keyboard events in the hardware FIFO while
    // still sampling pointer gestures and wake activity.
    void update(bool dispatchReady = true);
    bool hasKeyEvent() const { return _hasKey; }
    const KeyEvent& getKeyEvent() const { return _keyEvent; }
    bool hadActivity() const { return _activity; }
    bool hadStrongActivity() const { return _strongActivity; }
    bool hadLongPress() const { return _pointerInput.longPress; }
private:
    Keyboard* _kb = nullptr;
    Trackball* _pointer = nullptr;
    TouchInput* _touch = nullptr;
    Power* _powerMgr = nullptr;
    PointerInput _pointerInput;
    KeyEvent _keyEvent;
    bool _hasKey = false, _activity = false, _strongActivity = false;
    uint8_t _speed = 3;
    uint32_t _lastTouchPoll = 0;
};
