#include "ModelKeyboard.hpp"
#include "ModelInputDispatch.hpp"

#include <QtTest>

class ModelKeyboardTest : public QObject
{
    Q_OBJECT
private slots:
    void keyDispatchHandlesGrabFocusRepeatAndAmbiguity()
    {
        struct Binding
        {
            std::string id, reason;
            std::vector<std::string> keys;
            bool enabled = true;
        };
        std::vector<Binding> bindings{{"action-1", "", {"A"}, true}};
        ModelKeyboard keyboard;
        QCOMPARE(DecideModelKey(keyboard, false, KEY_A, true, &bindings).action,
            ModelKeyDecision::None);
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true,
            static_cast<const std::vector<Binding>*>(nullptr)).action, ModelKeyDecision::None);
        QVERIFY(!keyboard.Key(KEY_A, true)); // no model still updates pressed keys
        keyboard.Reset();

        const auto first = DecideModelKey(keyboard, true, KEY_A, true, &bindings);
        QCOMPARE(first.action, ModelKeyDecision::Trigger);
        QCOMPARE(first.bindingId, std::string("action-1"));
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true, &bindings).action,
            ModelKeyDecision::None); // repeat
        bindings.push_back({"action-2", "", {"A"}, true});
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, false, &bindings).action,
            ModelKeyDecision::None);
        QCOMPARE(DecideModelKey(keyboard, true, KEY_A, true, &bindings).action,
            ModelKeyDecision::None); // ambiguous chord
        QCOMPARE(DecideModelKey(keyboard, true, KEY_ESC, true, &bindings).action,
            ModelKeyDecision::ReleaseGrab);
        QVERIFY(keyboard.Key(KEY_A, true)); // Esc cleared held keys
    }

    void pendingTapDispatchSuppressesBothClicksOfDoubleClick()
    {
        ModelDoubleClick clicks;
        std::vector<std::pair<int, int>> taps;
        const auto tap = [&](int x, int y) { taps.emplace_back(x, y); };

        QCOMPARE(clicks.Release(100, 40, 50, false), ModelDoubleClick::Pending);
        DispatchPendingTap(clicks, tap); // expiry or pointer leave
        QCOMPARE(taps.size(), size_t(1));
        QCOMPARE(taps.front(), std::make_pair(40, 50));
        DispatchPendingTap(clicks, tap);
        QCOMPARE(taps.size(), size_t(1));

        QCOMPARE(clicks.Release(500, 40, 50, false), ModelDoubleClick::Pending);
        QCOMPARE(clicks.Release(600, 42, 51, false), ModelDoubleClick::DoubleClick);
        DispatchPendingTap(clicks, tap);
        QCOMPARE(taps.size(), size_t(1)); // both clicks were suppressed
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
