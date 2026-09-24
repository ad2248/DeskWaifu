#include "ModelKeyboard.hpp"
#include "ModelHotkeyTrigger.hpp"

#include <QtTest>

class ModelKeyboardTest : public QObject
{
    Q_OBJECT
private slots:
    void reportsHotkeyTriggerFailureWithBindingId()
    {
        struct Hotkeys
        {
            bool succeeds = false;
            bool Trigger(const std::string&, std::string& error)
            {
                if (!succeeds) error = "action not loaded";
                return succeeds;
            }
        } hotkeys;
        std::string loggedId, loggedError;
        const auto log = [&](const std::string& id, const std::string& error)
        {
            loggedId = id;
            loggedError = error;
        };

        QVERIFY(!TriggerModelHotkey(hotkeys, "binding-42", log));
        QCOMPARE(loggedId, std::string("binding-42"));
        QCOMPARE(loggedError, std::string("action not loaded"));

        hotkeys.succeeds = true;
        loggedId.clear();
        QVERIFY(TriggerModelHotkey(hotkeys, "binding-42", log));
        QVERIFY(loggedId.empty());
    }

    void doubleClickSuppressesTwoTaps()
    {
        ModelDoubleClick clicks;
        QCOMPARE(clicks.Release(100, 40, 50, false), ModelDoubleClick::Pending);
        QVERIFY(!clicks.Expired(449));
        QCOMPARE(clicks.Release(300, 45, 55, false), ModelDoubleClick::DoubleClick);
        QVERIFY(!clicks.Take());
        QCOMPARE(clicks.Release(500, 40, 50, false), ModelDoubleClick::Pending);
        QVERIFY(clicks.Expired(851));
        QVERIFY(clicks.Take());
        QCOMPARE(clicks.Release(900, 40, 50, false), ModelDoubleClick::Pending);
        QVERIFY(!clicks.IsSecondClick(950, 60, 50));
        QVERIFY(clicks.Take());
        QCOMPARE(clicks.Release(970, 60, 50, true), ModelDoubleClick::Ignore);
        QVERIFY(!clicks.Take());
    }

    void letterDigitChordAndRelease()
    {
        ModelKeyboard keyboard;
        QVERIFY(keyboard.Key(KEY_LEFTCTRL, true));
        QVERIFY(keyboard.Key(KEY_A, true));
        QVERIFY(keyboard.Matches({"LeftControl", "A"}));
        QVERIFY(!keyboard.Matches({"A"}));
        QVERIFY(!keyboard.Key(KEY_A, true)); // auto-repeat
        QVERIFY(!keyboard.Key(KEY_A, false));
        QVERIFY(keyboard.Key(KEY_1, true));
        QVERIFY(keyboard.Matches({"Ctrl", "N1"}));
        QVERIFY(!keyboard.Matches({"Ctrl", "Tab"}));
        keyboard.Reset(); // focus lost
        QVERIFY(!keyboard.Matches({"Ctrl", "N1"}));
        QVERIFY(keyboard.Key(KEY_Z, true));
        QVERIFY(keyboard.Matches({"Z"}));
        keyboard.Reset();
        QVERIFY(keyboard.Key(KEY_P, true));
        QVERIFY(keyboard.Matches({"P"}));
        QVERIFY(!keyboard.Key(KEY_TAB, true));
        keyboard.Reset();
        QVERIFY(keyboard.Key(KEY_RIGHTCTRL, true));
        QVERIFY(keyboard.Key(KEY_LEFTSHIFT, true));
        QVERIFY(keyboard.Key(KEY_RIGHTALT, true));
        QVERIFY(keyboard.Key(KEY_0, true));
        QVERIFY(keyboard.Matches({"LeftControl", "Shift", "LeftAlt", "N0"}));
        QVERIFY(!keyboard.Matches({"Ctrl", "Shift", "N0"}));
    }
};

QTEST_GUILESS_MAIN(ModelKeyboardTest)
#include "ModelKeyboardTest.moc"
