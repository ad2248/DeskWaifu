#include "ModelTransform.hpp"
#include "ModelScroll.hpp"

#include <QtTest>

class ModelTransformTest : public QObject
{
    Q_OBJECT

    static float modelPointAtAnchor(const ModelTransform& transform, float anchor)
    {
        return anchor / transform.scale;
    }

    static void compareAnchor(float actual, float expected)
    {
        QVERIFY(qAbs(actual - expected) < 0.00001f);
    }

private slots:
    void scrollConvertsAnchorAndUpdatesModelPosition()
    {
        struct View
        {
            float TransformScreenX(float x) { return x * 0.5f; }
            float TransformScreenY(float y) { return -y * 0.25f; }
        } view;
        const ModelTransform before{1.0f, 0.3f, -0.2f};
        const float anchorX = 0.8f;
        const float anchorY = -0.5f;
        const float pointX = anchorX / before.scale - before.x;
        const float pointY = anchorY / before.scale - before.y;

        const ModelTransform after = ZoomModelFromScroll(before, &view, 1.6f, 2.0f, 1.0f);

        compareAnchor(after.scale, 1.1f);
        compareAnchor(after.scale * (pointX + after.x), anchorX);
        compareAnchor(after.scale * (pointY + after.y), anchorY);
    }

    void scrollWithNullViewKeepsTransform()
    {
        struct View
        {
            float TransformScreenX(float) { return 0; }
            float TransformScreenY(float) { return 0; }
        };
        const ModelTransform before{1.0f, 0.3f, -0.2f};
        const ModelTransform after = ZoomModelFromScroll(before, static_cast<View*>(nullptr), 10.0f, 20.0f, 1.0f);

        QCOMPARE(after.scale, before.scale);
        QCOMPARE(after.x, before.x);
        QCOMPARE(after.y, before.y);
    }

    void keepsOffCenterPointFixed()
    {
        const ModelTransform before{1.0f, 0.0f, 0.0f};
        const float anchorX = 0.9f;
        const float anchorY = 0.5f;
        const float pointX = modelPointAtAnchor(before, anchorX) - before.x;
        const float pointY = modelPointAtAnchor(before, anchorY) - before.y;

        const ModelTransform after = ZoomModelAroundPoint(before, anchorX, anchorY, 1.1f);

        compareAnchor(after.scale * (pointX + after.x), anchorX);
        compareAnchor(after.scale * (pointY + after.y), anchorY);
    }

    void keepsPointFixedWithExistingTransform()
    {
        const ModelTransform before{2.0f, 0.2f, -0.1f};
        const float anchorX = -1.2f;
        const float anchorY = -0.7f;
        const float pointX = modelPointAtAnchor(before, anchorX) - before.x;
        const float pointY = modelPointAtAnchor(before, anchorY) - before.y;

        const ModelTransform after = ZoomModelAroundPoint(before, anchorX, anchorY, 1.8f);

        compareAnchor(after.scale * (pointX + after.x), anchorX);
        compareAnchor(after.scale * (pointY + after.y), anchorY);
    }

    void doesNotMoveAtScaleLimits()
    {
        const ModelTransform minimum{0.1f, 0.2f, -0.1f};
        const ModelTransform maximum{10.0f, 0.2f, -0.1f};

        const ModelTransform belowMinimum = ZoomModelAroundPoint(minimum, 1.0f, -1.0f, 0.09f);
        QCOMPARE(belowMinimum.scale, 0.1f);
        QCOMPARE(belowMinimum.x, minimum.x);
        QCOMPARE(belowMinimum.y, minimum.y);

        const ModelTransform aboveMaximum = ZoomModelAroundPoint(maximum, 1.0f, -1.0f, 11.0f);
        QCOMPARE(aboveMaximum.scale, 10.0f);
        QCOMPARE(aboveMaximum.x, maximum.x);
        QCOMPARE(aboveMaximum.y, maximum.y);
    }

    void usesClampedScaleForCompensation()
    {
        const ModelTransform before{0.105f, 0.2f, -0.1f};
        const float anchorX = 0.8f;
        const float anchorY = -0.4f;
        const float pointX = modelPointAtAnchor(before, anchorX) - before.x;
        const float pointY = modelPointAtAnchor(before, anchorY) - before.y;

        const ModelTransform after = ZoomModelAroundPoint(before, anchorX, anchorY, 0.09f);

        QCOMPARE(after.scale, 0.1f);
        compareAnchor(after.scale * (pointX + after.x), anchorX);
        compareAnchor(after.scale * (pointY + after.y), anchorY);
    }
};

QTEST_GUILESS_MAIN(ModelTransformTest)
#include "ModelTransformTest.moc"
