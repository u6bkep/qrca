/*
 *  SPDX-FileCopyrightText: 2026 Ben Kepner <u6bkep@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVideoSink>

#include <memory>

class PipeWireCameraPrivate;

/**
 * Camera source that streams from PipeWire instead of opening V4L2 devices directly.
 *
 * PipeWire (with its libcamera plugin) is the only way to get processed frames from
 * cameras that need an ISP, such as Intel IPU3/IPU6 sensors, and it is what the
 * XDG camera portal hands out to sandboxed applications.
 *
 * Frames are delivered straight into @c videoSink, so this can drive a VideoOutput
 * (and anything else watching that sink, such as Prison.VideoScanner) without a
 * QMediaCaptureSession.
 *
 * When qrca is built without PipeWire, or no PipeWire camera is found, @c status
 * becomes Unavailable and the caller should fall back to QtMultimedia's Camera.
 */
class PipeWireCamera : public QObject
{
    Q_OBJECT

    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY camerasChanged)
    Q_PROPERTY(QString cameraId READ cameraId WRITE setCameraId NOTIFY cameraIdChanged)
    Q_PROPERTY(QString cameraDescription READ cameraDescription NOTIFY cameraIdChanged)
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    enum Status {
        Probing, ///< Still connecting to PipeWire / enumerating cameras
        Available, ///< At least one PipeWire camera exists
        Unavailable, ///< No PipeWire, no permission, or no cameras: use another backend
    };
    Q_ENUM(Status)

    explicit PipeWireCamera(QObject *parent = nullptr);
    ~PipeWireCamera() override;

    Status status() const;
    /// List of { id: node.name, description: node.description }, best camera first
    QVariantList cameras() const;

    QString cameraId() const;
    void setCameraId(const QString &id);
    QString cameraDescription() const;

    bool isActive() const;
    void setActive(bool active);

    QVideoSink *videoSink() const;
    void setVideoSink(QVideoSink *sink);

    QString errorString() const;

Q_SIGNALS:
    void statusChanged();
    void camerasChanged();
    void cameraIdChanged();
    void activeChanged();
    void videoSinkChanged();
    void errorStringChanged();

private Q_SLOTS:
    void onPortalAccessResponse(uint response, const QVariantMap &results);

private:
    friend class PipeWireCameraPrivate;
    std::unique_ptr<PipeWireCameraPrivate> d;
};
