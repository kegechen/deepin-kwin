/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"
#include "blurshader.h"
// KConfigSkeleton
#include "blurconfig.h"

#include <QGuiApplication>
#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QPainterPath>
#include <QScreen> // for QGuiApplication
#include <QTime>
#include <QTimer>
#include <QWindow>
#include <cmath> // for ceil()

#include <DWayland/Server/surface_interface.h>
#include <DWayland/Server/shadow_interface.h>
#include <DWayland/Server/display.h>
#include <KSharedConfig>
#include <KConfigGroup>

Q_DECLARE_METATYPE(QPainterPath)

namespace KWin
{

static const QByteArray s_blurAtomName = QByteArrayLiteral("_KDE_NET_WM_BLUR_BEHIND_REGION");

KWaylandServer::BlurManagerInterface *BlurEffect::s_blurManager = nullptr;
QTimer *BlurEffect::s_blurManagerRemoveTimer = nullptr;

BlurEffect::BlurEffect()
{
    initConfig<BlurConfig>();
    m_shader = new BlurShader(this);

    initBlurStrengthValues();
    reconfigure(ReconfigureAll);

    // ### Hackish way to announce support.
    //     Should be included in _NET_SUPPORTED instead.
    if (m_shader && m_shader->isValid() && m_renderTargetsValid) {
        if (effects->xcbConnection()) {
            net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
        }
        if (effects->waylandDisplay()) {
            if (!s_blurManagerRemoveTimer) {
                s_blurManagerRemoveTimer = new QTimer(qApp);
                s_blurManagerRemoveTimer->setSingleShot(true);
                s_blurManagerRemoveTimer->callOnTimeout([]() {
                    s_blurManager->remove();
                    s_blurManager = nullptr;
                });
            }
            s_blurManagerRemoveTimer->stop();
            if (!s_blurManager) {
                s_blurManager = new KWaylandServer::BlurManagerInterface(effects->waylandDisplay(), s_blurManagerRemoveTimer);
            }
        }
    }

    connect(effects, &EffectsHandler::windowAdded, this, &BlurEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &BlurEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::propertyNotify, this, &BlurEffect::slotPropertyNotify);
    connect(effects, &EffectsHandler::virtualScreenGeometryChanged, this, &BlurEffect::slotScreenGeometryChanged);
    connect(effects, &EffectsHandler::xcbConnectionChanged, this,
        [this] {
            if (m_shader && m_shader->isValid() && m_renderTargetsValid) {
                net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
            }
        }
    );

    // Fetch the blur regions for all windows
    const auto stackingOrder = effects->stackingOrder();
    for (EffectWindow *window : stackingOrder) {
        slotWindowAdded(window);
    }
}

BlurEffect::~BlurEffect()
{
    // When compositing is restarted, avoid removing the manager immediately.
    if (s_blurManager) {
        s_blurManagerRemoveTimer->start(1000);
    }
    deleteFBOs();
}

void BlurEffect::slotScreenGeometryChanged()
{
    effects->makeOpenGLContextCurrent();
    updateTexture();

    // Fetch the blur regions for all windows
    const auto stackingOrder = effects->stackingOrder();
    for (EffectWindow *window : stackingOrder) {
        updateBlurRegion(window);
    }
    effects->doneOpenGLContextCurrent();
}

bool BlurEffect::renderTargetsValid() const
{
    return !m_renderTargets.isEmpty() && std::find_if(m_renderTargets.cbegin(), m_renderTargets.cend(),
        [](const GLRenderTarget *target) {
            return !target->valid();
        }) == m_renderTargets.cend();
}

void BlurEffect::deleteFBOs()
{
    qDeleteAll(m_renderTargets);

    m_renderTargets.clear();
    m_renderTextures.clear();
}

void BlurEffect::updateTexture()
{
    deleteFBOs();

    /* Reserve memory for:
     *  - The original sized texture (1)
     *  - The downsized textures (m_downSampleIterations)
     *  - The helper texture (1)
     */
    m_renderTargets.reserve(m_downSampleIterations + 2);
    m_renderTextures.reserve(m_downSampleIterations + 2);

    GLenum textureFormat = GL_RGBA8;

    // Check the color encoding of the default framebuffer
    if (!GLPlatform::instance()->isGLES()) {
        GLuint prevFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, reinterpret_cast<GLint *>(&prevFbo));

        if (prevFbo != 0) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }

        GLenum colorEncoding = GL_LINEAR;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT,
                                              GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
                                              reinterpret_cast<GLint *>(&colorEncoding));

        if (prevFbo != 0) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevFbo);
        }

        if (colorEncoding == GL_SRGB) {
            textureFormat = GL_SRGB8_ALPHA8;
        }
    }

    for (int i = 0; i <= m_downSampleIterations; i++) {
        m_renderTextures.append(GLTexture(textureFormat, effects->virtualScreenSize() / (1 << i)));
        m_renderTextures.last().setFilter(GL_LINEAR);
        m_renderTextures.last().setWrapMode(GL_CLAMP_TO_EDGE);

        m_renderTargets.append(new GLRenderTarget(m_renderTextures.last()));
    }

    // This last set is used as a temporary helper texture
    m_renderTextures.append(GLTexture(textureFormat, effects->virtualScreenSize()));
    m_renderTextures.last().setFilter(GL_LINEAR);
    m_renderTextures.last().setWrapMode(GL_CLAMP_TO_EDGE);

    m_renderTargets.append(new GLRenderTarget(m_renderTextures.last()));

    m_renderTargetsValid = renderTargetsValid();

    // Prepare the stack for the rendering
    m_renderTargetStack.clear();
    m_renderTargetStack.reserve(m_downSampleIterations * 2);

    // Upsample
    for (int i = 1; i < m_downSampleIterations; i++) {
        m_renderTargetStack.push(m_renderTargets[i]);
    }

    // Downsample
    for (int i = m_downSampleIterations; i > 0; i--) {
        m_renderTargetStack.push(m_renderTargets[i]);
    }

    // Copysample
    m_renderTargetStack.push(m_renderTargets[0]);

    // Generate the noise helper texture
    generateNoiseTexture();
}

void BlurEffect::initBlurStrengthValues()
{
    // This function creates an array of blur strength values that are evenly distributed

    // The range of the slider on the blur settings UI
    int numOfBlurSteps = 15;
    int remainingSteps = numOfBlurSteps;

    /*
     * Explanation for these numbers:
     *
     * The texture blur amount depends on the downsampling iterations and the offset value.
     * By changing the offset we can alter the blur amount without relying on further downsampling.
     * But there is a minimum and maximum value of offset per downsample iteration before we
     * get artifacts.
     *
     * The minOffset variable is the minimum offset value for an iteration before we
     * get blocky artifacts because of the downsampling.
     *
     * The maxOffset value is the maximum offset value for an iteration before we
     * get diagonal line artifacts because of the nature of the dual kawase blur algorithm.
     *
     * The expandSize value is the minimum value for an iteration before we reach the end
     * of a texture in the shader and sample outside of the area that was copied into the
     * texture from the screen.
     */

    // {minOffset, maxOffset, expandSize}
    blurOffsets.append({1.0, 2.0, 10});     // Down sample size / 2
    blurOffsets.append({2.0, 3.0, 20});     // Down sample size / 4
    blurOffsets.append({2.0, 5.0, 50});     // Down sample size / 8
    blurOffsets.append({3.0, 8.0, 150});    // Down sample size / 16
    //blurOffsets.append({5.0, 10.0, 400}); // Down sample size / 32
    //blurOffsets.append({7.0, ?.0});       // Down sample size / 64

    float offsetSum = 0;

    for (int i = 0; i < blurOffsets.size(); i++) {
        offsetSum += blurOffsets[i].maxOffset - blurOffsets[i].minOffset;
    }

    for (int i = 0; i < blurOffsets.size(); i++) {
        int iterationNumber = std::ceil((blurOffsets[i].maxOffset - blurOffsets[i].minOffset) / offsetSum * numOfBlurSteps);
        remainingSteps -= iterationNumber;

        if (remainingSteps < 0) {
            iterationNumber += remainingSteps;
        }

        float offsetDifference = blurOffsets[i].maxOffset - blurOffsets[i].minOffset;

        for (int j = 1; j <= iterationNumber; j++) {
            // {iteration, offset}
            blurStrengthValues.append({i + 1, blurOffsets[i].minOffset + (offsetDifference / iterationNumber) * j});
        }
    }
}

void BlurEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    BlurConfig::self()->read();

    int blurStrength = BlurConfig::blurStrength() - 1;
    m_downSampleIterations = blurStrengthValues[blurStrength].iteration;
    m_offset = blurStrengthValues[blurStrength].offset;
    m_expandSize = blurOffsets[m_downSampleIterations - 1].expandSize;
    m_noiseStrength = BlurConfig::noiseStrength();

    m_noiseStrength = 0;

    m_scalingFactor = qMax(1.0, QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0);

    updateTexture();

    // Update all windows for the blur to take effect
    effects->addRepaintFull();
}

QRegion rounded(QRegion region, const QPointF& r)
{
    auto rect = region.boundingRect();
    int x = rect.x();
    int y = rect.y();
    int w = rect.width() - r.x();
    int h = rect.height() - r.y();
    auto r1 = QRegion(x, y, r.x(), r.y());
    auto r2 = r1.translated(0, h);
    auto r3 = r1.translated(w, h);
    auto r4 = r1.translated(w, 0);
    auto c1 = QRegion(x, y, 2 * r.x(), 2 * r.y(), QRegion::Ellipse);
    auto c2 = c1.translated(0, h - r.y());
    auto c3 = c1.translated(w - r.x(), h - r.y());
    auto c4 = c1.translated(w - r.x(), 0);
    return (region - (r1 + r2 + r3 + r4) + (c1 + c2 + c3 + c4));
}

void BlurEffect::updateBlurRegion(EffectWindow *w) const
{
    QRegion region;
    bool valid = false;

    if (net_wm_blur_region != XCB_ATOM_NONE) {
        const QByteArray value = w->readProperty(net_wm_blur_region, XCB_ATOM_CARDINAL, 32);
        if (value.size() > 0 && !(value.size() % (4 * sizeof(uint32_t)))) {
            const uint32_t *cardinals = reinterpret_cast<const uint32_t*>(value.constData());
            for (unsigned int i = 0; i < value.size() / sizeof(uint32_t);) {
                int x = cardinals[i++];
                int y = cardinals[i++];
                int w = cardinals[i++];
                int h = cardinals[i++];
                region += QRect(x, y, w, h);
            }
        }
        valid = !value.isNull();
    }

    KWaylandServer::SurfaceInterface *surf = w->surface();

    if (surf && surf->blur()) {
        region = surf->blur()->region();
        valid = true;
    }

    if (auto internal = w->internalWindow()) {
        const auto property = internal->property("kwin_blur");
        if (property.isValid()) {
            region = property.value<QRegion>();
            valid = true;
        }
    }

    // If the specified blur region is empty, enable blur for the whole window.
    if (region.isEmpty() && valid) {
        // Set the data to a dummy value.
        // This is needed to be able to distinguish between the value not
        // being set, and being set to an empty region.
        w->setData(WindowBlurBehindRole, 1);
    } else {
        w->setData(WindowBlurBehindRole, region);
    }
}

void BlurEffect::slotWindowAdded(EffectWindow *w)
{
    KWaylandServer::SurfaceInterface *surf = w->surface();

    if (surf) {
        windowBlurChangedConnections[w] = connect(surf, &KWaylandServer::SurfaceInterface::blurChanged, this, [this, w] () {
            if (w) {
                updateBlurRegion(w);
            }
        });
    }
    if (auto internal = w->internalWindow()) {
        internal->installEventFilter(this);
    }

    updateBlurRegion(w);
}

void BlurEffect::slotWindowDeleted(EffectWindow *w)
{
    auto it = windowBlurChangedConnections.find(w);
    if (it == windowBlurChangedConnections.end()) {
        return;
    }
    disconnect(*it);
    windowBlurChangedConnections.erase(it);
}

void BlurEffect::slotPropertyNotify(EffectWindow *w, long atom)
{
    if (w && atom == net_wm_blur_region && net_wm_blur_region != XCB_ATOM_NONE) {
        updateBlurRegion(w);
    }
}

bool BlurEffect::eventFilter(QObject *watched, QEvent *event)
{
    auto internal = qobject_cast<QWindow*>(watched);
    if (internal && event->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent *pe = static_cast<QDynamicPropertyChangeEvent*>(event);
        if (pe->propertyName() == "kwin_blur") {
            if (auto w = effects->findWindow(internal)) {
                updateBlurRegion(w);
            }
        }
    }
    return false;
}

bool BlurEffect::enabledByDefault()
{
    GLPlatform *gl = GLPlatform::instance();

    if (gl->isIntel() && gl->chipClass() < SandyBridge)
        return false;
    if (gl->isPanfrost() && gl->chipClass() <= MaliT8XX) {
        return false;
    }
    if (gl->isSoftwareEmulation()) {
        return false;
    }

    return true;
}

bool BlurEffect::supported()
{
    bool supported = effects->isOpenGLCompositing() && GLRenderTarget::supported() && GLRenderTarget::blitSupported();

    if (supported) {
        int maxTexSize;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);

        const QSize screenSize = effects->virtualScreenSize();
        if (screenSize.width() > maxTexSize || screenSize.height() > maxTexSize)
            supported = false;
    }
    return supported;
}

QRect BlurEffect::expand(const QRect &rect) const
{
    return rect.adjusted(-m_expandSize, -m_expandSize, m_expandSize, m_expandSize);
}

QRegion BlurEffect::expand(const QRegion &region) const
{
    QRegion expanded;

    for (const QRect &rect : region) {
        expanded += expand(rect);
    }

    return expanded;
}

QRegion BlurEffect::blurRegion(const EffectWindow *w) const
{
    QRegion region;

    const QVariant value = w->data(WindowBlurBehindRole);
    if (value.isValid()) {
        const QRegion appRegion = qvariant_cast<QRegion>(value);
        if (!appRegion.isEmpty()) {
            if (w->decorationHasAlpha() && effects->decorationSupportsBlurBehind()) {
                region = QRegion(w->rect()) - w->decorationInnerRect();
            }
            region |= appRegion.translated(w->contentsRect().topLeft()) & w->decorationInnerRect();
        } else {
            // An empty region means that the blur effect should be enabled
            // for the whole window.
            region = w->rect();
        }
    } else if (w->decorationHasAlpha() && effects->decorationSupportsBlurBehind()) {
        // If the client hasn't specified a blur region, we'll only enable
        // the effect behind the decoration.
        region = QRegion(w->rect()) - w->decorationInnerRect();
    }

    return region;
}

void BlurEffect::uploadRegion(QVector2D *&map, const QRegion &region, const int downSampleIterations)
{
    for (int i = 0; i <= downSampleIterations; i++) {
        const int divisionRatio = (1 << i);

        for (const QRect &r : region) {
            const QVector2D topLeft(     r.x() / divisionRatio,               r.y() / divisionRatio);
            const QVector2D topRight(   (r.x() + r.width()) / divisionRatio,  r.y() / divisionRatio);
            const QVector2D bottomLeft(  r.x() / divisionRatio,              (r.y() + r.height()) / divisionRatio);
            const QVector2D bottomRight((r.x() + r.width()) / divisionRatio, (r.y() + r.height()) / divisionRatio);

            // First triangle
            *(map++) = topRight;
            *(map++) = topLeft;
            *(map++) = bottomLeft;

            // Second triangle
            *(map++) = bottomLeft;
            *(map++) = bottomRight;
            *(map++) = topRight;
        }
    }
}

void BlurEffect::uploadGeometry(GLVertexBuffer *vbo, const QRegion &blurRegion, const QRegion &windowRegion)
{
    const int vertexCount = ((blurRegion.rectCount() * (m_downSampleIterations + 1)) + windowRegion.rectCount()) * 6;

    if (!vertexCount)
        return;

    QVector2D *map = (QVector2D *) vbo->map(vertexCount * sizeof(QVector2D));

    uploadRegion(map, blurRegion, m_downSampleIterations);
    uploadRegion(map, windowRegion, 0);

    vbo->unmap();

    const GLVertexAttrib layout[] = {
        { VA_Position, 2, GL_FLOAT, 0 },
        { VA_TexCoord, 2, GL_FLOAT, 0 }
    };

    vbo->setAttribLayout(layout, 2, sizeof(QVector2D));
}

void BlurEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    m_paintedArea = QRegion();
    m_currentBlur = QRegion();

    effects->prePaintScreen(data, presentTime);
}

void BlurEffect::prePaintWindow(EffectWindow* w, WindowPrePaintData& data, std::chrono::milliseconds presentTime)
{
    // this effect relies on prePaintWindow being called in the bottom to top order

    effects->prePaintWindow(w, data, presentTime);

    if (!w->isPaintingEnabled()) {
        return;
    }
    if (!m_shader || !m_shader->isValid()) {
        return;
    }

    if (data.clip.intersects(m_currentBlur)) {
        // to blur an area partially we have to shrink the opaque area of a window
        QRegion newClip;
        const QRegion oldClip = data.clip;
        for (const QRect &rect : data.clip) {
            newClip |= rect.adjusted(m_expandSize, m_expandSize, -m_expandSize, -m_expandSize);
        }
        data.clip = newClip;

        // we don't have to blur a region we don't see
        m_currentBlur -= newClip;
        // if we have to paint a non-opaque part of this window that intersects with the
        // currently blurred region we have to redraw the whole region
        if ((data.paint - oldClip).intersects(m_currentBlur)) {
            data.paint |= m_currentBlur;
        }
    }

    // in case this window has regions to be blurred
    const QRect screen = effects->virtualScreenGeometry();
    const QRegion blurArea = blurRegion(w).translated(w->pos()) & screen;
    const QRegion expandedBlur = (w->isDock() ? blurArea : expand(blurArea)) & screen;

    // if this window or a window underneath the blurred area is painted again we have to
    // blur everything
    if (m_paintedArea.intersects(expandedBlur) || data.paint.intersects(blurArea)) {
        data.paint |= expandedBlur;
        // we have to check again whether we do not damage a blurred area
        // of a window
        if (expandedBlur.intersects(m_currentBlur)) {
            data.paint |= m_currentBlur;
        }
    }

    m_currentBlur |= expandedBlur;

    m_paintedArea -= data.clip;
    m_paintedArea |= data.paint;
}

bool BlurEffect::shouldBlur(const EffectWindow *w, int mask, const WindowPaintData &data) const
{
    if (!m_renderTargetsValid || !m_shader || !m_shader->isValid())
        return false;

    if (effects->activeFullScreenEffect() && !w->data(WindowForceBlurRole).toBool())
        return false;

    if (w->isDesktop())
        return false;

    bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
    bool translated = data.xTranslation() || data.yTranslation();

    if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !w->data(WindowForceBlurRole).toBool())
        return false;

    bool blurBehindDecos = effects->decorationsHaveAlpha() &&
                effects->decorationSupportsBlurBehind();

    if (!w->hasAlpha() && w->opacity() >= 1.0 && !(blurBehindDecos && w->hasDecoration()))
        return false;

    return true;
}

void BlurEffect::drawWindow(EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
    const QRect screen = GLRenderTarget::virtualScreenGeometry();
    if (shouldBlur(w, mask, data)) {
        QRegion shape = region & blurRegion(w).translated(w->pos()) & screen;

        // let's do the evil parts - someone wants to blur behind a transformed window
        const bool translated = data.xTranslation() || data.yTranslation();
        const bool scaled = data.xScale() != 1 || data.yScale() != 1;
        if (scaled) {
            QPoint pt = shape.boundingRect().topLeft();
            QRegion scaledShape;
            for (QRect r : shape) {
                r.moveTo(pt.x() + (r.x() - pt.x()) * data.xScale() + data.xTranslation(),
                            pt.y() + (r.y() - pt.y()) * data.yScale() + data.yTranslation());
                r.setWidth(r.width() * data.xScale());
                r.setHeight(r.height() * data.yScale());
                scaledShape |= r;
            }
            shape = scaledShape & region;

        //Only translated, not scaled
        } else if (translated) {
            shape = shape.translated(data.xTranslation(), data.yTranslation());
            shape = shape & region;
        }

        EffectWindow* modal = w->transientFor();
        const bool transientForIsDock = (modal ? modal->isDock() : false);

        if (!shape.isEmpty()) {
            const QVariant &data_clip_path = w->data(WindowClipPathRole);
            if (data_clip_path.isValid() && !w->isDock()) {
                const QPainterPath path = qvariant_cast<QPainterPath>(data_clip_path);
                QImage img(w->size(), QImage::Format_RGBA8888);
                img.fill(QColor(0,0,0,0));
                QPainter pa(&img);
                pa.setRenderHint(QPainter::Antialiasing);
                pa.fillPath(path, QColor(0, 0, 0, 255));
                pa.end();
                m_noiseTexture.reset(new GLTexture(img));
                m_noiseTexture->setFilter(GL_LINEAR);
                m_noiseTexture->setWrapMode(GL_REPEAT);
                m_noiseStrength = -1;
                doBlur(shape, screen, data.opacity(), data.screenProjectionMatrix(), false, w->frameGeometry());
            } else {
                m_noiseStrength = -2;
                const QVariant valueRadius = w->data(WindowRadiusRole);
                if (valueRadius.isValid() &&
                    valueRadius.toPointF().x() > 2 &&
                    valueRadius.toPointF().y() > 2)
                {
                    QPointF cornerRadius = w->data(WindowRadiusRole).toPointF();
                    const qreal xMin{ std::min(cornerRadius.x(),
                                               w->width() / 2.0) };
                    const qreal yMin{ std::min(cornerRadius.y(),
                                               w->height() / 2.0) };
                    const qreal minRadius{ std::min(xMin, yMin) };
                    cornerRadius = QPointF(minRadius, minRadius);
                    shape = rounded(shape, cornerRadius);
                }
                doBlur(shape, screen, data.opacity(), data.screenProjectionMatrix(), w->isDock() || transientForIsDock, w->frameGeometry());
            }
        }
    }

    // Draw the window over the blurred area
    effects->drawWindow(w, mask, region, data);
}

void BlurEffect::paintEffectFrame(EffectFrame *frame, const QRegion &region, double opacity, double frameOpacity)
{
    const QRect screen = effects->virtualScreenGeometry();
    bool valid = m_renderTargetsValid && m_shader && m_shader->isValid();

    QRegion shape = frame->geometry().adjusted(-borderSize, -borderSize, borderSize, borderSize) & screen;

    if (valid && !shape.isEmpty() && region.intersects(shape.boundingRect()) && frame->style() != EffectFrameNone) {
        doBlur(shape, screen, opacity * frameOpacity, frame->screenProjectionMatrix(), false, frame->geometry());
    }
    effects->paintEffectFrame(frame, region, opacity, frameOpacity);
}

void BlurEffect::generateNoiseTexture()
{
    if (m_noiseStrength == 0) {
        return;
    }

    // Init randomness based on time
    qsrand((uint)QTime::currentTime().msec());

    QImage noiseImage(QSize(256, 256), QImage::Format_Grayscale8);

    for (int y = 0; y < noiseImage.height(); y++) {
        uint8_t *noiseImageLine = (uint8_t *) noiseImage.scanLine(y);

        for (int x = 0; x < noiseImage.width(); x++) {
            noiseImageLine[x] = qrand() % m_noiseStrength;
        }
    }

    // The noise texture looks distorted when not scaled with integer
    noiseImage = noiseImage.scaled(noiseImage.size() * m_scalingFactor);

    m_noiseTexture.reset(new GLTexture(noiseImage));
    m_noiseTexture->setFilter(GL_LINEAR);
    m_noiseTexture->setWrapMode(GL_REPEAT);
}

void BlurEffect::doBlur(const QRegion& shape, const QRect& screen, const float opacity, const QMatrix4x4 &screenProjection, bool isDock, QRect windowRect)
{
    // Blur would not render correctly on a secondary monitor because of wrong coordinates
    // BUG: 393723
    const int xTranslate = -screen.x();
    const int yTranslate = effects->virtualScreenSize().height() - screen.height() - screen.y();

    const QRegion expandedBlurRegion = expand(shape) & expand(screen);

    const bool useSRGB = m_renderTextures.first().internalFormat() == GL_SRGB8_ALPHA8;

    // Upload geometry for the down and upsample iterations
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();

    uploadGeometry(vbo, expandedBlurRegion.translated(xTranslate, yTranslate), shape);
    vbo->bindArrays();

    const QRect sourceRect = expandedBlurRegion.boundingRect() & screen;
    const QRect destRect = sourceRect.translated(xTranslate, yTranslate);

    GLRenderTarget::pushRenderTargets(m_renderTargetStack);
    int blurRectCount = expandedBlurRegion.rectCount() * 6;

    /*
     * If the window is a dock or panel we avoid the "extended blur" effect.
     * Extended blur is when windows that are not under the blurred area affect
     * the final blur result.
     * We want to avoid this on panels, because it looks really weird and ugly
     * when maximized windows or windows near the panel affect the dock blur.
     */
    if (isDock) {
        m_renderTargets.last()->blitFromFramebuffer(sourceRect, destRect);

        if (useSRGB) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }

        const QRect screenRect = effects->virtualScreenGeometry();
        QMatrix4x4 mvp;
        mvp.ortho(0, screenRect.width(), screenRect.height(), 0, 0, 65535);
        copyScreenSampleTexture(vbo, blurRectCount, shape.translated(xTranslate, yTranslate), mvp);
    } else {
        m_renderTargets.first()->blitFromFramebuffer(sourceRect, destRect);

        if (useSRGB) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }

        // Remove the m_renderTargets[0] from the top of the stack that we will not use
        GLRenderTarget::popRenderTarget();
    }

    downSampleTexture(vbo, blurRectCount);
    upSampleTexture(vbo, blurRectCount);

    // Modulate the blurred texture with the window opacity if the window isn't opaque
    if (opacity < 1.0) {
        glEnable(GL_BLEND);
#if 1 // bow shape, always above y = x
        float o = 1.0f-opacity;
        o = 1.0f - o*o;
#else // sigmoid shape, above y = x for x > 0.5, below y = x for x < 0.5
        float o = 2.0f*opacity - 1.0f;
        o = 0.5f + o / (1.0f + qAbs(o));
#endif
        glBlendColor(0, 0, 0, o);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }

    // Render to the screen
    if (-1 == m_noiseStrength) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE1); m_noiseTexture->bind();
        glActiveTexture(GL_TEXTURE0); m_renderTextures[1].bind();
        m_shader->bind(BlurShader::UpSampleType);
        m_shader->setTargetTextureSize(m_renderTextures[0].size() * GLRenderTarget::virtualScreenScale());
        m_shader->setOffset(m_offset);
        m_shader->setModelViewProjectionMatrix(screenProjection);
        GLShader *shader = ShaderManager::instance()->getBoundShader();
        shader->setUniform("clip", true);
        shader->setUniform("texUnit", 0);
        shader->setUniform("texClip", 1);
        shader->setUniform("rect", QVector4D(windowRect.x(), screen.height() - windowRect.y(), windowRect.width(), windowRect.height()));
        vbo->draw(GL_TRIANGLES, blurRectCount * (m_downSampleIterations + 1), blurRectCount);
        shader->setUniform("clip", false);
        m_shader->unbind();
        glActiveTexture(GL_TEXTURE1); m_noiseTexture->unbind();
        glActiveTexture(GL_TEXTURE0); m_renderTextures[1].unbind();
        glDisable(GL_BLEND);
        m_noiseStrength = 0;
    } else {
        upscaleRenderToScreen(vbo, blurRectCount * (m_downSampleIterations + 1), shape.rectCount() * 6, screenProjection, windowRect.topLeft());
    }

    if (useSRGB) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }

    if (opacity < 1.0) {
        glDisable(GL_BLEND);
    }

    if (m_noiseStrength > 0) {
        // Apply an additive noise onto the blurred image.
        // The noise is useful to mask banding artifacts, which often happens due to the smooth color transitions in the
        // blurred image.
        // The noise is applied in perceptual space (i.e. after glDisable(GL_FRAMEBUFFER_SRGB)). This practice is also
        // seen in other application of noise synthesis (films, image codecs), and makes the noise less visible overall
        // (reduces graininess).
        glEnable(GL_BLEND);
        if (opacity < 1.0) {
            // We need to modulate the opacity of the noise as well; otherwise a thin layer would appear when applying
            // effects like fade out.
            // glBlendColor should have been set above.
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
        } else {
            // Add the shader's output directly to the pixels in framebuffer.
            glBlendFunc(GL_ONE, GL_ONE);
        }
        applyNoise(vbo, blurRectCount * (m_downSampleIterations + 1), shape.rectCount() * 6, screenProjection, windowRect.topLeft());
        glDisable(GL_BLEND);
    }

    vbo->unbindArrays();
}

void BlurEffect::upscaleRenderToScreen(GLVertexBuffer *vbo, int vboStart, int blurRectCount, const QMatrix4x4 &screenProjection, QPoint windowPosition)
{
    m_renderTextures[1].bind();

    m_shader->bind(BlurShader::UpSampleType);
    m_shader->setTargetTextureSize(m_renderTextures[0].size() * GLRenderTarget::virtualScreenScale());

    m_shader->setOffset(m_offset);
    m_shader->setModelViewProjectionMatrix(screenProjection);

    {
        GLShader *shader = ShaderManager::instance()->getBoundShader();
        shader->setUniform("clip", false);
    }

    //Render to the screen
    vbo->draw(GL_TRIANGLES, vboStart, blurRectCount);
    m_shader->unbind();
}

void BlurEffect::applyNoise(GLVertexBuffer *vbo, int vboStart, int blurRectCount, const QMatrix4x4 &screenProjection, QPoint windowPosition)
{
    m_shader->bind(BlurShader::NoiseSampleType);
    m_shader->setTargetTextureSize(m_renderTextures[0].size() * GLRenderTarget::virtualScreenScale());
    m_shader->setNoiseTextureSize(m_noiseTexture->size() * GLRenderTarget::virtualScreenScale());
    m_shader->setTexturePosition(windowPosition * GLRenderTarget::virtualScreenScale());

    m_noiseTexture->bind();

    m_shader->setOffset(m_offset);
    m_shader->setModelViewProjectionMatrix(screenProjection);

    vbo->draw(GL_TRIANGLES, vboStart, blurRectCount);
    m_shader->unbind();
}

void BlurEffect::downSampleTexture(GLVertexBuffer *vbo, int blurRectCount)
{
    QMatrix4x4 modelViewProjectionMatrix;

    m_shader->bind(BlurShader::DownSampleType);
    m_shader->setOffset(m_offset);

    for (int i = 1; i <= m_downSampleIterations; i++) {
        modelViewProjectionMatrix.setToIdentity();
        modelViewProjectionMatrix.ortho(0, m_renderTextures[i].width(), m_renderTextures[i].height(), 0 , 0, 65535);

        m_shader->setModelViewProjectionMatrix(modelViewProjectionMatrix);
        m_shader->setTargetTextureSize(m_renderTextures[i].size());

        //Copy the image from this texture
        m_renderTextures[i - 1].bind();

        vbo->draw(GL_TRIANGLES, blurRectCount * i, blurRectCount);
        GLRenderTarget::popRenderTarget();
    }

    m_shader->unbind();
}

void BlurEffect::upSampleTexture(GLVertexBuffer *vbo, int blurRectCount)
{
    QMatrix4x4 modelViewProjectionMatrix;

    m_shader->bind(BlurShader::UpSampleType);
    m_shader->setOffset(m_offset);

    for (int i = m_downSampleIterations - 1; i >= 1; i--) {
        modelViewProjectionMatrix.setToIdentity();
        modelViewProjectionMatrix.ortho(0, m_renderTextures[i].width(), m_renderTextures[i].height(), 0 , 0, 65535);

        m_shader->setModelViewProjectionMatrix(modelViewProjectionMatrix);
        m_shader->setTargetTextureSize(m_renderTextures[i].size());

        //Copy the image from this texture
        m_renderTextures[i + 1].bind();

        vbo->draw(GL_TRIANGLES, blurRectCount * i, blurRectCount);
        GLRenderTarget::popRenderTarget();
    }

    m_shader->unbind();
}

void BlurEffect::copyScreenSampleTexture(GLVertexBuffer *vbo, int blurRectCount, QRegion blurShape, const QMatrix4x4 &screenProjection)
{
    m_shader->bind(BlurShader::CopySampleType);

    m_shader->setModelViewProjectionMatrix(screenProjection);
    m_shader->setTargetTextureSize(effects->virtualScreenSize());

    /*
     * This '1' sized adjustment is necessary do avoid windows affecting the blur that are
     * right next to this window.
     */
    m_shader->setBlurRect(blurShape.boundingRect().adjusted(1, 1, -1, -1), effects->virtualScreenSize());
    m_renderTextures.last().bind();

    vbo->draw(GL_TRIANGLES, 0, blurRectCount);
    GLRenderTarget::popRenderTarget();

    m_shader->unbind();
}

bool BlurEffect::isActive() const
{
    return !effects->isScreenLocked();
}

bool BlurEffect::blocksDirectScanout() const
{
    return false;
}

} // namespace KWin
