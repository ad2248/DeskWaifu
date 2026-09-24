#include "ModelKeyboard.hpp"
#include "ModelInputDispatch.hpp"
#include "ModelKeyboardGrab.hpp"
#include "ModelHotkeyTrigger.hpp"
#include "ModelHotkeys.hpp"

#include <QtTest>

class ModelKeyboardTest : public QObject
{
    Q_OBJECT
private slots:
    void keyDispatchHandlesGrabFocusRepeatAndAmbiguity()
    {
        std::vector<ModelHotkeys::Binding> bindings(1);
        bindings[0].id = "action-1";
        bindings[0].keys = {"A"};
        ModelKeyboard keyboard;
        QCOMPARE(DecideModelKey(keyboard, false, KEY_A, true, &bindings).action,
            ModelKeyDecision::None);
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true,
            static_cast<const std::vector<ModelHotkeys::Binding>*>(nullptr)).action,
            ModelKeyDecision::None);
        QVERIFY(!keyboard.Key(KEY_A, true)); // no model still updates pressed keys
        keyboard.Reset();

        const auto first = DecideModelKey(keyboard, true, KEY_A, true, &bindings);
        QCOMPARE(first.action, ModelKeyDecision::Trigger);
        QCOMPARE(first.bindingId, std::string("action-1"));
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true, &bindings).action,
            ModelKeyDecision::None); // repeat
        ModelHotkeys::Binding duplicate;
        duplicate.id = "action-2";
        duplicate.keys = {"A"};
        bindings.push_back(duplicate);
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, false, &bindings).action,
            ModelKeyDecision::None);
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true, &bindings).action,
            ModelKeyDecision::None); // ambiguous chord
        QCOMPARE(DecideModelKey(keyboard, true, KEY_ESC, true, &bindings).action,
            ModelKeyDecision::ReleaseGrab);
        QVERIFY(keyboard.Key(KEY_A, true)); // Esc cleared held keys

        keyboard.Reset();
        ModelHotkeys::Binding disabled;
        disabled.id = "disabled";
        disabled.keys = {"B"};
        disabled.enabled = false;
        bindings = {disabled};
        QCOMPARE(DecideModelKey(keyboard, true, KEY_B, true, &bindings).action,
            ModelKeyDecision::None);
        keyboard.Reset();
        ModelHotkeys::Binding conflict;
        conflict.id = "conflict";
        conflict.reason = "duplicate chord";
        conflict.keys = {"C"};
        bindings = {conflict};
        QCOMPARE(DecideModelKey(keyboard, true, KEY_C, true, &bindings).action,
            ModelKeyDecision::None);

        ModelHotkeys::Binding chord;
        chord.id = "ctrl-a";
        chord.keys = {"Ctrl", "A"};
        bindings = {chord};
        keyboard.Reset();
        QCOMPARE(DecideModelKey(keyboard, true, KEY_LEFTCTRL, true, &bindings).action,
            ModelKeyDecision::None);
        const auto completedChord = DecideModelKey(keyboard, true, KEY_A, true, &bindings);
        QCOMPARE(completedChord.action, ModelKeyDecision::Trigger);
        QCOMPARE(completedChord.bindingId, std::string("ctrl-a"));
        QCOMPARE(DecideModelKey(keyboard, true, KEY_LEFTCTRL, false, &bindings).action,
            ModelKeyDecision::None);
    }

    void pendingTapDispatchSuppressesBothClicksOfDoubleClick()
    {
        ModelDoubleClick clicks;
        std::vector<std::pair<int, int>> taps;
        const auto tap = [&](int x, int y) { taps.emplace_back(x, y); };

        QCOMPARE(clicks.Release(100, 40, 50, 45, 55), ModelDoubleClick::Pending);
        DispatchPendingTap(clicks, tap); // expiry or pointer leave
        QCOMPARE(taps.size(), size_t(1));
        QCOMPARE(taps.front(), std::make_pair(45, 55));
        DispatchPendingTap(clicks, tap);
        QCOMPARE(taps.size(), size_t(1));

        QCOMPARE(clicks.Release(500, 40, 50, 49, 59), ModelDoubleClick::Pending);
        QCOMPARE(clicks.Release(600, 42, 51, 43, 52), ModelDoubleClick::DoubleClick);
        DispatchPendingTap(clicks, tap);
        QCOMPARE(taps.size(), size_t(1)); // both clicks were suppressed
    }

    void externalFocusLossAllowsRegrab()
    {
        bool grabbed = true;
        QVERIFY(CanChangeKeyboardGrab(grabbed, false, false, false));
        grabbed = false; // compositor focus lost or keyboard capability removed
        QVERIFY(!CanChangeKeyboardGrab(grabbed, true, false, true));
        QVERIFY(!CanChangeKeyboardGrab(grabbed, true, true, false));
        QVERIFY(CanChangeKeyboardGrab(grabbed, true, true, true));
        grabbed = true;
        QVERIFY(!CanChangeKeyboardGrab(grabbed, true, true, true));
    }

    void focusLossDuringOutputSwitchDefersCommitAndOnlyResetsOnce()
    {
        bool grabbed = true;
        bool dirty = false;
        const bool configured = false; // newly created layer awaiting configure

        QVERIFY(UpdateKeyboardGrabState(grabbed, false, true, true));
        QVERIFY(!UpdateKeyboardGrabState(grabbed, false, true, true)); // no second reset
        QVERIFY(DeferKeyboardInteractivity(configured, dirty)); // no pre-configure commit
        QVERIFY(dirty);
        QVERIFY(TakePendingKeyboardInteractivity(dirty)); // apply after configure
        QVERIFY(!TakePendingKeyboardInteractivity(dirty));
        QVERIFY(!DeferKeyboardInteractivity(true, dirty)); // live layer commits directly
        QVERIFY(!dirty);
    }

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
        QCOMPARE(clicks.Release(100, 40, 50, 40, 50), ModelDoubleClick::Pending);
        QVERIFY(!clicks.Expired(449));
        QCOMPARE(clicks.Release(300, 45, 55, 45, 55), ModelDoubleClick::DoubleClick);
        QVERIFY(!clicks.Take());
        QCOMPARE(clicks.Release(500, 40, 50, 40, 50), ModelDoubleClick::Pending);
        QVERIFY(clicks.Expired(851));
        QVERIFY(clicks.Take());
        QCOMPARE(clicks.Release(900, 40, 50, 40, 50), ModelDoubleClick::Pending);
        QVERIFY(!clicks.IsSecondClick(950, 60, 50));
        QVERIFY(clicks.Take());
        // Drag path flushes the previous tap without calling Release.
        QVERIFY(!clicks.Take());
        QCOMPARE(clicks.Release(970, 60, 50, 60, 50), ModelDoubleClick::Pending);
    }

    void delayedTapUsesReleaseCoordinatesButDoubleClickUsesPressCoordinates()
    {
        ModelDoubleClick clicks;
        QCOMPARE(clicks.Release(100, 40, 50, 49, 59), ModelDoubleClick::Pending);
        QCOMPARE(clicks.X(), 49);
        QCOMPARE(clicks.Y(), 59);
        QVERIFY(clicks.IsSecondClick(300, 32, 43));
        QVERIFY(!clicks.IsSecondClick(300, 50, 60));
        QCOMPARE(clicks.Release(300, 32, 43, 32, 43), ModelDoubleClick::DoubleClick);
        QVERIFY(!clicks.Take());

        QCOMPARE(clicks.Release(500, 40, 50, 49, 59), ModelDoubleClick::Pending);
        QVERIFY(clicks.Take()); // pointer left the surface: flush without waiting
        QCOMPARE(clicks.X(), 49);
        QCOMPARE(clicks.Y(), 59);
        QVERIFY(!clicks.IsSecondClick(600, 40, 50));
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
