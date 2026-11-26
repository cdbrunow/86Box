/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Sound gain dialog UI module.
 *
 * Authors: Cacodemon345
 *
 *          Copyright 2021-2022 Cacodemon345
 */
#include "qt_soundgain.hpp"
#include "ui_qt_soundgain.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/sound.h>
}

SoundGain::SoundGain(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SoundGain)
{
    ui->setupUi(this);
    ui->verticalSlider->setValue(sound_gain);
    sound_gain_orig = sound_gain;

    /* Drive sounds: one slider per kind, 0-200 %, heard as you drag */
    const struct {
        QString name;
        int    *value;
    } kinds[4] = {
        { tr("Floppy drives"), &fdd_sound_gain },
        { tr("Hard disks"), &hdd_sound_gain },
        { tr("CD-ROM drives"), &cdrom_sound_gain },
        { tr("Fan"), &fan_sound_gain },
    };

    auto *group = new QGroupBox(tr("Drive sounds"), this);
    auto *grid  = new QGridLayout(group);
    for (int i = 0; i < 4; i++) {
        levels[i] = { kinds[i].value, *kinds[i].value };

        auto *slider  = new QSlider(Qt::Horizontal, group);
        auto *percent = new QLabel(group);
        slider->setRange(0, 200);
        slider->setPageStep(10);
        slider->setValue(*kinds[i].value);
        percent->setText(QString("%1 %").arg(*kinds[i].value));
        percent->setMinimumWidth(percent->fontMetrics().horizontalAdvance("200 %"));
        percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        int *value = kinds[i].value;
        connect(slider, &QSlider::valueChanged, this, [value, percent](int v) {
            *value = v;
            percent->setText(QString("%1 %").arg(v));
        });

        grid->addWidget(new QLabel(kinds[i].name, group), i, 0);
        grid->addWidget(slider, i, 1);
        grid->addWidget(percent, i, 2);
    }

    /* Below the master gain, across the whole dialog; the .ui fixes the
       size for the master slider alone, so let it grow */
    ui->gridLayout->addWidget(group, ui->gridLayout->rowCount(), 0, 1, ui->gridLayout->columnCount());
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setMinimumSize(0, 0);
    setFixedSize(sizeHint().expandedTo(QSize(320, 0)));
}

SoundGain::~SoundGain()
{
    delete ui;
}

void
SoundGain::on_verticalSlider_valueChanged(int value)
{
    sound_gain = value;
}

void
SoundGain::on_SoundGain_accepted()
{
    config_save();
}

void
SoundGain::on_SoundGain_rejected()
{
    sound_gain = sound_gain_orig;
    for (auto &level : levels)
        *level.value = level.orig;
}
