#include "processor/interaction/ModelZoom.hpp"
#include "processor/model/ModelProjection.hpp"

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
    void rendererUsesModelSpaceTranslation()
    {
        const ModelTransform before{2.0f, 0.3f, -0.2f};
        Live2D::Cubism::Framework::CubismMatrix44 projection;
        ApplyModelTransform(projection, before);

        compareAnchor(projection.TransformX(0.4f), before.scale * (0.4f + before.x));
        compareAnchor(projection.TransformY(-0.1f), before.scale * (-0.1f + before.y));
    }

    void rendererPreservesNonIdentityBaseProjection()
    {
        const ModelTransform transform{2.0f, 0.3f, -0.2f};
        Live2D::Cubism::Framework::CubismMatrix44 projection;
        const float aspect = 0.75f;
        projection.ScaleRelative(aspect, 1.0f);

        ApplyModelTransform(projection, transform);

        compareAnchor(projection.TransformX(0.4f),
            aspect * transform.scale * (0.4f + transform.x));
        compareAnchor(projection.TransformY(-0.1f),
            transform.scale * (-0.1f + transform.y));
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

    void roundTripRestoresTranslation()
    {
        const ModelTransform before{1.0f, 0.3f, -0.2f};
        const ModelTransform enlarged = ZoomModelAroundPoint(before, 0.9f, -0.6f, 2.0f);
        const ModelTransform restored = ZoomModelAroundPoint(enlarged, 0.9f, -0.6f, 1.0f);

        QCOMPARE(restored.scale, before.scale);
        compareAnchor(restored.x, before.x);
        compareAnchor(restored.y, before.y);
    }

    void nonPositiveScaleSkipsCompensation()
    {
        for (float scale : {0.0f, -1.0f})
        {
            const ModelTransform before{scale, 0.3f, -0.2f};
            const ModelTransform after = ZoomModelAroundPoint(before, 0.9f, -0.6f, 2.0f);

            QCOMPARE(after.scale, 2.0f);
            QCOMPARE(after.x, before.x);
            QCOMPARE(after.y, before.y);
        }
    }
};

QTEST_GUILESS_MAIN(ModelTransformTest)
#include "ModelTransformTest.moc"
