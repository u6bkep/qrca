/*
 *  SPDX-FileCopyrightText: 2026 Ben Kepner <u6bkep@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pipewirecamera.h"

#include "config-qrca.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QVideoFrame>
#include <QVideoFrameFormat>

Q_LOGGING_CATEGORY(QRCA_PIPEWIRE, "org.kde.qrca.pipewire", QtWarningMsg)

#if HAVE_PIPEWIRE

#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>

#if HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

namespace
{
constexpr QLatin1StringView portalService("org.freedesktop.portal.Desktop");
constexpr QLatin1StringView portalPath("/org/freedesktop/portal/desktop");
constexpr QLatin1StringView portalCameraInterface("org.freedesktop.portal.Camera");

struct CameraNode {
    QString name; // node.name: stable across restarts, used as cameraId and stream target
    QString description;
    int priority = 0;
};

// Formats we can hand to Qt without conversion. libcamera produces NV12; USB webcams
// commonly produce YUY2. RGB formats are deliberately omitted: no camera we know of
// needs them and their byte-order mapping onto Qt's formats is untested.
QVideoFrameFormat::PixelFormat toQtPixelFormat(spa_video_format format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_NV12:
        return QVideoFrameFormat::Format_NV12;
    case SPA_VIDEO_FORMAT_NV21:
        return QVideoFrameFormat::Format_NV21;
    case SPA_VIDEO_FORMAT_YUY2:
        return QVideoFrameFormat::Format_YUYV;
    case SPA_VIDEO_FORMAT_UYVY:
        return QVideoFrameFormat::Format_UYVY;
    case SPA_VIDEO_FORMAT_I420:
        return QVideoFrameFormat::Format_YUV420P;
    case SPA_VIDEO_FORMAT_YV12:
        return QVideoFrameFormat::Format_YV12;
    default:
        return QVideoFrameFormat::Format_Invalid;
    }
}

struct PlaneLayout {
    int count = 0;
    int heights[3] = {};
    // Used when the producer packs all planes into a single spa_data:
    // stride of plane i relative to plane 0's stride, as numerator/denominator.
    int strideDiv[3] = {1, 1, 1};
};

PlaneLayout planeLayout(QVideoFrameFormat::PixelFormat format, int height)
{
    const int halfHeight = (height + 1) / 2;
    switch (format) {
    case QVideoFrameFormat::Format_NV12:
    case QVideoFrameFormat::Format_NV21:
        return {2, {height, halfHeight, 0}, {1, 1, 1}};
    case QVideoFrameFormat::Format_YUV420P:
    case QVideoFrameFormat::Format_YV12:
        return {3, {height, halfHeight, halfHeight}, {1, 2, 2}};
    case QVideoFrameFormat::Format_YUYV:
    case QVideoFrameFormat::Format_UYVY:
        return {1, {height, 0, 0}, {1, 1, 1}};
    default:
        return {};
    }
}
} // namespace

class PipeWireCameraPrivate
{
public:
    explicit PipeWireCameraPrivate(PipeWireCamera *q);
    ~PipeWireCameraPrivate();

    // --- main thread ---
    void start();
    void connectToPipeWire(int fd);
    void requestPortalAccess();
    void openPortalRemote();
    void fail(const QString &error);
    void setError(const QString &error);
    void publishCameras(const QList<CameraNode> &cameras, bool initialSyncDone);
    QString effectiveCameraId() const;
    void updateStream();
    void createStream(const QString &target);
    void destroyStream();

    PipeWireCamera *const q;
    PipeWireCamera::Status status = PipeWireCamera::Probing;
    QList<CameraNode> cameras;
    QString cameraId;
    QString streamTarget; // node the current stream is connected to, empty if none
    bool active = false;
    bool initialSyncDone = false;
    bool failed = false; // fail() is final: no reconnect, stay Unavailable
    QPointer<QVideoSink> sink;
    QString errorString;
    QString portalRequestPath;

    // --- shared, protected by the thread loop lock ---
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    pw_stream *stream = nullptr;
    // Bumped whenever the stream is created or destroyed. Results queued to the main thread carry
    // the generation they came from, so late errors or frames from an old stream are ignored.
    // Written only on the main thread under the loop lock; read by loop callbacks (which hold the lock).
    quint64 streamGeneration = 0;
    spa_hook coreListener = {};
    spa_hook registryListener = {};
    spa_hook streamListener = {};

    // --- PipeWire thread only ---
    QHash<uint32_t, CameraNode> nodes;
    int syncSeq = 0;
    bool syncDone = false;
    spa_video_info_raw videoInfo = {};
    QVideoFrameFormat frameFormat;
    // Buffers PipeWire didn't map (DMA-BUFs not flagged mappable) are mapped by us. These are kept
    // out of spa_data::data so PipeWire never sees, and never unmaps, memory it didn't map.
    struct Mapping {
        void *base;
        size_t size;
        const uint8_t *data; // base + mapoffset
    };
    QHash<const spa_data *, Mapping> mappings;
    const uint8_t *dataPointer(const spa_data &data) const;

    // Set while a frame is queued for the main thread; frames arriving meanwhile are dropped
    // so a slow consumer never builds up latency.
    std::atomic<bool> framePending = false;

    void publishNodesFromLoop();
    void onStreamFormat(const spa_pod *param);
    void onStreamProcess();
    void onAddBuffer(pw_buffer *buffer);
    void onRemoveBuffer(pw_buffer *buffer);
    QVideoFrame copyFrame(spa_buffer *buffer);

    static const pw_core_events coreEvents;
    static const pw_registry_events registryEvents;
    static const pw_stream_events streamEvents;
};

const pw_core_events PipeWireCameraPrivate::coreEvents = [] {
    pw_core_events events = {};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = [](void *data, uint32_t id, int seq) {
        auto *d = static_cast<PipeWireCameraPrivate *>(data);
        if (id == PW_ID_CORE && seq == d->syncSeq && !d->syncDone) {
            d->syncDone = true;
            d->publishNodesFromLoop();
        }
    };
    events.error = [](void *data, uint32_t id, int seq, int res, const char *message) {
        Q_UNUSED(seq);
        auto *d = static_cast<PipeWireCameraPrivate *>(data);
        qCWarning(QRCA_PIPEWIRE) << "PipeWire error on object" << id << spa_strerror(res) << message;
        if (id == PW_ID_CORE) {
            const QString error = QString::fromUtf8(message);
            QMetaObject::invokeMethod(
                d->q,
                [d, error] {
                    d->fail(error);
                },
                Qt::QueuedConnection);
        }
    };
    return events;
}();

const pw_registry_events PipeWireCameraPrivate::registryEvents = [] {
    pw_registry_events events = {};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = [](void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const spa_dict *props) {
        Q_UNUSED(permissions);
        Q_UNUSED(version);
        auto *d = static_cast<PipeWireCameraPrivate *>(data);
        if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
            return;
        }
        const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!mediaClass || std::strcmp(mediaClass, "Video/Source") != 0) {
            return;
        }
        // Virtual cameras may not set a role; anything that sets a different one isn't a camera.
        const char *role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);
        if (role && std::strcmp(role, "Camera") != 0) {
            return;
        }
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!name) {
            return;
        }
        const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *priority = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);

        CameraNode node;
        node.name = QString::fromUtf8(name);
        node.description = description ? QString::fromUtf8(description) : node.name;
        node.priority = priority ? atoi(priority) : 0;
        d->nodes.insert(id, node);
        if (d->syncDone) {
            d->publishNodesFromLoop();
        }
    };
    events.global_remove = [](void *data, uint32_t id) {
        auto *d = static_cast<PipeWireCameraPrivate *>(data);
        if (d->nodes.remove(id) && d->syncDone) {
            d->publishNodesFromLoop();
        }
    };
    return events;
}();

const pw_stream_events PipeWireCameraPrivate::streamEvents = [] {
    pw_stream_events events = {};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = [](void *data, pw_stream_state old, pw_stream_state state, const char *error) {
        Q_UNUSED(old);
        auto *d = static_cast<PipeWireCameraPrivate *>(data);
        qCDebug(QRCA_PIPEWIRE) << "stream state" << pw_stream_state_as_string(state);
        if (state == PW_STREAM_STATE_ERROR) {
            const QString message = QString::fromUtf8(error ? error : "unknown error");
            const quint64 generation = d->streamGeneration;
            QMetaObject::invokeMethod(
                d->q,
                [d, message, generation] {
                    if (generation == d->streamGeneration) {
                        d->setError(message);
                    }
                },
                Qt::QueuedConnection);
        }
    };
    events.param_changed = [](void *data, uint32_t id, const spa_pod *param) {
        if (id == SPA_PARAM_Format) {
            static_cast<PipeWireCameraPrivate *>(data)->onStreamFormat(param);
        }
    };
    events.add_buffer = [](void *data, pw_buffer *buffer) {
        static_cast<PipeWireCameraPrivate *>(data)->onAddBuffer(buffer);
    };
    events.remove_buffer = [](void *data, pw_buffer *buffer) {
        static_cast<PipeWireCameraPrivate *>(data)->onRemoveBuffer(buffer);
    };
    events.process = [](void *data) {
        static_cast<PipeWireCameraPrivate *>(data)->onStreamProcess();
    };
    return events;
}();

PipeWireCameraPrivate::PipeWireCameraPrivate(PipeWireCamera *q)
    : q(q)
{
}

PipeWireCameraPrivate::~PipeWireCameraPrivate()
{
    if (loop) {
        // Stop the thread first; afterwards nothing else touches these objects.
        pw_thread_loop_stop(loop);
    }
    if (stream) {
        pw_stream_destroy(stream);
    }
    if (registry) {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    if (core) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
    }
    if (context) {
        pw_context_destroy(context);
    }
    if (loop) {
        pw_thread_loop_destroy(loop);
    }
}

void PipeWireCameraPrivate::start()
{
    pw_init(nullptr, nullptr);

    // Sandboxed apps can't reach the PipeWire socket; they must ask the camera portal for a
    // connection, which also only exposes camera nodes. Unsandboxed apps talk to PipeWire directly,
    // as Firefox and OBS do. QRCA_PIPEWIRE_PORTAL=1/0 overrides the detection for testing.
    bool usePortal = QFileInfo::exists(QStringLiteral("/.flatpak-info")) || qEnvironmentVariableIsSet("SNAP");
    bool overrideOk = false;
    const int portalOverride = qEnvironmentVariableIntValue("QRCA_PIPEWIRE_PORTAL", &overrideOk);
    if (overrideOk) {
        usePortal = portalOverride != 0;
    }

    if (usePortal) {
#if HAVE_QTDBUS
        requestPortalAccess();
#else
        fail(QStringLiteral("Camera portal requires D-Bus support"));
#endif
    } else {
        connectToPipeWire(-1);
    }
}

void PipeWireCameraPrivate::connectToPipeWire(int fd)
{
    loop = pw_thread_loop_new("qrca-camera", nullptr);
    if (!loop) {
        if (fd >= 0) {
            close(fd);
        }
        fail(QStringLiteral("Could not create PipeWire thread"));
        return;
    }
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (!context) {
        if (fd >= 0) {
            close(fd);
        }
        fail(QStringLiteral("Could not create PipeWire context"));
        return;
    }
    // pw_context_connect_fd takes ownership of fd, even on failure.
    core = fd >= 0 ? pw_context_connect_fd(context, fd, nullptr, 0) : pw_context_connect(context, nullptr, 0);
    if (!core) {
        fail(QStringLiteral("Could not connect to PipeWire"));
        return;
    }
    pw_core_add_listener(core, &coreListener, &coreEvents, this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
    // The registry replays all existing globals before answering this sync, so the
    // matching done event marks the end of the initial camera enumeration.
    syncSeq = pw_core_sync(core, PW_ID_CORE, 0);

    if (pw_thread_loop_start(loop) < 0) {
        fail(QStringLiteral("Could not start PipeWire thread"));
    }
}

#if HAVE_QTDBUS
void PipeWireCameraPrivate::requestPortalAccess()
{
    auto bus = QDBusConnection::sessionBus();
    const QString token = QStringLiteral("qrca%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    portalRequestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    // Subscribe before calling, so a fast response can't be missed.
    bus.connect(portalService,
                portalRequestPath,
                QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"),
                q,
                SLOT(onPortalAccessResponse(uint, QVariantMap)));

    auto message = QDBusMessage::createMethodCall(portalService, portalPath, portalCameraInterface, QStringLiteral("AccessCamera"));
    message << QVariantMap{{QStringLiteral("handle_token"), token}};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), q);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, q, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            fail(QStringLiteral("Camera portal unavailable: %1").arg(reply.error().message()));
        }
    });
}

void PipeWireCameraPrivate::openPortalRemote()
{
    auto message = QDBusMessage::createMethodCall(portalService, portalPath, portalCameraInterface, QStringLiteral("OpenPipeWireRemote"));
    message << QVariantMap();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), q);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, q, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        if (reply.isError()) {
            fail(QStringLiteral("Could not open camera connection: %1").arg(reply.error().message()));
            return;
        }
        if (!reply.value().isValid()) {
            fail(QStringLiteral("Could not open camera connection: the camera portal returned no connection"));
            return;
        }
        // QDBusUnixFileDescriptor owns its fd; PipeWire needs one of its own.
        const int fd = fcntl(reply.value().fileDescriptor(), F_DUPFD_CLOEXEC, 3);
        if (fd < 0) {
            fail(QStringLiteral("Could not open camera connection"));
            return;
        }
        connectToPipeWire(fd);
    });
}
#else
void PipeWireCameraPrivate::requestPortalAccess()
{
}
void PipeWireCameraPrivate::openPortalRemote()
{
}
#endif

void PipeWireCameraPrivate::fail(const QString &error)
{
    qCInfo(QRCA_PIPEWIRE) << "PipeWire camera unavailable:" << error;
    failed = true;
    setError(error);
    if (status != PipeWireCamera::Unavailable) {
        status = PipeWireCamera::Unavailable;
        Q_EMIT q->statusChanged();
    }
    updateStream();
}

void PipeWireCameraPrivate::setError(const QString &error)
{
    if (errorString != error) {
        errorString = error;
        Q_EMIT q->errorStringChanged();
    }
}

void PipeWireCameraPrivate::publishNodesFromLoop()
{
    QList<CameraNode> list = nodes.values();
    std::stable_sort(list.begin(), list.end(), [](const CameraNode &a, const CameraNode &b) {
        return a.priority > b.priority;
    });
    QMetaObject::invokeMethod(
        q,
        [this, list] {
            publishCameras(list, true);
        },
        Qt::QueuedConnection);
}

void PipeWireCameraPrivate::publishCameras(const QList<CameraNode> &newCameras, bool syncDone)
{
    if (failed) {
        return;
    }
    const QString previousDescription = q->cameraDescription();
    bool changed = cameras.size() != newCameras.size();
    for (qsizetype i = 0; !changed && i < cameras.size(); ++i) {
        changed = cameras[i].name != newCameras[i].name || cameras[i].description != newCameras[i].description;
    }
    cameras = newCameras;
    initialSyncDone = initialSyncDone || syncDone;
    if (changed) {
        Q_EMIT q->camerasChanged();
    }
    if (q->cameraDescription() != previousDescription) {
        Q_EMIT q->cameraIdChanged();
    }

    if (initialSyncDone) {
        const auto newStatus = cameras.isEmpty() ? PipeWireCamera::Unavailable : PipeWireCamera::Available;
        if (newStatus != status) {
            status = newStatus;
            Q_EMIT q->statusChanged();
        }
    }
    updateStream();
}

QString PipeWireCameraPrivate::effectiveCameraId() const
{
    for (const auto &camera : cameras) {
        if (camera.name == cameraId) {
            return cameraId;
        }
    }
    return cameras.isEmpty() ? QString() : cameras.first().name;
}

void PipeWireCameraPrivate::updateStream()
{
    const QString target = (active && status == PipeWireCamera::Available) ? effectiveCameraId() : QString();
    if (target == streamTarget) {
        return;
    }
    qCDebug(QRCA_PIPEWIRE) << "updateStream" << q << "active" << active << "status" << status << "from" << streamTarget << "to" << target;
    destroyStream();
    if (!target.isEmpty()) {
        createStream(target);
    }
}

void PipeWireCameraPrivate::createStream(const QString &target)
{
    pw_thread_loop_lock(loop);

    const QByteArray targetUtf8 = target.toUtf8();
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                             "Video",
                                             PW_KEY_MEDIA_CATEGORY,
                                             "Capture",
                                             PW_KEY_MEDIA_ROLE,
                                             "Camera",
                                             PW_KEY_TARGET_OBJECT,
                                             targetUtf8.constData(),
                                             nullptr);
    ++streamGeneration;
    stream = pw_stream_new(core, "qrca", props);
    if (!stream) {
        pw_thread_loop_unlock(loop);
        setError(QStringLiteral("Could not create camera stream"));
        return;
    }
    pw_stream_add_listener(stream, &streamListener, &streamEvents, this);

    uint8_t podBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const spa_rectangle defaultSize = SPA_RECTANGLE(1280, 720);
    const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maxSize = SPA_RECTANGLE(4096, 4096);
    const spa_fraction defaultRate = SPA_FRACTION(30, 1);
    const spa_fraction minRate = SPA_FRACTION(0, 1);
    const spa_fraction maxRate = SPA_FRACTION(120, 1);
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
                                                                         SPA_TYPE_OBJECT_Format,
                                                                         SPA_PARAM_EnumFormat,
                                                                         SPA_FORMAT_mediaType,
                                                                         SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                                                         SPA_FORMAT_mediaSubtype,
                                                                         SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                                                         SPA_FORMAT_VIDEO_format,
                                                                         SPA_POD_CHOICE_ENUM_Id(7,
                                                                                                SPA_VIDEO_FORMAT_NV12,
                                                                                                SPA_VIDEO_FORMAT_NV12,
                                                                                                SPA_VIDEO_FORMAT_YUY2,
                                                                                                SPA_VIDEO_FORMAT_NV21,
                                                                                                SPA_VIDEO_FORMAT_UYVY,
                                                                                                SPA_VIDEO_FORMAT_I420,
                                                                                                SPA_VIDEO_FORMAT_YV12),
                                                                         SPA_FORMAT_VIDEO_size,
                                                                         SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
                                                                         SPA_FORMAT_VIDEO_framerate,
                                                                         SPA_POD_CHOICE_RANGE_Fraction(&defaultRate, &minRate, &maxRate)));

    const int res = pw_stream_connect(stream,
                                      PW_DIRECTION_INPUT,
                                      PW_ID_ANY,
                                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                      params,
                                      1);
    pw_thread_loop_unlock(loop);

    if (res < 0) {
        setError(QStringLiteral("Could not connect to camera: %1").arg(QString::fromUtf8(spa_strerror(res))));
        destroyStream();
        return;
    }
    streamTarget = target;
    setError(QString());
}

void PipeWireCameraPrivate::destroyStream()
{
    if (!stream) {
        streamTarget.clear();
        return;
    }
    pw_thread_loop_lock(loop);
    ++streamGeneration;
    spa_hook_remove(&streamListener);
    // Buffers are released synchronously here, while our listener is gone; drop our own mappings.
    pw_stream_destroy(stream);
    stream = nullptr;
    for (const auto &mapping : std::as_const(mappings)) {
        munmap(mapping.base, mapping.size);
    }
    mappings.clear();
    pw_thread_loop_unlock(loop);
    streamTarget.clear();
}

void PipeWireCameraPrivate::onStreamFormat(const spa_pod *param)
{
    if (!param) {
        return;
    }
    spa_video_info_raw info = {};
    if (spa_format_video_raw_parse(param, &info) < 0) {
        return;
    }
    const auto pixelFormat = toQtPixelFormat(info.format);
    if (pixelFormat == QVideoFrameFormat::Format_Invalid || info.size.width == 0 || info.size.height == 0) {
        pw_stream_set_error(stream, -EINVAL, "unsupported video format");
        return;
    }
    videoInfo = info;
    frameFormat = QVideoFrameFormat(QSize(info.size.width, info.size.height), pixelFormat);
    switch (info.color_matrix) {
    case SPA_VIDEO_COLOR_MATRIX_BT709:
        frameFormat.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
        break;
    case SPA_VIDEO_COLOR_MATRIX_BT2020:
        frameFormat.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020);
        break;
    default:
        frameFormat.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
        break;
    }
    frameFormat.setColorRange(info.color_range == SPA_VIDEO_COLOR_RANGE_0_255 ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    qCDebug(QRCA_PIPEWIRE) << "negotiated format" << pixelFormat << info.size.width << "x" << info.size.height;

    uint8_t podBuffer[256];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
                                                                         SPA_TYPE_OBJECT_ParamBuffers,
                                                                         SPA_PARAM_Buffers,
                                                                         SPA_PARAM_BUFFERS_dataType,
                                                                         SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_DmaBuf))));
    pw_stream_update_params(stream, params, 1);
}

void PipeWireCameraPrivate::onAddBuffer(pw_buffer *buffer)
{
    // PW_STREAM_FLAG_MAP_BUFFERS maps MemFd buffers, but DmaBuf only when the producer marks it
    // mappable. libcamera's buffers are CPU-accessible dma-bufs, so map whatever is left ourselves.
    spa_buffer *buf = buffer->buffer;
    for (uint32_t i = 0; i < buf->n_datas; ++i) {
        spa_data &data = buf->datas[i];
        if (data.data || data.fd < 0 || (data.type != SPA_DATA_DmaBuf && data.type != SPA_DATA_MemFd)) {
            continue;
        }
        const size_t size = data.mapoffset + data.maxsize;
        void *base = mmap(nullptr, size, PROT_READ, MAP_SHARED, data.fd, 0);
        if (base == MAP_FAILED) {
            qCWarning(QRCA_PIPEWIRE) << "could not map camera buffer:" << strerror(errno);
            continue;
        }
        mappings.insert(&data, {base, size, static_cast<const uint8_t *>(base) + data.mapoffset});
    }
}

void PipeWireCameraPrivate::onRemoveBuffer(pw_buffer *buffer)
{
    spa_buffer *buf = buffer->buffer;
    for (uint32_t i = 0; i < buf->n_datas; ++i) {
        const auto it = mappings.find(&buf->datas[i]);
        if (it != mappings.end()) {
            munmap(it->base, it->size);
            mappings.erase(it);
        }
    }
}

const uint8_t *PipeWireCameraPrivate::dataPointer(const spa_data &data) const
{
    if (data.data) {
        return static_cast<const uint8_t *>(data.data);
    }
    const auto it = mappings.constFind(&data);
    return it != mappings.cend() ? it->data : nullptr;
}

void PipeWireCameraPrivate::onStreamProcess()
{
    pw_buffer *buffer = nullptr;
    // Drain to the newest buffer: older ones are stale for a viewfinder.
    while (pw_buffer *next = pw_stream_dequeue_buffer(stream)) {
        if (buffer) {
            pw_stream_queue_buffer(stream, buffer);
        }
        buffer = next;
    }
    if (!buffer) {
        return;
    }
    if (framePending.load()) {
        pw_stream_queue_buffer(stream, buffer);
        return;
    }
    QVideoFrame frame = copyFrame(buffer->buffer);
    pw_stream_queue_buffer(stream, buffer);
    if (!frame.isValid()) {
        return;
    }

    framePending = true;
    const quint64 generation = streamGeneration;
    QMetaObject::invokeMethod(
        q,
        [this, frame, generation] {
            framePending = false;
            // Drop a frame from a stream that has since been stopped or replaced: by now the sink
            // may belong to QtMultimedia's Camera or show a different camera.
            if (sink && generation == streamGeneration) {
                sink->setVideoFrame(frame);
            }
        },
        Qt::QueuedConnection);
}

QVideoFrame PipeWireCameraPrivate::copyFrame(spa_buffer *buffer)
{
    if (!frameFormat.isValid() || buffer->n_datas == 0) {
        return {};
    }
    const spa_data &first = buffer->datas[0];
    const uint8_t *firstData = dataPointer(first);
    if (!firstData || !first.chunk || (first.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        return {};
    }

    const int height = int(videoInfo.size.height);
    const PlaneLayout layout = planeLayout(frameFormat.pixelFormat(), height);
    if (layout.count == 0) {
        return {};
    }

    struct SourcePlane {
        const uint8_t *data;
        int stride;
    };
    SourcePlane source[3] = {};

    if (buffer->n_datas >= uint32_t(layout.count)) {
        // One spa_data per plane (possibly sharing one fd with different offsets).
        for (int i = 0; i < layout.count; ++i) {
            const spa_data &data = buffer->datas[i];
            const uint8_t *base = dataPointer(data);
            if (!base || !data.chunk || data.chunk->stride <= 0) {
                return {};
            }
            const size_t needed = size_t(data.chunk->stride) * layout.heights[i];
            if (data.chunk->offset + needed > data.maxsize) {
                return {};
            }
            source[i] = {base + data.chunk->offset, data.chunk->stride};
        }
    } else {
        // All planes packed back to back in a single spa_data.
        const int stride0 = first.chunk->stride;
        if (stride0 <= 0) {
            return {};
        }
        size_t offset = first.chunk->offset;
        for (int i = 0; i < layout.count; ++i) {
            const int stride = stride0 / layout.strideDiv[i];
            const size_t needed = size_t(stride) * layout.heights[i];
            if (offset + needed > first.maxsize) {
                return {};
            }
            source[i] = {firstData + offset, stride};
            offset += needed;
        }
    }

    // CPU reads of dma-bufs must be bracketed so caches are coherent with the device's writes.
    auto syncDmaBuf = [&](uint64_t flags) {
        for (uint32_t i = 0; i < buffer->n_datas; ++i) {
            if (buffer->datas[i].type == SPA_DATA_DmaBuf && buffer->datas[i].fd >= 0) {
                dma_buf_sync sync = {flags | DMA_BUF_SYNC_READ};
                ioctl(buffer->datas[i].fd, DMA_BUF_IOCTL_SYNC, &sync);
            }
        }
    };

    QVideoFrame frame(frameFormat);
    if (!frame.map(QVideoFrame::WriteOnly)) {
        return {};
    }
    syncDmaBuf(DMA_BUF_SYNC_START);
    for (int i = 0; i < layout.count; ++i) {
        uchar *dst = frame.bits(i);
        const int dstStride = frame.bytesPerLine(i);
        const int rowBytes = std::min(dstStride, source[i].stride);
        const uint8_t *src = source[i].data;
        for (int row = 0; row < layout.heights[i]; ++row) {
            std::memcpy(dst + size_t(row) * dstStride, src + size_t(row) * source[i].stride, rowBytes);
        }
    }
    syncDmaBuf(DMA_BUF_SYNC_END);
    frame.unmap();
    return frame;
}

PipeWireCamera::PipeWireCamera(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<PipeWireCameraPrivate>(this))
{
    // Defer so QML bindings (videoSink, active, cameraId) are in place before anything starts.
    QMetaObject::invokeMethod(
        this,
        [this] {
            d->start();
        },
        Qt::QueuedConnection);
}

PipeWireCamera::~PipeWireCamera() = default;

void PipeWireCamera::onPortalAccessResponse(uint response, const QVariantMap &results)
{
    Q_UNUSED(results);
#if HAVE_QTDBUS
    QDBusConnection::sessionBus().disconnect(portalService,
                                             d->portalRequestPath,
                                             QStringLiteral("org.freedesktop.portal.Request"),
                                             QStringLiteral("Response"),
                                             this,
                                             SLOT(onPortalAccessResponse(uint, QVariantMap)));
#endif
    if (response != 0) {
        d->fail(QStringLiteral("Camera access was denied"));
        return;
    }
    d->openPortalRemote();
}

void PipeWireCamera::setCameraId(const QString &id)
{
    if (d->cameraId == id) {
        return;
    }
    const QString previous = d->effectiveCameraId();
    d->cameraId = id;
    if (d->effectiveCameraId() != previous) {
        Q_EMIT cameraIdChanged();
    }
    d->updateStream();
}

void PipeWireCamera::setActive(bool active)
{
    if (d->active == active) {
        return;
    }
    d->active = active;
    Q_EMIT activeChanged();
    d->updateStream();
}

#else // !HAVE_PIPEWIRE

class PipeWireCameraPrivate
{
public:
    PipeWireCamera::Status status = PipeWireCamera::Unavailable;
    QVariantList cameras;
    QString cameraId;
    bool active = false;
    QPointer<QVideoSink> sink;
    QString errorString;
    QString effectiveCameraId() const
    {
        return cameraId;
    }
};

PipeWireCamera::PipeWireCamera(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<PipeWireCameraPrivate>())
{
}

PipeWireCamera::~PipeWireCamera() = default;

void PipeWireCamera::onPortalAccessResponse(uint, const QVariantMap &)
{
}

void PipeWireCamera::setCameraId(const QString &id)
{
    if (d->cameraId != id) {
        d->cameraId = id;
        Q_EMIT cameraIdChanged();
    }
}

void PipeWireCamera::setActive(bool active)
{
    if (d->active != active) {
        d->active = active;
        Q_EMIT activeChanged();
    }
}

#endif // HAVE_PIPEWIRE

PipeWireCamera::Status PipeWireCamera::status() const
{
    return d->status;
}

QVariantList PipeWireCamera::cameras() const
{
#if HAVE_PIPEWIRE
    QVariantList list;
    for (const auto &camera : d->cameras) {
        list.append(QVariantMap{{QStringLiteral("id"), camera.name}, {QStringLiteral("description"), camera.description}});
    }
    return list;
#else
    return d->cameras;
#endif
}

QString PipeWireCamera::cameraId() const
{
    return d->effectiveCameraId();
}

QString PipeWireCamera::cameraDescription() const
{
#if HAVE_PIPEWIRE
    const QString id = d->effectiveCameraId();
    for (const auto &camera : d->cameras) {
        if (camera.name == id) {
            return camera.description;
        }
    }
#endif
    return {};
}

bool PipeWireCamera::isActive() const
{
    return d->active;
}

QVideoSink *PipeWireCamera::videoSink() const
{
    return d->sink;
}

void PipeWireCamera::setVideoSink(QVideoSink *sink)
{
    if (d->sink != sink) {
        d->sink = sink;
        Q_EMIT videoSinkChanged();
    }
}

QString PipeWireCamera::errorString() const
{
    return d->errorString;
}

#include "moc_pipewirecamera.cpp"
