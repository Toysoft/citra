// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <QAbstractListModel>
#include <QDockWidget>

#include "video_core/gpu_debugger.h"
#include "video_core/debug_utils/debug_utils.h"

class GPUCommandListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    GPUCommandListModel(QObject* parent);

    int columnCount(const QModelIndex& parent = QModelIndex()) const;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

public slots:
    void OnPicaTraceFinished(const Pica::DebugUtils::PicaTrace& trace);

private:
    Pica::DebugUtils::PicaTrace pica_trace;
};

class GPUCommandListWidget : public QDockWidget
{
    Q_OBJECT

public:
    GPUCommandListWidget(QWidget* parent = 0);

public slots:
    void OnToggleTracing();

signals:
    void TracingFinished(const Pica::DebugUtils::PicaTrace&);

private:
    std::unique_ptr<Pica::DebugUtils::PicaTrace> pica_trace;
};
