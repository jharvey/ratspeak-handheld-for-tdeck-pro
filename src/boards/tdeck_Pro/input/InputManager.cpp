#include "InputManager.h"
#include "config/BoardConfig.h"

void InputManager::begin(Keyboard* kb, Trackball* pointer, TouchInput* touch) {
    _kb = kb;
    _pointer = pointer;
    _touch = touch;
}

void InputManager::update(bool dispatchReady) {
    _hasKey = _activity = _strongActivity = false;
    const bool screenOn = !_powerMgr || _powerMgr->isScreenOn();
    const uint32_t now = millis();

    if (_pointer) {
        _pointer->update();
    }

    const bool clickHeld =
        _pointer && TBALL_CLICK >= 0 && digitalRead(TBALL_CLICK) == LOW;

    _pointerInput.update(
        _pointer ? _pointer->lastDeltaX() : 0,
        _pointer ? _pointer->lastDeltaY() : 0,
        clickHeld,
        screenOn,
        now,
        _speed,
        false);

    _activity = _pointerInput.activity;
    _strongActivity = _pointerInput.strongActivity;

    if (_pointerInput.longPress && _kb) {
        _kb->discardPending();
    }

    if (screenOn && dispatchReady && !_pointerInput.longPress && _pointerInput.preferPointer()) {
        _hasKey = _pointerInput.take(_keyEvent);
    }

    if (_kb && !_hasKey && !_pointerInput.longPress && (dispatchReady || !screenOn)) {
        _kb->update();
        if (_kb->hasEvent()) {
            _activity = _strongActivity = true;
            if (screenOn) {
                _keyEvent = _kb->getEvent();
                _hasKey = true;
                _pointerInput.keyboardDelivered();
            } else {
                _kb->discardPending();
            }
        }
    }

    if (!_hasKey && screenOn && dispatchReady && !_pointerInput.longPress) {
        _hasKey = _pointerInput.take(_keyEvent);
    }
    if (_hasKey) {
        _activity = _strongActivity = true;
    }

    if (_touch && now - _lastTouchPoll >= 20) {
        _lastTouchPoll = now;
        _touch->update();
        if (screenOn && _touch->isTouched()) {
            _activity = _strongActivity = true;
        }
    }
}