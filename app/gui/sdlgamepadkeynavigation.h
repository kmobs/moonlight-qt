#pragma once

#include <QTimer>
#include <QEvent>
#include <QHash>
#include <QPair>
#include <QPointer>

class QWindow;

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

private:
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                 QWindow* target = nullptr);

    void releaseControllerKeys();

    void updateTimerState();

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    // Popups can change navigation mode during a button press. Its release
    // must still match the key and modifiers sent for that same press.
    struct ControllerKey {
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
        QPointer<QWindow> window;
    };
    QHash<quint64, ControllerKey> m_PressedControllerKeys;
};
