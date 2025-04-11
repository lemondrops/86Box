/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Hard disk dialog code.
 *
 *
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *          Cacodemon345
 *
 *          Copyright 2021 Joakim L. Gilje
 *          Copyright 2022 Cacodemon345
 */
#include "qt_harddiskdialog.hpp"
#include "qt_deviceconfig.hpp"
// #include "ui_qt_harddiskdialog.h"
#include "ui_qt_newharddiskdialog.h"

extern "C" {
#ifdef __unix__
#include <unistd.h>
#endif
#include <86box/86box.h>
#include <86box/hdd.h>
#include "../disk/minivhd/minivhd.h"
}

#include <thread>

#include <QMessageBox>
#include <QDebug>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringBuilder>

#include "qt_harddrive_common.hpp"
#include "qt_settings_bus_tracking.hpp"
#include "qt_models_common.hpp"
#include "qt_util.hpp"

HarddiskDialog::HarddiskDialog(bool existing, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::HarddiskDialog)
{
    ui->setupUi(this);

    ui->progressBar->setHidden(true);

    auto *model = ui->comboBoxFormat->model();
    model->insertRows(0, 6);
    model->setData(model->index(0, 0), tr("Raw image (.img)"));
    model->setData(model->index(1, 0), tr("HDI image (.hdi)"));
    model->setData(model->index(2, 0), tr("HDX image (.hdx)"));
    model->setData(model->index(3, 0), tr("Fixed-size VHD (.vhd)"));
    model->setData(model->index(4, 0), tr("Dynamic-size VHD (.vhd)"));
    model->setData(model->index(5, 0), tr("Differencing VHD (.vhd)"));

    model = ui->comboBoxBlockSize->model();
    model->insertRows(0, 2);
    model->setData(model->index(0, 0), tr("Large blocks (2 MB)"));
    model->setData(model->index(1, 0), tr("Small blocks (512 KB)"));

    ui->comboBoxBlockSize->hide();
    ui->labelBlockSize->hide();

    Harddrives::populateBuses(ui->comboBoxBus->model());
    ui->comboBoxBus->setCurrentIndex(3);

    // First, create the model containing the list of manufacturer names
    const auto presetManufacturersModel = createManufacturersModel();
    ui->comboBoxManPreset->setModel(presetManufacturersModel);

    // Then create the main model that is populated using the json data
    // We don't directly assign this model to any widgets
    const auto presetModel = createHddPresetModel();

    // Instead, the proxy model is used for sorting the hdd preset model list
    // and assigned to the combo box
    hddPresetModel = createHddPresetProxyModel(presetModel);
    ui->comboBoxModelPreset->setModel(hddPresetModel);

    // Signal assignments for the preset widgets
    connect(ui->comboBoxManPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &HarddiskDialog::populatePresetModels);
    connect(ui->comboBoxModelPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &HarddiskDialog::setPresetParameters);
    connect(ui->comboBoxModelPreset, QOverload<const QString &>::of(&QComboBox::currentTextChanged), this, &HarddiskDialog::presetTextChanged);

    ui->lineEditSize->setValidator(new QIntValidator());
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    filters = QStringList({ tr("Raw image") % util::DlgFilter({ "img" }, true),
                          tr("HDI image") % util::DlgFilter({ "hdi" }, true),
                          tr("HDX image") % util::DlgFilter({ "hdx" }, true),
                          tr("Fixed-size VHD") % util::DlgFilter({ "vhd" }, true),
                          tr("Dynamic-size VHD") % util::DlgFilter({ "vhd" }, true),
                          tr("Differencing VHD") % util::DlgFilter({ "vhd" }, true) });

    if (existing) {
        ui->fileField->setFilter(tr("Hard disk images") % util::DlgFilter({ "hd?", "im?", "vhd" }) % tr("All files") % util::DlgFilter({ "*" }, true));

        setWindowTitle(tr("Add Existing Hard Disk"));
        ui->lineEditCylinders->setEnabled(false);
        ui->lineEditHeads->setEnabled(false);
        ui->lineEditSectors->setEnabled(false);
        ui->lineEditSize->setEnabled(false);
        ui->sizeSlider->setEnabled(false);
        ui->comboBoxManPreset->setEnabled(false);
        ui->comboBoxModelPreset->setEnabled(false);

        ui->comboBoxFormat->hide();
        ui->labelFormat->hide();

        connect(ui->fileField, &FileField::fileSelected, this, &HarddiskDialog::onExistingFileSelected);
    } else {
        ui->fileField->setFilter(filters.join(";;"));

        setWindowTitle(tr("Add New Hard Disk"));
        ui->fileField->setCreateFile(true);

        // Enable the OK button as long as the filename length is non-zero
        connect(ui->fileField, &FileField::fileTextEntered, this, [this] {
            ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled((this->fileName().length() > 0));
        });

        connect(ui->fileField, &FileField::fileSelected, this, [this] {
            int filter = filters.indexOf(ui->fileField->selectedFilter());
            if (filter > -1)
                ui->comboBoxFormat->setCurrentIndex(filter);
            ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        });
        // Set the default format to Dynamic-size VHD. Do it last after everything is set up
        // so the currentIndexChanged signal can do what is needed
        ui->comboBoxFormat->setCurrentIndex(DEFAULT_DISK_FORMAT);
        ui->fileField->setselectedFilter(filters.value(DEFAULT_DISK_FORMAT));
    }
    // QWindowsStyle really wants to make this window larger than it needs to be
    resize(minimumSize());
    // Signals for the size slider
    connect(ui->sizeSlider, &QSlider::valueChanged, this, &HarddiskDialog::sizeSliderChanged);
    connect(ui->sizeSlider, &QSlider::sliderMoved,  this, &HarddiskDialog::sizeSliderMoved);
    connect(ui->sizeSlider, &QSlider::rangeChanged, this, &HarddiskDialog::sizeSliderRangeChanged);
}

HarddiskDialog::~HarddiskDialog()
{
    delete ui;
}

uint8_t
HarddiskDialog::bus() const
{
    return static_cast<uint8_t>(ui->comboBoxBus->currentData().toUInt());
}

uint8_t
HarddiskDialog::channel() const
{
    return static_cast<uint8_t>(ui->comboBoxChannel->currentData().toUInt());
}

QString
HarddiskDialog::fileName() const
{
    return ui->fileField->fileName();
}

uint32_t
HarddiskDialog::speed() const
{
    return static_cast<uint32_t>(ui->comboBoxSpeed->currentData().toUInt());
}

void
HarddiskDialog::on_comboBoxFormat_currentIndexChanged(int index)
{
    bool enabled;
    if (index == 5) { /* They switched to a diff VHD; disable the geometry fields. */
        enabled = false;
        ui->lineEditCylinders->setText(tr("(N/A)"));
        ui->lineEditHeads->setText(tr("(N/A)"));
        ui->lineEditSectors->setText(tr("(N/A)"));
        ui->lineEditSize->setText(tr("(N/A)"));
        ui->sizeSlider->setEnabled(false);
    } else {
        enabled = true;
        ui->sizeSlider->setEnabled(true);
        ui->lineEditCylinders->setText(QString::number(cylinders_));
        ui->lineEditHeads->setText(QString::number(heads_));
        ui->lineEditSectors->setText(QString::number(sectors_));
        recalcSize();
    }
    ui->lineEditCylinders->setEnabled(enabled);
    ui->lineEditHeads->setEnabled(enabled);
    ui->lineEditSectors->setEnabled(enabled);
    ui->lineEditSize->setEnabled(enabled);

    if (index < 4) {
        ui->comboBoxBlockSize->hide();
        ui->labelBlockSize->hide();
    } else {
        ui->comboBoxBlockSize->show();
        ui->labelBlockSize->show();
    }
    ui->fileField->setselectedFilter(filters.value(index));
}

/* If the disk geometry requested in the 86Box GUI is not compatible with the internal VHD geometry,
 * we adjust it to the next-largest size that is compatible. On average, this will be a difference
 * of about 21 MB, and should only be necessary for VHDs larger than 31.5 GB, so should never be more
 * than a tenth of a percent change in size.
 */
static void
adjust_86box_geometry_for_vhd(MVHDGeom *_86box_geometry, MVHDGeom *vhd_geometry)
{
    if (_86box_geometry->cyl <= 65535) {
        vhd_geometry->cyl   = _86box_geometry->cyl;
        vhd_geometry->heads = _86box_geometry->heads;
        vhd_geometry->spt   = _86box_geometry->spt;
        return;
    }

    int desired_sectors = _86box_geometry->cyl * _86box_geometry->heads * _86box_geometry->spt;
    if (desired_sectors > 267321600)
        desired_sectors = 267321600;

    int remainder = desired_sectors % 85680; /* 8560 is the LCM of 1008 (63*16) and 4080 (255*16) */
    if (remainder > 0)
        desired_sectors += (85680 - remainder);

    _86box_geometry->cyl   = desired_sectors / (16 * 63);
    _86box_geometry->heads = 16;
    _86box_geometry->spt   = 63;

    vhd_geometry->cyl   = desired_sectors / (16 * 255);
    vhd_geometry->heads = 16;
    vhd_geometry->spt   = 255;
}

static HarddiskDialog *callbackPtr = nullptr;
static MVHDGeom
create_drive_vhd_fixed(const QString &fileName, HarddiskDialog *p, uint16_t cyl, uint8_t heads, uint8_t spt)
{
    MVHDGeom _86box_geometry = {
        .cyl   = cyl,
        .heads = heads,
        .spt   = spt
    };
    MVHDGeom vhd_geometry;
    adjust_86box_geometry_for_vhd(&_86box_geometry, &vhd_geometry);

    int        vhd_error     = 0;
    QByteArray filenameBytes = fileName.toUtf8();
    callbackPtr              = p;
    MVHDMeta *vhd            = mvhd_create_fixed(filenameBytes.data(), vhd_geometry, &vhd_error, [](uint32_t current_sector, uint32_t total_sectors) {
        callbackPtr->fileProgress((current_sector * 100) / total_sectors);
    });
    callbackPtr              = nullptr;

    if (vhd == NULL) {
        _86box_geometry.cyl   = 0;
        _86box_geometry.heads = 0;
        _86box_geometry.spt   = 0;
    } else {
        mvhd_close(vhd);
    }

    return _86box_geometry;
}

static MVHDGeom
create_drive_vhd_dynamic(const QString &fileName, uint16_t cyl, uint8_t heads, uint8_t spt, int blocksize)
{
    MVHDGeom _86box_geometry = {
        .cyl   = cyl,
        .heads = heads,
        .spt   = spt
    };
    MVHDGeom vhd_geometry;
    adjust_86box_geometry_for_vhd(&_86box_geometry, &vhd_geometry);
    int                 vhd_error     = 0;
    QByteArray          filenameBytes = fileName.toUtf8();
    MVHDCreationOptions options;
    options.block_size_in_sectors = blocksize;
    options.path                  = filenameBytes.data();
    options.size_in_bytes         = 0;
    options.geometry              = vhd_geometry;
    options.type                  = MVHD_TYPE_DYNAMIC;

    MVHDMeta *vhd = mvhd_create_ex(options, &vhd_error);
    if (vhd == NULL) {
        _86box_geometry.cyl   = 0;
        _86box_geometry.heads = 0;
        _86box_geometry.spt   = 0;
    } else {
        mvhd_close(vhd);
    }

    return _86box_geometry;
}

static MVHDGeom
create_drive_vhd_diff(const QString &fileName, const QString &parentFileName, int blocksize)
{
    int                 vhd_error           = 0;
    QByteArray          filenameBytes       = fileName.toUtf8();
    QByteArray          parentFilenameBytes = parentFileName.toUtf8();
    MVHDCreationOptions options;
    options.block_size_in_sectors = blocksize;
    options.path                  = filenameBytes.data();
    options.parent_path           = parentFilenameBytes.data();
    options.type                  = MVHD_TYPE_DIFF;

    MVHDMeta *vhd = mvhd_create_ex(options, &vhd_error);
    MVHDGeom  vhd_geometry;
    if (vhd == NULL) {
        vhd_geometry.cyl   = 0;
        vhd_geometry.heads = 0;
        vhd_geometry.spt   = 0;
    } else {
        vhd_geometry = mvhd_get_geometry(vhd);

        if (vhd_geometry.spt > 63) {
            vhd_geometry.cyl   = mvhd_calc_size_sectors(&vhd_geometry) / (16 * 63);
            vhd_geometry.heads = 16;
            vhd_geometry.spt   = 63;
        }

        mvhd_close(vhd);
    }

    return vhd_geometry;
}

void
HarddiskDialog::onCreateNewFile()
{

    for (auto &curObject : children()) {
        if (qobject_cast<QWidget *>(curObject))
            qobject_cast<QWidget *>(curObject)->setDisabled(true);
    }

    ui->progressBar->setEnabled(true);
    setResult(QDialog::Rejected);
    uint32_t sector_size = 512;
    quint64  size        = (static_cast<uint64_t>(cylinders_) * static_cast<uint64_t>(heads_) * static_cast<uint64_t>(sectors_) * static_cast<uint64_t>(sector_size));
    if (size > 0x1FFFFFFE00LL) {
        QMessageBox::critical(this, tr("Disk image too large"), tr("Disk images cannot be larger than 127 GB."));
        return;
    }

    int      img_format = ui->comboBoxFormat->currentIndex();
    uint32_t zero       = 0;
    uint32_t base       = 0x1000;

    auto    fileName = ui->fileField->fileName();
    QString expectedSuffix;
    switch (img_format) {
        case IMG_FMT_HDI:
            expectedSuffix = "hdi";
            break;
        case IMG_FMT_HDX:
            expectedSuffix = "hdx";
            break;
        case IMG_FMT_VHD_FIXED:
        case IMG_FMT_VHD_DYNAMIC:
        case IMG_FMT_VHD_DIFF:
            expectedSuffix = "vhd";
            break;
    }
    if (!expectedSuffix.isEmpty()) {
        QFileInfo fileInfo(fileName);
        if (fileInfo.suffix().compare(expectedSuffix, Qt::CaseInsensitive) != 0) {
            fileName = QString("%1.%2").arg(fileName, expectedSuffix);
            ui->fileField->setFileName(fileName);
        }
    }
    QFileInfo fi(fileName);
    fileName = (fi.isRelative() && !fi.filePath().isEmpty()) ? usr_path + fi.filePath() : fi.filePath();
    ui->fileField->setFileName(fileName);

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, tr("Unable to write file"), tr("Make sure the file is being saved to a writable directory."));
        return;
    }

    if (img_format == IMG_FMT_HDI) { /* HDI file */
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        if (size >= 0x100000000LL) {
            QMessageBox::critical(this, tr("Disk image too large"), tr("HDI disk images cannot be larger than 4 GB."));
            return;
        }
        uint32_t s = static_cast<uint32_t>(size);
        stream << zero;        /* 00000000: Zero/unknown */
        stream << zero;        /* 00000004: Zero/unknown */
        stream << base;        /* 00000008: Offset at which data starts */
        stream << s;           /* 0000000C: Full size of the data (32-bit) */
        stream << sector_size; /* 00000010: Sector size in bytes */
        stream << sectors_;    /* 00000014: Sectors per cylinder */
        stream << heads_;      /* 00000018: Heads per cylinder */
        stream << cylinders_;  /* 0000001C: Cylinders */

        for (int i = 0; i < 0x3f8; i++) {
            stream << zero;
        }
    } else if (img_format == IMG_FMT_HDX) { /* HDX file */
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        quint64 signature = 0xD778A82044445459;
        stream << signature;                      /* 00000000: Signature */
        stream << size;                           /* 00000008: Full size of the data (64-bit) */
        stream << sector_size;                    /* 00000010: Sector size in bytes */
        stream << sectors_;                       /* 00000014: Sectors per cylinder */
        stream << heads_;                         /* 00000018: Heads per cylinder */
        stream << cylinders_;                     /* 0000001C: Cylinders */
        stream << zero;                           /* 00000020: [Translation] Sectors per cylinder */
        stream << zero;                           /* 00000004: [Translation] Heads per cylinder */
    } else if (img_format >= IMG_FMT_VHD_FIXED) { /* VHD file */
        file.close();

        MVHDGeom _86box_geometry {};
        int      block_size = ui->comboBoxBlockSize->currentIndex() == 0 ? MVHD_BLOCK_LARGE : MVHD_BLOCK_SMALL;
        switch (img_format) {
            case IMG_FMT_VHD_FIXED:
                {
                    connect(this, &HarddiskDialog::fileProgress, this, [this](int value) { ui->progressBar->setValue(value); QApplication::processEvents(); });
                    ui->progressBar->setVisible(true);
                    [&_86box_geometry, fileName, this] {
                        _86box_geometry = create_drive_vhd_fixed(fileName, this, cylinders_, heads_, sectors_);
                    }();
                }
                break;
            case IMG_FMT_VHD_DYNAMIC:
                _86box_geometry = create_drive_vhd_dynamic(fileName, cylinders_, heads_, sectors_, block_size);
                break;
            case IMG_FMT_VHD_DIFF:
                QString vhdParent = QFileDialog::getOpenFileName(
                    this,
                    tr("Select the parent VHD"),
                    QString(),
                    tr("VHD files") % util::DlgFilter({ "vhd" }) % tr("All files") % util::DlgFilter({ "*" }, true));

                if (vhdParent.isEmpty()) {
                    return;
                }
                _86box_geometry = create_drive_vhd_diff(fileName, vhdParent, block_size);
                break;
        }

        if (_86box_geometry.cyl == 0 && _86box_geometry.heads == 0 && _86box_geometry.spt == 0) {
            QMessageBox::critical(this, tr("Unable to write file"), tr("Make sure the file is being saved to a writable directory."));
            return;
        } else if (img_format != IMG_FMT_VHD_DIFF) {
            QMessageBox::information(this, tr("Disk image created"), tr("Remember to partition and format the newly-created drive."));
        }

        ui->lineEditCylinders->setText(QString::number(_86box_geometry.cyl));
        ui->lineEditHeads->setText(QString::number(_86box_geometry.heads));
        ui->lineEditSectors->setText(QString::number(_86box_geometry.spt));
        cylinders_ = _86box_geometry.cyl;
        heads_     = _86box_geometry.heads;
        sectors_   = _86box_geometry.spt;
        setResult(QDialog::Accepted);

        return;
    }

    // formats 0, 1 and 2
#ifndef __unix__
    connect(this, &HarddiskDialog::fileProgress, this, [this](int value) { ui->progressBar->setValue(value); QApplication::processEvents(); });
    ui->progressBar->setVisible(true);
    [size, &file, this] {
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);

        QByteArray buf(1048576, 0);
        uint64_t   mibBlocks = size >> 20;
        uint64_t   restBlock = size & 0xfffff;

        if (restBlock) {
            stream.writeRawData(buf.data(), restBlock);
        }

        if (mibBlocks) {
            for (uint64_t i = 0; i < mibBlocks; ++i) {
                stream.writeRawData(buf.data(), buf.size());
                emit fileProgress(static_cast<int>((i * 100) / mibBlocks));
            }
        }
        emit fileProgress(100);
    }();
#else
    int ret = ftruncate(file.handle(), (size_t) size);

    if (ret) {
        QMessageBox::critical(this, tr("Unable to write file"), tr("Make sure the file is being saved to a writable directory."));
    }
#endif

    QMessageBox::information(this, tr("Disk image created"), tr("Remember to partition and format the newly-created drive."));
    setResult(QDialog::Accepted);
}

static void
adjust_vhd_geometry_for_86box(MVHDGeom *vhd_geometry)
{
    if (vhd_geometry->spt <= 63)
        return;

    int desired_sectors = vhd_geometry->cyl * vhd_geometry->heads * vhd_geometry->spt;
    if (desired_sectors > 267321600)
        desired_sectors = 267321600;

    int remainder = desired_sectors % 85680; /* 8560 is the LCM of 1008 (63*16) and 4080 (255*16) */
    if (remainder > 0)
        desired_sectors -= remainder;

    vhd_geometry->cyl   = desired_sectors / (16 * 63);
    vhd_geometry->heads = 16;
    vhd_geometry->spt   = 63;
}

void
HarddiskDialog::recalcSelection()
{
    int selection = 127;
    for (int i = 0; i < 127; i++) {
        if ((cylinders_ == hdd_table[i][0]) && (heads_ == hdd_table[i][1]) && (sectors_ == hdd_table[i][2]))
            selection = i;
    }
    if ((selection == 127) && (heads_ == 16) && (sectors_ == 63)) {
        selection = 128;
    }
}

void
HarddiskDialog::onExistingFileSelected(const QString &fileName, bool precheck)
{
    // TODO : Over to non-existing file selected
#if 0
    if (!(existing & 1)) {
        fp = _wfopen(wopenfilestring, L"rb");
        if (fp != NULL) {
            fclose(fp);
            if (settings_msgbox_ex(MBX_QUESTION_YN, L"Disk image file already exists", L"The selected file will be overwritten. Are you sure you want to use it?", L"Overwrite", L"Don't overwrite", NULL) != 0)	/ * yes * /
                return false;
        }
    }

    fp = _wfopen(wopenfilestring, (existing & 1) ? L"rb" : L"wb");
    if (fp == NULL) {
    hdd_add_file_open_error:
        fclose(fp);
        settings_msgbox_header(MBX_ERROR, (existing & 1) ? L"Make sure the file exists and is readable." : L"Make sure the file is being saved to a writable directory.", (existing & 1) ? L"Unable to read file" : L"Unable to write file");
        return true;
    }
#endif

    uint64_t size        = 0;
    uint32_t sector_size = 0;
    uint32_t sectors     = 0;
    uint32_t heads       = 0;
    uint32_t cylinders   = 0;
    int      vhd_error   = 0;

    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        // No message box during precheck (performed when the file input loses focus and this function is called)
        // If precheck is false, the file has been chosen from a file dialog and the alert should display.
        if(!precheck) {
            QMessageBox::critical(this, tr("Unable to read file"), tr("Make sure the file exists and is readable."));
        }
        return;
    }
    QByteArray fileNameUtf8 = fileName.toUtf8();

    QFileInfo fi(file);
    if (image_is_hdi(fileNameUtf8.data()) || image_is_hdx(fileNameUtf8.data(), 1)) {
        file.seek(0x10);
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream >> sector_size;
        if (sector_size != 512) {
            QMessageBox::critical(this, tr("Unsupported disk image"), tr("HDI or HDX images with a sector size other than 512 are not supported."));
            return;
        }

        sectors = heads = cylinders = 0;
        stream >> sectors;
        stream >> heads;
        stream >> cylinders;
    } else if (image_is_vhd(fileNameUtf8.data(), 1)) {
        MVHDMeta *vhd = mvhd_open(fileNameUtf8.data(), 0, &vhd_error);
        if (vhd == nullptr) {
            QMessageBox::critical(this, tr("Unable to read file"), tr("Make sure the file exists and is readable."));
            return;
        } else if (vhd_error == MVHD_ERR_TIMESTAMP) {
            QMessageBox::StandardButton btn = QMessageBox::warning(this, tr("Parent and child disk timestamps do not match"), tr("This could mean that the parent image was modified after the differencing image was created.\n\nIt can also happen if the image files were moved or copied, or by a bug in the program that created this disk.\n\nDo you want to fix the timestamps?"), QMessageBox::Yes | QMessageBox::No);
            if (btn == QMessageBox::Yes) {
                int ts_res = mvhd_diff_update_par_timestamp(vhd, &vhd_error);
                if (ts_res != 0) {
                    QMessageBox::critical(this, tr("Error"), tr("Could not fix VHD timestamp"));
                    mvhd_close(vhd);
                    return;
                }
            } else {
                mvhd_close(vhd);
                return;
            }
        }

        MVHDGeom vhd_geom = mvhd_get_geometry(vhd);
        adjust_vhd_geometry_for_86box(&vhd_geom);
        cylinders = vhd_geom.cyl;
        heads     = vhd_geom.heads;
        sectors   = vhd_geom.spt;
        size      = static_cast<uint64_t>(cylinders * heads * sectors * 512);
        mvhd_close(vhd);
    } else {
        size = file.size();
        if (((size % 17) == 0) && (size <= 142606336)) {
            sectors = 17;
            if (size <= 26738688)
                heads = 4;
            else if (((size % 3072) == 0) && (size <= 53477376))
                heads = 6;
            else {
                uint32_t i;
                for (i = 5; i < 16; i++) {
                    if (((size % (i << 9)) == 0) && (size <= ((i * 17) << 19)))
                        break;
                    if (i == 5)
                        i++;
                }
                heads = i;
            }
        } else {
            sectors = 63;
            heads   = 16;
        }

        cylinders = ((size >> 9) / heads) / sectors;
    }

    if ((sectors > max_sectors) || (heads > max_heads) || (cylinders > max_cylinders)) {
        QMessageBox::critical(this, tr("Unable to read file"), tr("Make sure the file exists and is readable."));
        return;
    }

    heads_     = heads;
    sectors_   = sectors;
    cylinders_ = cylinders;
    ui->lineEditCylinders->setText(QString::number(cylinders));
    ui->lineEditHeads->setText(QString::number(heads));
    ui->lineEditSectors->setText(QString::number(sectors));
    recalcSize();

    ui->lineEditCylinders->setEnabled(true);
    ui->lineEditHeads->setEnabled(true);
    ui->lineEditSectors->setEnabled(true);
    ui->lineEditSize->setEnabled(true);
    ui->sizeSlider->setEnabled(true);
    ui->comboBoxManPreset->setEnabled(true);
    ui->comboBoxModelPreset->setEnabled(true);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void
HarddiskDialog::recalcSize()
{
    if (disallowSizeModifications)
        return;
    uint64_t size = (static_cast<uint64_t>(cylinders_) * static_cast<uint64_t>(heads_) * static_cast<uint64_t>(sectors_)) << 9;
    const auto new_size = (size >> 20);
    ui->lineEditSize->setText(QString::number(new_size));
    // If needed, change the max value on the slider
    if(new_size > ui->sizeSlider->maximum()) {
        ui->sizeSlider->setMaximum(new_size);
        ui->sizeSlider->setValue(ui->sizeSlider->maximum());
        return;
    } else {
        // Similarly, bring it back down if needed
        if (ui->sizeSlider->maximum() > DEFAULT_MAX_SIZE_SLIDER && new_size < DEFAULT_MAX_SIZE_SLIDER) {
            ui->sizeSlider->setMaximum(DEFAULT_MAX_SIZE_SLIDER);
        }
        ui->sizeSlider->setSliderPosition(new_size);
    }
}

bool
HarddiskDialog::checkAndAdjustSectors()
{
    if (sectors_ > max_sectors) {
        sectors_ = max_sectors;
        ui->lineEditSectors->setText(QString::number(max_sectors));
        recalcSize();
        return false;
    }
    return true;
}

bool
HarddiskDialog::checkAndAdjustHeads()
{
    if (heads_ > max_heads) {
        heads_ = max_heads;
        ui->lineEditHeads->setText(QString::number(max_heads));
        recalcSize();
        return false;
    }
    return true;
}

bool
HarddiskDialog::checkAndAdjustCylinders()
{
    if (cylinders_ > max_cylinders) {
        cylinders_ = max_cylinders;
        ui->lineEditCylinders->setText(QString::number(max_cylinders));
        recalcSize();
        return false;
    }
    return true;
}

void
HarddiskDialog::on_comboBoxBus_currentIndexChanged(int index)
{
    int chanIdx = 0;
    if (index < 0) {
        return;
    }

    switch (ui->comboBoxBus->currentData().toInt()) {
        case HDD_BUS_DISABLED:
        default:
            max_sectors = max_heads = max_cylinders = 0;
            break;
        case HDD_BUS_MFM:
            max_sectors   = 26; /* 17 for MFM, 26 for RLL. */
            max_heads     = 15;
            max_cylinders = 2047;
            break;
        case HDD_BUS_XTA:
            max_sectors   = 63;
            max_heads     = 16;
            max_cylinders = 1023;
            break;
        case HDD_BUS_ESDI:
            max_sectors   = 99; /* ESDI drives usually had 32 to 43 sectors per track. */
            max_heads     = 16;
            max_cylinders = 266305;
            break;
        case HDD_BUS_IDE:
            max_sectors   = 255;
            max_heads     = 255;
            max_cylinders = 266305;
            break;
        case HDD_BUS_ATAPI:
        case HDD_BUS_SCSI:
            max_sectors   = 255;
            max_heads     = 255;
            max_cylinders = 266305;
            break;
    }

    checkAndAdjustCylinders();
    checkAndAdjustHeads();
    checkAndAdjustSectors();

    if (ui->lineEditCylinders->validator() != nullptr) {
        delete ui->lineEditCylinders->validator();
    }
    if (ui->lineEditHeads->validator() != nullptr) {
        delete ui->lineEditHeads->validator();
    }
    if (ui->lineEditSectors->validator() != nullptr) {
        delete ui->lineEditSectors->validator();
    }

    ui->lineEditCylinders->setValidator(new QIntValidator(1, max_cylinders, this));
    ui->lineEditHeads->setValidator(new QIntValidator(1, max_heads, this));
    ui->lineEditSectors->setValidator(new QIntValidator(1, max_sectors, this));

    Harddrives::populateBusChannels(ui->comboBoxChannel->model(), ui->comboBoxBus->currentData().toInt(), Harddrives::busTrackClass);
    Harddrives::populateSpeeds(ui->comboBoxSpeed->model(), ui->comboBoxBus->currentData().toInt());

    switch (ui->comboBoxBus->currentData().toInt()) {
        case HDD_BUS_MFM:
            chanIdx = (Harddrives::busTrackClass->next_free_mfm_channel());
            break;
        case HDD_BUS_XTA:
            chanIdx = (Harddrives::busTrackClass->next_free_xta_channel());
            break;
        case HDD_BUS_ESDI:
            chanIdx = (Harddrives::busTrackClass->next_free_esdi_channel());
            break;
        case HDD_BUS_ATAPI:
        case HDD_BUS_IDE:
            chanIdx = (Harddrives::busTrackClass->next_free_ide_channel());
            break;
        case HDD_BUS_SCSI:
            chanIdx = (Harddrives::busTrackClass->next_free_scsi_id());
            break;
    }

    if (chanIdx == 0xFF)
        chanIdx = 0;
    ui->comboBoxChannel->setCurrentIndex(chanIdx);
}

void
HarddiskDialog::on_lineEditSize_textEdited(const QString &text)
{
    disallowSizeModifications = true;
    const int size            = text.toInt();
    /* This is needed to ensure VHD standard compliance. */
    hdd_image_calc_chs(&cylinders_, &heads_, &sectors_, size);
    ui->lineEditCylinders->setText(QString::number(cylinders_));
    ui->lineEditHeads->setText(QString::number(heads_));
    ui->lineEditSectors->setText(QString::number(sectors_));
    // Again here we adjust the size slider up and down as necessary
    if(size > ui->sizeSlider->maximum()) {
        ui->sizeSlider->setMaximum(size);
        ui->sizeSlider->setValue(ui->sizeSlider->maximum());
    } else {
        if (ui->sizeSlider->maximum() > DEFAULT_MAX_SIZE_SLIDER && size < DEFAULT_MAX_SIZE_SLIDER) {
            ui->sizeSlider->setMaximum(DEFAULT_MAX_SIZE_SLIDER);
        }
        ui->sizeSlider->setValue(size);
    }

    checkAndAdjustCylinders();
    checkAndAdjustHeads();
    checkAndAdjustSectors();

    disallowSizeModifications = false;
}

void
HarddiskDialog::on_lineEditCylinders_textEdited(const QString &text)
{
    cylinders_ = text.toUInt();
    if (checkAndAdjustCylinders()) {
        recalcSize();
    }
}

void
HarddiskDialog::on_lineEditHeads_textEdited(const QString &text)
{
    heads_ = text.toUInt();
    if (checkAndAdjustHeads()) {
        recalcSize();
    }
}

void
HarddiskDialog::on_lineEditSectors_textEdited(const QString &text)
{
    sectors_ = text.toUInt();
    if (checkAndAdjustSectors()) {
        recalcSize();
    }
}

void
HarddiskDialog::accept()
{
    if (ui->fileField->createFile())
        onCreateNewFile();
    else
        setResult(QDialog::Accepted);
    QDialog::done(result());
}

void
HarddiskDialog::parsePresetList()
{
    if(!presets.isEmpty()) {
        // Already parsed
        return;
    }
    QFile json_file(":/json/hdd_presets.json");
    if (!json_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("Couldn't open the hdd preset file: error %d", json_file.error());
        return;
    }
    const QString read_file = json_file.readAll();
    json_file.close();

    const auto json_doc = QJsonDocument::fromJson(read_file.toUtf8());

    if (json_doc.isNull()) {
        qWarning("Failed to create QJsonDocument, possibly invalid JSON");
        return;
    }
    if (!json_doc.isArray()) {
        qWarning("JSON does not have the expected format (array in root), cannot continue");
        return;
    }

    QJsonArray json_array = json_doc.array();

    for (const auto &val : json_array) {
        if (auto preset = presetFromJson(val.toObject()); preset.has_value()) {
            presets.append(preset.value());
        }
    }
}

std::optional<HarddiskDialog::HDDPreset>
HarddiskDialog::presetFromJson(const QJsonObject &json)
{
    // Some basic validation
    if (
        !json.contains("id") ||
        !json.contains("manufacturer") ||
        !json.contains("model") ||
        !json.contains("size") ||
        !json.contains("c") ||
        !json.contains("h") ||
        !json.contains("s") ||
        !json.contains("bus")) {
        return std::nullopt;
    }

    // Some models are listed in the json as numbers instead of strings. Convert if necessary.
    const auto model = json["model"].isDouble() ? QString::number(json["model"].toDouble()) : json["model"];

    return HDDPreset {
        .id           = json["id"].toInt(),
        .manufacturer = json["manufacturer"].toString(),
        .model        = model.toString(),
        .size         = json["size"].toInt(),
        .c            = json["c"].toInt(),
        .h            = json["h"].toInt(),
        .s            = json["s"].toInt(),
        .bus          = json["bus"].toString(),
    };
}

QStringListModel *
HarddiskDialog::createManufacturersModel()
{
    parsePresetList();
    const auto presetManufacturersModel = new QStringListModel();
    QSet<QString> manufacturerSet;
    for (const auto &preset : presets) {
        manufacturerSet.insert(preset.manufacturer);
    }
    // Add "Generic" for the built-in presets
    manufacturerSet.insert(tr("Generic"));
    // Sort the manufacturers
    QList<QString> manList = manufacturerSet.values();
    std::sort(manList.begin(), manList.end());
    manList.prepend(tr("Select a manufacturer"));
    // Use the sorted list as the model source
    presetManufacturersModel->setStringList(manList);

    return presetManufacturersModel;
}

QStandardItemModel *
HarddiskDialog::createHddPresetModel()
{
    parsePresetList();
    const auto presetModel = new QStandardItemModel();
    // Populate the presets model
    for (const auto &preset : presets) {
        auto     *item = new QStandardItem();
        const int row  = presetModel->rowCount();
        presetModel->appendRow(item);
        auto pmIndex = presetModel->index(row, 0);

        const uint64_t size    = static_cast<uint64_t>(preset.c) * preset.h * preset.s;
        const uint32_t size_mb = size >> 11LL;
        auto text = QString(tr("%1 (%2 MB)")).arg(preset.model, QString::number(size_mb));

        presetModel->setData(pmIndex, text, Qt::DisplayRole);
        presetModel->setData(pmIndex, preset.manufacturer, Manufacturer);
        presetModel->setData(pmIndex, preset.model, Model);
        presetModel->setData(pmIndex, preset.size, Size);
        presetModel->setData(pmIndex, preset.c, Cylinders);
        presetModel->setData(pmIndex, preset.h, Heads);
        presetModel->setData(pmIndex, preset.s, Sectors);
        presetModel->setData(pmIndex, preset.bus, Bus);
    }

    // Now add the stock hdd_table options to the model as "Generic"
    for (int i = 0; i < 127; i++) {
        auto     *item = new QStandardItem();
        const int row  = presetModel->rowCount();
        presetModel->appendRow(item);
        auto pmIndex = presetModel->index(row, 0);

        const uint64_t size    = static_cast<uint64_t>(hdd_table[i][0]) * hdd_table[i][1] * hdd_table[i][2];
        const uint32_t size_mb = size >> 11LL;
        QString        display = QString::asprintf(tr("%u MB (%i/%i/%i)").toUtf8().constData(), size_mb, hdd_table[i][0], hdd_table[i][1], hdd_table[i][2]);

        presetModel->setData(pmIndex, display, Qt::DisplayRole);
        presetModel->setData(pmIndex, tr("Generic"), Manufacturer);
        presetModel->setData(pmIndex, display, Model);
        presetModel->setData(pmIndex, size_mb, Size);
        presetModel->setData(pmIndex, hdd_table[i][0], Cylinders);
        presetModel->setData(pmIndex, hdd_table[i][1], Heads);
        presetModel->setData(pmIndex, hdd_table[i][2], Sectors);
    }
    return presetModel;
}

QSortFilterProxyModel *
HarddiskDialog::createHddPresetProxyModel(QStandardItemModel* sourceModel)
{
    const auto hddPresetModel = new QSortFilterProxyModel();
    hddPresetModel->setSourceModel(sourceModel);
    hddPresetModel->setSortLocaleAware(true);
    hddPresetModel->setSortRole(Qt::DisplayRole);
    hddPresetModel->setFilterFixedString("-invalid-");
    hddPresetModel->sort(0, Qt::AscendingOrder);
    return hddPresetModel;
}

void
HarddiskDialog::populatePresetModels(const int index) const
{
    const auto currentIndex = ui->comboBoxManPreset->model()->index(index, 0);
    const auto filterNameVal = currentIndex.data();
    if(filterNameVal.type() != QVariant::String) {
        return;
    }
    const auto filterName = filterNameVal.toString();
    hddPresetModel->setFilterRole(Manufacturer);
    hddPresetModel->setFilterKeyColumn(0);
    hddPresetModel->setFilterFixedString(filterName);
}

void
HarddiskDialog::setPresetParameters(const int index)
{
    if (index < 0) {
        return;
    }

    const auto currentIndex = ui->comboBoxModelPreset->model()->index(index, 0);
    const auto cylinders    = currentIndex.data(Cylinders).toInt();
    const auto heads        = currentIndex.data(Heads).toInt();
    const auto sectors      = currentIndex.data(Sectors).toInt();

    cylinders_ = cylinders;
    heads_     = heads;
    sectors_   = sectors;

    ui->lineEditCylinders->setText(QString::number(cylinders_));
    ui->lineEditHeads->setText(QString::number(heads_));
    ui->lineEditSectors->setText(QString::number(sectors_));
    recalcSize();
}

// The QComboBox::currentTextChanged signal is used and connected here instead of
// QComboBox::currentIndexChanged. When a manufacturer is selected and the
// models are populated the model index could still remain the same (such as zero, the first item).
void
HarddiskDialog::
presetTextChanged(const QString &text)
{
    setPresetParameters(ui->comboBoxModelPreset->currentIndex());
}

void
HarddiskDialog::sizeSliderChanged(int value)
{
    // If the values differ, we have arrived here because of the slider being
    // set interactively
    if (value != ui->lineEditSize->text().toUInt()) {
        // Enforce even values for the slider
        if ((value & 1) == 1) {
            value++;
        }
        ui->lineEditSize->setText(QString::number(value));
        on_lineEditSize_textEdited(QString::number(value));
    }
}

// Reset the preset selection whenever the slider is moved interactively
void
HarddiskDialog::sizeSliderMoved(int value) const
{
    ui->comboBoxManPreset->setCurrentIndex(0);
}

// Ensure a consistent amount of slider ticks no matter the range
void
HarddiskDialog::sizeSliderRangeChanged(const int min, const int max) const
{
    ui->sizeSlider->setTickInterval(max / 16);
}