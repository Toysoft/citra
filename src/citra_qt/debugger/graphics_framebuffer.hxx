// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <QDockWidget>

#include "graphics_breakpoint_observer.hxx"

class QComboBox;
class QLabel;
class QSpinBox;

class CSpinBox;

class GraphicsFramebufferWidget : public BreakPointObserverDock {
    Q_OBJECT

    using Event = Pica::DebugContext::Event;

    enum class Source {
        PicaTarget = 0,
        Custom = 1,

        // TODO: Add GPU framebuffer sources!
    };

    enum class Format {
        RGBA8    = 0,
        RGB8     = 1,
        RGBA5551 = 2,
        RGB565   = 3,
        RGBA4    = 4,
    };

public:
    GraphicsFramebufferWidget(std::shared_ptr<Pica::DebugContext> debug_context, QWidget* parent = nullptr);

public slots:
    void OnFramebufferSourceChanged(int new_value);
    void OnFramebufferAddressChanged(qint64 new_value);
    void OnFramebufferWidthChanged(int new_value);
    void OnFramebufferHeightChanged(int new_value);
    void OnFramebufferFormatChanged(int new_value);
    void OnUpdate();

private slots:
    void OnBreakPointHit(Pica::DebugContext::Event event, void* data) override;
    void OnResumed() override;

signals:
    void Update();

private:

    QComboBox* framebuffer_source_list;
    CSpinBox* framebuffer_address_control;
    QSpinBox* framebuffer_width_control;
    QSpinBox* framebuffer_height_control;
    QComboBox* framebuffer_format_control;

    QLabel* framebuffer_picture_label;

    Source framebuffer_source;
    unsigned framebuffer_address;
    unsigned framebuffer_width;
    unsigned framebuffer_height;
    Format framebuffer_format;
};
