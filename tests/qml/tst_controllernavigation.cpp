#include "gui/sdlgamepadkeynavigation.h"
#include "settings/mappingmanager.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest>
#include <functional>
#include <memory>

// Mapping downloads and display discovery are unrelated to key delivery.
MappingManager::MappingManager() {}
void MappingManager::applyMappings() {}
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class KeyWindow : public QWindow {
public:
    struct Key { QEvent::Type type; int key; Qt::KeyboardModifiers modifiers; };
    QList<Key> keys;
    std::function<void()> onPress;
    bool event(QEvent* event) override {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            const auto* key = static_cast<QKeyEvent*>(event);
            keys.append({event->type(), key->key(), key->modifiers()});
            if (event->type() == QEvent::KeyPress && onPress) onPress();
            return true;
        }
        return QWindow::event(event);
    }
};

class ControllerNavigationTest : public QObject {
    Q_OBJECT
    QTemporaryDir config;
    KeyWindow window;
    StreamingPreferences* prefs = nullptr;
    std::unique_ptr<SdlGamepadKeyNavigation> navigation;
    QList<SDL_Joystick*> virtualControllers;
    void button(Uint32 type, Uint8 button, Sint32 controller = 1) {
        SDL_Event event {};
        event.type = type;
        event.cbutton.type = type;
        event.cbutton.button = button;
        event.cbutton.which = virtualControllers.isEmpty() ? controller :
            SDL_JoystickInstanceID(virtualControllers[controller - 1]);
        QCOMPARE(SDL_PushEvent(&event), 1);
        QVERIFY(QMetaObject::invokeMethod(navigation.get(), "onPollingTimerFired"));
    }
private slots:
    void initTestCase() {
        QVERIFY(config.isValid());
        QCoreApplication::setOrganizationName("MoonlightControllerNavigationTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, config.path());
        prefs = StreamingPreferences::get();
        navigation = std::make_unique<SdlGamepadKeyNavigation>(prefs);
        navigation->enable();
#if SDL_VERSION_ATLEAST(2, 0, 14)
        // sdl2-compat translates injected events through SDL3 and requires
        // real instance IDs to distinguish controllers during that conversion.
        for (int i = 0; i < 2; ++i) {
            const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            QVERIFY(index >= 0);
            auto* joystick = SDL_JoystickOpen(index);
            QVERIFY(joystick);
            virtualControllers.append(joystick);
        }
#endif
        window.show();
        window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
    }
    void init() {
        prefs->swapFaceButtons = false;
        window.keys.clear();
        window.onPress = {};
    }
    void modeChangeDuringPress_data() {
        QTest::addColumn<int>("buttonCode");
        QTest::addColumn<bool>("uiMode");
        QTest::addColumn<int>("expectedKey");
        QTest::addColumn<int>("expectedModifiers");
        QTest::newRow("open-menu") << int(SDL_CONTROLLER_BUTTON_A) << true << int(Qt::Key_Space) << int(Qt::NoModifier);
        QTest::newRow("confirm-menu") << int(SDL_CONTROLLER_BUTTON_A) << false << int(Qt::Key_Return) << int(Qt::NoModifier);
        QTest::newRow("focus-up") << int(SDL_CONTROLLER_BUTTON_DPAD_UP) << true << int(Qt::Key_Tab) << int(Qt::ShiftModifier);
        QTest::newRow("menu-up") << int(SDL_CONTROLLER_BUTTON_DPAD_UP) << false << int(Qt::Key_Up) << int(Qt::NoModifier);
        QTest::newRow("focus-down") << int(SDL_CONTROLLER_BUTTON_DPAD_DOWN) << true << int(Qt::Key_Tab) << int(Qt::NoModifier);
        QTest::newRow("menu-down") << int(SDL_CONTROLLER_BUTTON_DPAD_DOWN) << false << int(Qt::Key_Down) << int(Qt::NoModifier);
    }
    void modeChangeDuringPress() {
        QFETCH(int, buttonCode); QFETCH(bool, uiMode);
        QFETCH(int, expectedKey); QFETCH(int, expectedModifiers);
        navigation->setUiNavMode(uiMode);
        window.onPress = [this, uiMode] { navigation->setUiNavMode(!uiMode); };
        button(SDL_CONTROLLERBUTTONDOWN, Uint8(buttonCode));
        button(SDL_CONTROLLERBUTTONUP, Uint8(buttonCode));
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].type, QEvent::KeyPress);
        QCOMPARE(window.keys[1].type, QEvent::KeyRelease);
        for (const auto& key : window.keys) {
            QCOMPARE(key.key, expectedKey);
            QCOMPARE(int(key.modifiers), expectedModifiers);
        }
    }
    void controllersKeepIndependentPresses() {
        navigation->setUiNavMode(true);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A, 1);
        navigation->setUiNavMode(false);
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A, 2);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A, 1);
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A, 2);
        QCOMPARE(window.keys.size(), 4);
        QCOMPARE(window.keys[2].key, int(Qt::Key_Space));
        QCOMPARE(window.keys[3].key, int(Qt::Key_Return));
    }
    void swapChangeStillReleasesOriginalKey() {
        navigation->setUiNavMode(true);
        prefs->swapFaceButtons = true;
        button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B);
        prefs->swapFaceButtons = false;
        button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B);
        QCOMPARE(window.keys.size(), 2);
        QCOMPARE(window.keys[0].key, int(Qt::Key_Space));
        QCOMPARE(window.keys[1].key, int(Qt::Key_Space));
    }
    void cleanupTestCase() {
        for (auto* joystick : virtualControllers) SDL_JoystickClose(joystick);
        navigation.reset();
        SDL_Quit();
    }
};
QTEST_MAIN(ControllerNavigationTest)
#include "tst_controllernavigation.moc"
