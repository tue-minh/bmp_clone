#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "linux_backend.hpp"
#include "logo_data.hpp"

struct SpeedOpt { const char* label; uint32_t hz; };
static const SpeedOpt kSwdSpeeds[] = {
    {"4 MHz",   4000000},
    {"2 MHz",   2000000},
    {"1.8 MHz", 1800000},
    {"1 MHz",   1000000},
    {"500 kHz", 500000},
    {"200 kHz", 200000},
    {"100 kHz", 100000},
    {"50 kHz",  50000},
    {"20 kHz",  20000},
};
static constexpr int kDefaultSpeedIdx = 3;

struct OcdOpt { const char* label; const char* cfg; };
static const OcdOpt kOcdTargets[] = {
    {"STM32F4 (stm32f4x.cfg)", "target/stm32f4x.cfg"},
    {"STM32F1 (stm32f1x.cfg)", "target/stm32f1x.cfg"},
    {"STM32F3 (stm32f3x.cfg)", "target/stm32f3x.cfg"},
    {"STM32G4 (stm32g4x.cfg)", "target/stm32g4x.cfg"},
    {"STM32L4 (stm32l4x.cfg)", "target/stm32l4x.cfg"},
    {"STM32F0 (stm32f0x.cfg)", "target/stm32f0x.cfg"},
    {"STM32G0 (stm32g0x.cfg)", "target/stm32g0x.cfg"},
    {"STM32H7 (stm32h7x.cfg)", "target/stm32h7x.cfg"},
};

static uint32_t parseHexOrDec(const QString& s, uint32_t defVal) {
    bool ok = false;
    uint32_t v = s.trimmed().toUInt(&ok, 0);
    return ok ? v : defVal;
}

static QString formatHexDump(uint32_t baseAddr, const std::vector<uint8_t>& data) {
    QString out;
    char line[128];
    for (size_t off = 0; off < data.size(); off += 16) {
        size_t n = std::min(size_t(16), data.size() - off);
        std::string hexPart, ascPart;
        for (size_t i = 0; i < 16; ++i) {
            if (i < n) {
                uint8_t b = data[off + i];
                char hb[4];
                std::snprintf(hb, sizeof(hb), "%02X ", b);
                hexPart += hb;
                ascPart += (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
            } else {
                hexPart += "   ";
            }
            if (i == 7) hexPart += ' ';
        }
        std::snprintf(line, sizeof(line), "0x%08X: %s |%s|\n",
                      baseAddr + static_cast<uint32_t>(off), hexPart.c_str(), ascPart.c_str());
        out += QString::fromLatin1(line);
    }
    return out;
}

static std::string decodeEscapes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char next = in[i + 1];
            if (next == 'r')  { out.push_back('\r'); ++i; continue; }
            if (next == 'n')  { out.push_back('\n'); ++i; continue; }
            if (next == 't')  { out.push_back('\t'); ++i; continue; }
            if (next == '\\') { out.push_back('\\'); ++i; continue; }
            if (next == '0')  { out.push_back('\0'); ++i; continue; }
            if (next == 'x' && i + 3 < in.size() &&
                std::isxdigit(static_cast<unsigned char>(in[i + 2])) &&
                std::isxdigit(static_cast<unsigned char>(in[i + 3]))) {
                auto hv = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return c - 'A' + 10;
                };
                out.push_back(static_cast<char>((hv(in[i + 2]) << 4) | hv(in[i + 3])));
                i += 3;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QPixmap logoPix;
    logoPix.loadFromData(kLogoPng, static_cast<uint>(kLogoPngLen), "PNG");
    app.setWindowIcon(QIcon(logoPix));

    QMainWindow win;
    QMainWindow* winPtr = &win;
    win.setWindowTitle("TUE CMSIS-DAP Programmer & Debugger");
    win.setFixedSize(900, 620);

    QFont monoFont("Monospace", 9);
    monoFont.setStyleHint(QFont::TypeWriter);

    // State
    std::vector<LinuxHidInfo> hidDevices;
    std::mutex hidMutex;
    bool probePresent = false;
    bool targetConnected = false;
    std::atomic<bool> busy{false};
    QString ocdPath = "openocd";
    QProcess gdbProc;
    LinuxSerialPort serialPort;

    // Central Layout
    auto* central = new QWidget(&win);
    win.setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    // Row 1: USB status + Use OpenOCD + Target + Speed + Under Reset
    auto* row1 = new QHBoxLayout();
    auto* lblUsb = new QLabel("USB: Scanning...");
    lblUsb->setStyleSheet("font-weight: bold;");
    lblUsb->setMinimumWidth(330);
    auto* chkUseOcd = new QCheckBox("Use OpenOCD");
    auto* cmbOcdTarget = new QComboBox();
    for (const auto& t : kOcdTargets) cmbOcdTarget->addItem(t.label, t.cfg);
    auto* cmbSpeed = new QComboBox();
    for (const auto& s : kSwdSpeeds) cmbSpeed->addItem(s.label, s.hz);
    cmbSpeed->setCurrentIndex(kDefaultSpeedIdx);
    auto* chkUnderReset = new QCheckBox("Under Reset");

    row1->addWidget(lblUsb, 1);
    row1->addWidget(chkUseOcd);
    row1->addWidget(cmbOcdTarget);
    row1->addWidget(cmbSpeed);
    row1->addWidget(chkUnderReset);
    root->addLayout(row1);

    // Row 2: Function Buttons
    auto* row2 = new QHBoxLayout();
    auto* btnConnect = new QPushButton("Connect");
    auto* btnFlash   = new QPushButton("Flash");
    auto* btnVerify  = new QPushButton("Verify");
    auto* btnReset   = new QPushButton("Reset");
    auto* btnErase   = new QPushButton("Mass Erase");
    auto* btnHalt    = new QPushButton("Halt");
    auto* btnRun     = new QPushButton("Run");
    auto* btnStep    = new QPushButton("Step");
    auto* btnRegs    = new QPushButton("Read Regs");

    for (auto* b : {btnConnect, btnFlash, btnVerify, btnReset, btnErase, btnHalt, btnRun, btnStep, btnRegs}) {
        b->setMinimumHeight(28);
        row2->addWidget(b);
    }
    root->addLayout(row2);

    // Row 3: Firmware File + Browse + Addr
    auto* row3 = new QHBoxLayout();
    row3->addWidget(new QLabel("Firmware:"));
    auto* edtFile = new QLineEdit();
    auto* btnBrowse = new QPushButton("Browse...");
    row3->addWidget(edtFile, 1);
    row3->addWidget(btnBrowse);
    row3->addWidget(new QLabel("Addr:"));
    auto* edtBaseAddr = new QLineEdit("0x08000000");
    edtBaseAddr->setFixedWidth(100);
    row3->addWidget(edtBaseAddr);
    root->addLayout(row3);

    // Center Split: Left = Target Status & On-Chip Flash, Right = Debug & CPU Regs
    auto* centerRow = new QHBoxLayout();
    auto* leftCol = new QVBoxLayout();
    leftCol->addWidget(new QLabel("Target Status & On-Chip Flash:"));
    auto* edtTarget = new QPlainTextEdit(
        "Not connected.\nPlug in the CMSIS-DAP probe and click [Connect] to inspect chip & flash content.");
    edtTarget->setReadOnly(true);
    edtTarget->setFont(monoFont);
    leftCol->addWidget(edtTarget);

    auto* rightCol = new QVBoxLayout();
    rightCol->addWidget(new QLabel("Debug & CPU Registers:"));
    auto* edtDebug = new QPlainTextEdit(
        "CPU: Not connected.\nAfter Connect, use Halt / Run / Step / Read Regs.");
    edtDebug->setReadOnly(true);
    edtDebug->setFont(monoFont);
    rightCol->addWidget(edtDebug);

    centerRow->addLayout(leftCol, 58);
    centerRow->addLayout(rightCol, 42);
    root->addLayout(centerRow, 1);

    // Bottom: Progress + Clear Log + Log
    auto* progRow = new QHBoxLayout();
    auto* progress = new QProgressBar();
    progress->setRange(0, 100);
    auto* btnClearLog = new QPushButton("Clear Log");
    progRow->addWidget(progress, 1);
    progRow->addWidget(btnClearLog);
    root->addLayout(progRow);

    auto* edtLog = new QPlainTextEdit();
    edtLog->setReadOnly(true);
    edtLog->setFont(monoFont);
    edtLog->setFixedHeight(135);
    root->addWidget(edtLog);

    auto postLog = [edtLog](const QString& msg) {
        QString line = QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), msg);
        QMetaObject::invokeMethod(edtLog, [edtLog, line]() {
            edtLog->appendPlainText(line);
        }, Qt::QueuedConnection);
    };

    auto updateButtons = [&]() {
        bool canOp = targetConnected && !busy.load();
        bool notBusy = !busy.load();
        bool ocd = chkUseOcd->isChecked();
        btnConnect->setEnabled(notBusy && (probePresent || ocd));
        btnFlash->setEnabled(canOp);
        btnVerify->setEnabled(canOp);
        btnReset->setEnabled(canOp);
        btnErase->setEnabled(canOp);
        btnHalt->setEnabled(canOp);
        btnRun->setEnabled(canOp);
        btnStep->setEnabled(canOp && !ocd);
        btnRegs->setEnabled(canOp && !ocd);
        cmbOcdTarget->setEnabled(ocd && notBusy);
    };

    auto getSelectedHid = [&]() -> LinuxHidInfo {
        std::lock_guard<std::mutex> lk(hidMutex);
        if (hidDevices.empty()) throw std::runtime_error("No CMSIS-DAP probe detected");
        return hidDevices[0];
    };

    auto runBg = [&](std::function<void()> fn) {
        if (busy.exchange(true)) { postLog("[WARN] Busy..."); return; }
        updateButtons();
        progress->setValue(0);
        std::thread([&, fn]() {
            try { fn(); }
            catch (const std::exception& ex) { postLog(QString("[ERROR] ") + ex.what()); }
            QMetaObject::invokeMethod(&win, [&]() {
                busy.store(false);
                updateButtons();
            }, Qt::QueuedConnection);
        }).detach();
    };

    auto formatTargetText = [](const Stm32TargetInfo& info) -> QString {
        QString rdpStr = "N/A";
        if (info.rdp == 0xAA) rdpStr = "Level 0 (Unlocked)";
        else if (info.rdp >= 0) rdpStr = QString("Protected (0x%1)").arg(info.rdp, 2, 16, QChar('0')).toUpper();

        QString out = QString(
            "Chip:      %1\n"
            "DEV_ID:    0x%2 (Rev 0x%3) | Family: %4\n"
            "DP_IDCODE: 0x%5 | DBGMCU: 0x%6\n"
            "Flash:     %7 KB (Page/Sector: %8 B) | RDP: %9\n"
            "UID:       %10\n")
            .arg(QString::fromStdString(info.name))
            .arg(info.devId, 3, 16, QChar('0')).toUpper()
            .arg(info.revId, 4, 16, QChar('0')).toUpper()
            .arg(QString::fromStdString(info.family))
            .arg(info.dpId, 8, 16, QChar('0')).toUpper()
            .arg(info.dbgId, 8, 16, QChar('0')).toUpper()
            .arg(info.flashKB).arg(info.pageSize).arg(rdpStr)
            .arg(QString::fromStdString(info.uid));

        if (info.flashPreview.size() >= 8) {
            uint32_t sp = info.flashPreview[0] | (info.flashPreview[1] << 8) |
                          (info.flashPreview[2] << 16) | (info.flashPreview[3] << 24);
            uint32_t rv = info.flashPreview[4] | (info.flashPreview[5] << 8) |
                          (info.flashPreview[6] << 16) | (info.flashPreview[7] << 24);
            bool blank = std::all_of(info.flashPreview.begin(), info.flashPreview.end(),
                                     [](uint8_t b) { return b == 0xFF; });
            out += QString("--- On-Chip Flash Content @ 0x08000000 ---\n"
                           "Status: %1 | Init SP: 0x%2 | Reset_Handler: 0x%3\n\n")
                .arg(blank ? "BLANK (0xFF)" : "PROGRAMMED")
                .arg(sp, 8, 16, QChar('0')).toUpper()
                .arg(rv, 8, 16, QChar('0')).toUpper();
            out += formatHexDump(0x08000000, info.flashPreview);
        }
        return out;
    };

    auto formatDebugText = [](const CpuDebugStatus& st) -> QString {
        return QString(
            "CPU State: %1\n"
            "PC   = 0x%2   SP   = 0x%3\n"
            "LR   = 0x%4   xPSR = 0x%5\n"
            "R0   = 0x%6   R1   = 0x%7\n"
            "R2   = 0x%8   R3   = 0x%9\n"
            "R12  = 0x%10   MSP  = 0x%11\n"
            "PSP  = 0x%12\n"
            "CFSR = 0x%13   HFSR = 0x%14\n")
            .arg(st.halted ? "HALTED (Debug Mode)" : "RUNNING")
            .arg(st.pc, 8, 16, QChar('0')).toUpper()
            .arg(st.sp, 8, 16, QChar('0')).toUpper()
            .arg(st.lr, 8, 16, QChar('0')).toUpper()
            .arg(st.xpsr, 8, 16, QChar('0')).toUpper()
            .arg(st.r0, 8, 16, QChar('0')).toUpper()
            .arg(st.r1, 8, 16, QChar('0')).toUpper()
            .arg(st.r2, 8, 16, QChar('0')).toUpper()
            .arg(st.r3, 8, 16, QChar('0')).toUpper()
            .arg(st.r12, 8, 16, QChar('0')).toUpper()
            .arg(st.msp, 8, 16, QChar('0')).toUpper()
            .arg(st.psp, 8, 16, QChar('0')).toUpper()
            .arg(st.cfsr, 8, 16, QChar('0')).toUpper()
            .arg(st.hfsr, 8, 16, QChar('0')).toUpper();
    };

    auto runOcdSync = [&](const QStringList& extraCmds) -> int {
        QStringList args;
        args << "-f" << "interface/cmsis-dap.cfg"
             << "-f" << cmbOcdTarget->currentData().toString()
             << "-c" << QString("adapter speed %1").arg(cmbSpeed->currentData().toUInt() / 1000);
        for (const auto& c : extraCmds) args << "-c" << c;
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(ocdPath, args);
        if (!p.waitForStarted(3000)) {
            postLog("[OCD] Failed to start openocd");
            return -1;
        }
        p.waitForFinished(-1);
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        for (const auto& line : out.split('\n', Qt::SkipEmptyParts)) {
            postLog("[OCD] " + line.trimmed());
        }
        return p.exitCode();
    };

    // USB Auto-Detect Timer (every 500ms)
    auto refreshUsb = [&](bool logChange) {
        auto devs = LinuxHidDevice::enumerateDapDevices();
        auto coms = LinuxSerialPort::enumeratePorts();
        bool was = probePresent;
        {
            std::lock_guard<std::mutex> lk(hidMutex);
            hidDevices = devs;
            probePresent = !devs.empty();
        }
        if (probePresent) {
            QString comStr = coms.empty() ? "" : (" | " + QString::fromStdString(coms[0]));
            QString badge = QString("USB: Connected - %1 (%2:%3)%4")
                .arg(QString::fromStdString(devs[0].product))
                .arg(devs[0].vid, 4, 16, QChar('0')).toUpper()
                .arg(devs[0].pid, 4, 16, QChar('0')).toUpper()
                .arg(comStr);
            lblUsb->setText(badge);
            if (logChange && !was) postLog("[USB] Device detected: " + QString::fromStdString(devs[0].product));
        } else {
            lblUsb->setText("USB: Waiting for CMSIS-DAP probe...");
            if (targetConnected) {
                targetConnected = false;
                edtTarget->setPlainText("Disconnected.\nPlug in the CMSIS-DAP probe and click [Connect].");
                edtDebug->setPlainText("CPU: Not connected.");
            }
            if (logChange && was) postLog("[USB] Programmer disconnected.");
        }
        updateButtons();
    };
    refreshUsb(false);
    auto* usbTimer = new QTimer(&win);
    QObject::connect(usbTimer, &QTimer::timeout, [&]() { refreshUsb(true); });
    usbTimer->start(500);

    QObject::connect(chkUseOcd, &QCheckBox::toggled, [&](bool) { updateButtons(); });
    QObject::connect(btnClearLog, &QPushButton::clicked, [&]() { edtLog->clear(); });
    QObject::connect(btnBrowse, &QPushButton::clicked, [&]() {
        QString p = QFileDialog::getOpenFileName(&win, "Select Firmware", "", "Firmware (*.bin *.hex *.elf);;All (*)");
        if (!p.isEmpty()) { edtFile->setText(p); postLog("[FILE] Selected: " + p); }
    });

    // Connect
    QObject::connect(btnConnect, &QPushButton::clicked, [&]() {
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool underReset = chkUnderReset->isChecked();
        bool ocd = chkUseOcd->isChecked();
        runBg([&, speed, underReset, ocd]() {
            if (ocd) {
                int rc = runOcdSync({"init", "targets", "exit"});
                QMetaObject::invokeMethod(winPtr, [&, rc]() {
                    targetConnected = (rc == 0);
                    edtTarget->setPlainText(rc == 0
                        ? "Mode: OpenOCD (interface/cmsis-dap.cfg)\nStatus: Connected OK."
                        : "OpenOCD connection failed.");
                    updateButtons();
                }, Qt::QueuedConnection);
                return;
            }
            LinuxHidInfo devInfo = getSelectedHid();
            LinuxHidDevice hid;
            std::string err;
            if (!hid.open(devInfo, err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            postLog("[SWD] Connecting and reading Chip ID + Flash @ 0x08000000...");
            auto info = dap.probeTarget(speed, underReset, true);
            postLog(QString("[SWD] Connected: %1 | Flash=%2 KB").arg(QString::fromStdString(info.name)).arg(info.flashKB));
            QString txt = formatTargetText(info);
            QMetaObject::invokeMethod(winPtr, [&, txt]() {
                targetConnected = true;
                edtTarget->setPlainText(txt);
                updateButtons();
            }, Qt::QueuedConnection);
        });
    });

    // Flash
    QObject::connect(btnFlash, &QPushButton::clicked, [&]() {
        QString fwPath = edtFile->text().trimmed();
        if (fwPath.isEmpty()) { QMessageBox::warning(&win, "No File", "Select a firmware file first."); return; }
        uint32_t baseAddr = parseHexOrDec(edtBaseAddr->text(), 0x08000000);
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool underReset = chkUnderReset->isChecked();
        bool ocd = chkUseOcd->isChecked();
        runBg([&, fwPath, baseAddr, speed, underReset, ocd]() {
            if (ocd) {
                QString cmd = fwPath.toLower().endsWith(".bin")
                    ? QString("program \"%1\" 0x08000000 verify reset exit").arg(fwPath)
                    : QString("program \"%1\" verify reset exit").arg(fwPath);
                int rc = runOcdSync({cmd});
                postLog(rc == 0 ? "[OK] OpenOCD Flash succeeded!" : "[ERROR] OpenOCD Flash failed!");
                return;
            }
            auto img = loadFirmwareImageLinux(fwPath.toStdString(), baseAddr);
            postLog(QString("[FLASH] Programming %1 bytes @ 0x%2").arg(img.data.size()).arg(img.baseAddr, 8, 16, QChar('0')));
            LinuxHidInfo devInfo = getSelectedHid();
            LinuxHidDevice hid;
            std::string err;
            if (!hid.open(devInfo, err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            auto info = dap.flashImage(img.baseAddr, img.data, speed, underReset, true,
                [&](const std::string& st, uint32_t done, uint32_t tot) {
                    int pct = tot ? (st == "erase" ? 15 * done / tot : st == "program" ? 15 + 65 * done / tot : 80 + 20 * done / tot) : 0;
                    QMetaObject::invokeMethod(progress, [progress, pct]() { progress->setValue(pct); }, Qt::QueuedConnection);
                });
            postLog(QString("[OK] Flashed & verified %1 bytes on %2!").arg(img.data.size()).arg(QString::fromStdString(info.name)));
            auto refreshed = dap.probeTarget(speed, false, true);
            QString txt = formatTargetText(refreshed);
            QMetaObject::invokeMethod(winPtr, [&, txt]() { edtTarget->setPlainText(txt); }, Qt::QueuedConnection);
        });
    });

    // Verify
    QObject::connect(btnVerify, &QPushButton::clicked, [&]() {
        QString fwPath = edtFile->text().trimmed();
        if (fwPath.isEmpty()) { QMessageBox::warning(&win, "No File", "Select a firmware file first."); return; }
        uint32_t baseAddr = parseHexOrDec(edtBaseAddr->text(), 0x08000000);
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool underReset = chkUnderReset->isChecked();
        runBg([&, fwPath, baseAddr, speed, underReset]() {
            auto img = loadFirmwareImageLinux(fwPath.toStdString(), baseAddr);
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            auto res = dap.verifyImage(img.baseAddr, img.data, speed, underReset,
                [&](const std::string&, uint32_t d, uint32_t t) {
                    int pct = t ? static_cast<int>(100 * d / t) : 100;
                    QMetaObject::invokeMethod(progress, [progress, pct]() { progress->setValue(pct); }, Qt::QueuedConnection);
                });
            if (res.mismatches == 0) postLog("[OK] Verify PASSED 100%!");
            else postLog(QString("[FAIL] Verify mismatch: %1 words!").arg(res.mismatches));
        });
    });

    // Reset
    QObject::connect(btnReset, &QPushButton::clicked, [&]() {
        bool ocd = chkUseOcd->isChecked();
        runBg([&, ocd]() {
            if (ocd) { runOcdSync({"init", "reset run", "exit"}); return; }
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            dap.hardResetPulse(50);
            postLog("[SWD] Hardware Reset (nRESET) pulse sent.");
        });
    });

    // Erase
    QObject::connect(btnErase, &QPushButton::clicked, [&]() {
        if (QMessageBox::question(&win, "Confirm Mass Erase", "Erase entire target Flash?") != QMessageBox::Yes) return;
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool underReset = chkUnderReset->isChecked();
        bool ocd = chkUseOcd->isChecked();
        runBg([&, speed, underReset, ocd]() {
            if (ocd) { runOcdSync({"init", "halt", "flash erase_sector 0 0 last", "exit"}); return; }
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            auto info = dap.massErase(speed, underReset);
            postLog("[OK] Mass Erase completed on " + QString::fromStdString(info.name));
            auto refreshed = dap.probeTarget(speed, false, true);
            QString txt = formatTargetText(refreshed);
            QMetaObject::invokeMethod(winPtr, [&, txt]() { edtTarget->setPlainText(txt); }, Qt::QueuedConnection);
        });
    });

    // Debug Halt / Run / Step / Regs
    auto doCpu = [&](const std::string& act) {
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool ocd = chkUseOcd->isChecked();
        runBg([&, act, speed, ocd]() {
            if (ocd) { runOcdSync({"init", act == "halt" ? "halt" : "resume", "exit"}); return; }
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            postLog("[DEBUG] " + QString::fromStdString(dap.cpuControl(act, speed)));
            if (act == "halt" || act == "step") {
                QString txt = formatDebugText(dap.readCpuStatus(speed, false));
                QMetaObject::invokeMethod(winPtr, [&, txt]() { edtDebug->setPlainText(txt); }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(winPtr, [&]() { edtDebug->setPlainText("CPU State: RUNNING"); }, Qt::QueuedConnection);
            }
        });
    };
    QObject::connect(btnHalt, &QPushButton::clicked, [&]() { doCpu("halt"); });
    QObject::connect(btnRun,  &QPushButton::clicked, [&]() { doCpu("run"); });
    QObject::connect(btnStep, &QPushButton::clicked, [&]() { doCpu("step"); });
    QObject::connect(btnRegs, &QPushButton::clicked, [&]() {
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool underReset = chkUnderReset->isChecked();
        runBg([&, speed, underReset]() {
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            QString txt = formatDebugText(dap.readCpuStatus(speed, underReset));
            QMetaObject::invokeMethod(winPtr, [&, txt]() { edtDebug->setPlainText(txt); }, Qt::QueuedConnection);
        });
    });

    // Tool Dialog 1: Hex Viewer & Mem32
    QDialog hexDlg(&win);
    hexDlg.setWindowTitle("Memory Hex Viewer & Mem32");
    hexDlg.setFixedSize(680, 450);
    {
        auto* hl = new QVBoxLayout(&hexDlg);
        auto* hr1 = new QHBoxLayout();
        auto* hAddr = new QLineEdit("0x08000000");
        auto* hLen = new QLineEdit("512");
        auto* hRead = new QPushButton("Read Hex");
        hr1->addWidget(new QLabel("Address:")); hr1->addWidget(hAddr);
        hr1->addWidget(new QLabel("Bytes:")); hr1->addWidget(hLen);
        hr1->addWidget(hRead);
        hl->addLayout(hr1);

        auto* hr2 = new QHBoxLayout();
        auto* mAddr = new QLineEdit("0xE0042000");
        auto* mVal = new QLineEdit("0x00000000");
        auto* mRead = new QPushButton("Read 32");
        auto* mWrite = new QPushButton("Write 32");
        hr2->addWidget(new QLabel("Mem32 Addr:")); hr2->addWidget(mAddr);
        hr2->addWidget(new QLabel("Value:")); hr2->addWidget(mVal);
        hr2->addWidget(mRead); hr2->addWidget(mWrite);
        hl->addLayout(hr2);

        auto* hOut = new QPlainTextEdit();
        hOut->setReadOnly(true);
        hOut->setFont(monoFont);
        hl->addWidget(hOut);

        QObject::connect(hRead, &QPushButton::clicked, [&, hAddr, hLen, hOut]() {
            uint32_t addr = parseHexOrDec(hAddr->text(), 0x08000000);
            uint32_t len = std::min(parseHexOrDec(hLen->text(), 512), uint32_t(16384));
            uint32_t speed = cmbSpeed->currentData().toUInt();
            bool ur = chkUnderReset->isChecked();
            runBg([&, addr, len, speed, ur, hOut]() {
                LinuxHidDevice hid; std::string err;
                if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
                LinuxCmsisDap dap(hid);
                auto data = dap.readMemoryBytes(addr, len, speed, ur);
                QString txt = formatHexDump(addr, data);
                QMetaObject::invokeMethod(hOut, [hOut, txt]() { hOut->setPlainText(txt); }, Qt::QueuedConnection);
            });
        });
        QObject::connect(mRead, &QPushButton::clicked, [&, mAddr, mVal]() {
            uint32_t addr = parseHexOrDec(mAddr->text(), 0xE0042000);
            uint32_t speed = cmbSpeed->currentData().toUInt();
            bool ur = chkUnderReset->isChecked();
            runBg([&, addr, speed, ur, mVal]() {
                LinuxHidDevice hid; std::string err;
                if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
                LinuxCmsisDap dap(hid);
                auto d = dap.readMemoryBytes(addr, 4, speed, ur);
                uint32_t v = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
                QString vs = QString("0x%1").arg(v, 8, 16, QChar('0')).toUpper();
                QMetaObject::invokeMethod(mVal, [mVal, vs]() { mVal->setText(vs); }, Qt::QueuedConnection);
            });
        });
        QObject::connect(mWrite, &QPushButton::clicked, [&, mAddr, mVal]() {
            uint32_t addr = parseHexOrDec(mAddr->text(), 0xE0042000);
            uint32_t val  = parseHexOrDec(mVal->text(), 0);
            uint32_t speed = cmbSpeed->currentData().toUInt();
            runBg([&, addr, val, speed]() {
                LinuxHidDevice hid; std::string err;
                if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
                LinuxCmsisDap dap(hid);
                if (dap.connectSwd() != 1) throw std::runtime_error("DAP_Connect failed");
                dap.swjClock(speed); dap.transferConfigure(2, 500, 0); dap.swdConfigure(0);
                dap.swdLineResetToSwd(); dap.readDp(0); dap.writeAbort(0x1E); dap.writeDp(8, 0); dap.writeDp(4, 0x50000000);
                for (int i = 0; i < 30; ++i) if ((dap.readDp(4) & 0xA0000000) == 0xA0000000) break;
                uint8_t c, a; std::vector<uint32_t> v;
                dap.transfer({{0x01, 0x23000012}}, c, a, v);
                dap.memWrite32(addr, val);
                dap.disconnect();
                postLog(QString("[MEM32] Wrote [0x%1] = 0x%2").arg(addr, 8, 16, QChar('0')).arg(val, 8, 16, QChar('0')));
            });
        });
    }

    // Tool Dialog 2: UART Terminal
    QDialog uartDlg(&win);
    uartDlg.setWindowTitle("UART Terminal (Virtual COM)");
    uartDlg.setFixedSize(650, 420);
    auto* uComCmb = new QComboBox(&uartDlg);
    {
        auto* ul = new QVBoxLayout(&uartDlg);
        auto* ur1 = new QHBoxLayout();
        auto* uBaud = new QComboBox();
        uBaud->addItems({"115200", "9600", "57600", "921600"});
        auto* uConn = new QPushButton("Connect COM");
        auto* uClear = new QPushButton("Clear");
        auto* uEcho = new QCheckBox("Echo TX"); uEcho->setChecked(true);
        ur1->addWidget(uComCmb, 1); ur1->addWidget(uBaud); ur1->addWidget(uConn); ur1->addWidget(uClear); ur1->addWidget(uEcho);
        ul->addLayout(ur1);

        auto* ur2 = new QHBoxLayout();
        auto* uCr = new QCheckBox("\\r (CR)"); uCr->setChecked(true);
        auto* uLf = new QCheckBox("\\n (LF)"); uLf->setChecked(true);
        auto* uTab = new QCheckBox("\\t (TAB)");
        auto* uEsc = new QCheckBox("Parse \\r \\n \\t \\xHH"); uEsc->setChecked(true);
        ur2->addWidget(new QLabel("Append:")); ur2->addWidget(uCr); ur2->addWidget(uLf); ur2->addWidget(uTab); ur2->addWidget(uEsc); ur2->addStretch();
        ul->addLayout(ur2);

        auto* uLog = new QPlainTextEdit();
        uLog->setReadOnly(true);
        uLog->setFont(monoFont);
        ul->addWidget(uLog, 1);

        auto* ur3 = new QHBoxLayout();
        auto* uIn = new QLineEdit();
        auto* uSend = new QPushButton("Send");
        ur3->addWidget(uIn, 1); ur3->addWidget(uSend);
        ul->addLayout(ur3);

        auto sendFn = [&, uIn, uLog, uEcho, uCr, uLf, uTab, uEsc]() {
            if (!serialPort.isOpen()) { uLog->appendPlainText("[WARN] COM port is not connected!"); return; }
            QString qtxt = uIn->text();
            uIn->clear();
            uIn->setFocus();
            std::string payload = qtxt.toStdString();
            if (uEsc->isChecked()) payload = decodeEscapes(payload);
            if (uTab->isChecked()) payload.push_back('\t');
            if (uCr->isChecked())  payload.push_back('\r');
            if (uLf->isChecked())  payload.push_back('\n');
            if (payload.empty()) return;
            if (serialPort.writeText(payload)) {
                if (uEcho->isChecked()) {
                    QString suf;
                    if (uTab->isChecked()) suf += "\\t";
                    if (uCr->isChecked())  suf += "\\r";
                    if (uLf->isChecked())  suf += "\\n";
                    QString shown = qtxt.isEmpty() ? suf : (qtxt + (suf.isEmpty() ? "" : " [" + suf + "]"));
                    uLog->appendPlainText("[TX] " + shown);
                }
            } else {
                uLog->appendPlainText("[ERROR] Failed to write to COM port.");
            }
        };
        QObject::connect(uSend, &QPushButton::clicked, sendFn);
        QObject::connect(uIn, &QLineEdit::returnPressed, sendFn);
        QObject::connect(uClear, &QPushButton::clicked, [uLog]() { uLog->clear(); });
        QObject::connect(uConn, &QPushButton::clicked, [&, uComCmb, uBaud, uConn, uLog, uIn]() {
            if (serialPort.isOpen()) {
                serialPort.close();
                uConn->setText("Connect COM");
                uLog->appendPlainText("[COM] Disconnected.");
                return;
            }
            QString port = uComCmb->currentText();
            if (port.isEmpty()) { QMessageBox::warning(&uartDlg, "Error", "No COM port found."); return; }
            uint32_t baud = uBaud->currentText().toUInt();
            std::string err;
            if (serialPort.open(port.toStdString(), baud, err,
                                [uLog](const std::string& rx) {
                                    QString s = QString::fromUtf8(rx.data(), static_cast<int>(rx.size()));
                                    QMetaObject::invokeMethod(uLog, [uLog, s]() {
                                        uLog->moveCursor(QTextCursor::End);
                                        uLog->insertPlainText(s);
                                    }, Qt::QueuedConnection);
                                },
                                [uConn]() {
                                    QMetaObject::invokeMethod(uConn, [uConn]() { uConn->setText("Connect COM"); }, Qt::QueuedConnection);
                                })) {
                uConn->setText("Disconnect");
                uLog->appendPlainText(QString("[COM] Opened %1 @ %2 bps").arg(port).arg(baud));
                uIn->setFocus();
            } else {
                QMessageBox::critical(&uartDlg, "COM Error", QString::fromStdString(err));
            }
        });
    }

    // Top Menu Bar (File, Tools, Help)
    auto* mFile = win.menuBar()->addMenu("&File");
    QObject::connect(mFile->addAction("Open Firmware (.bin/.hex/.elf)..."), &QAction::triggered, [&]() { btnBrowse->click(); });
    QObject::connect(mFile->addAction("Dump Flash to .bin..."), &QAction::triggered, [&]() {
        QString outP = QFileDialog::getSaveFileName(&win, "Save Flash Dump", "flash_dump.bin", "Binary (*.bin)");
        if (outP.isEmpty()) return;
        uint32_t speed = cmbSpeed->currentData().toUInt();
        bool ur = chkUnderReset->isChecked();
        runBg([&, outP, speed, ur]() {
            LinuxHidDevice hid; std::string err;
            if (!hid.open(getSelectedHid(), err)) throw std::runtime_error(err);
            LinuxCmsisDap dap(hid);
            auto info = dap.probeTarget(speed, ur, false);
            uint32_t len = info.flashKB ? info.flashKB * 1024U : 65536U;
            auto bytes = dap.readMemoryBytes(0x08000000, len, speed, ur,
                [&](const std::string&, uint32_t d, uint32_t t) {
                    int pct = t ? static_cast<int>(100 * d / t) : 100;
                    QMetaObject::invokeMethod(progress, [progress, pct]() { progress->setValue(pct); }, Qt::QueuedConnection);
                });
            std::ofstream f(outP.toStdString(), std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            postLog(QString("[OK] Saved Flash dump (%1 bytes).").arg(bytes.size()));
        });
    });
    mFile->addSeparator();
    QObject::connect(mFile->addAction("Exit"), &QAction::triggered, &win, &QMainWindow::close);

    auto* mTools = win.menuBar()->addMenu("&Tools");
    QObject::connect(mTools->addAction("Memory Hex Viewer && Mem32..."), &QAction::triggered, [&]() { hexDlg.show(); hexDlg.raise(); });
    QObject::connect(mTools->addAction("UART Terminal (Virtual COM)..."), &QAction::triggered, [&]() {
        uComCmb->clear();
        for (const auto& p : LinuxSerialPort::enumeratePorts()) uComCmb->addItem(QString::fromStdString(p));
        uartDlg.show(); uartDlg.raise();
    });
    mTools->addSeparator();
    QObject::connect(mTools->addAction("Toggle OpenOCD GDB Server (:3333)"), &QAction::triggered, [&]() {
        if (gdbProc.state() != QProcess::NotRunning) {
            gdbProc.terminate();
            postLog("[GDB] OpenOCD GDB Server stopped.");
        } else {
            QStringList args{"-f", "interface/cmsis-dap.cfg", "-f", cmbOcdTarget->currentData().toString(),
                             "-c", QString("adapter speed %1").arg(cmbSpeed->currentData().toUInt() / 1000)};
            gdbProc.start(ocdPath, args);
            postLog("[GDB] OpenOCD GDB Server started on localhost:3333");
        }
    });

    auto* mHelp = win.menuBar()->addMenu("&Help");
    QObject::connect(mHelp->addAction("LED Status Legend"), &QAction::triggered, [&]() {
        QMessageBox::information(&win, "LED Status Legend",
            "Probe LED (PC13) Status Legend:\n\n"
            "• Heartbeat (2 short blinks / 1.2s): Idle / Ready\n"
            "• Solid ON: SWD / Target Connected\n"
            "• Rapid Flicker (16.7 Hz): Flashing / Erasing / Verifying\n"
            "• Double Blink (1.0s): Target CPU Halted (Debug)\n"
            "• Triple Rapid Blink: SWD Error / Verify Mismatch\n\n"
            "On-board Button (PA0): Press to trigger Target Hardware Reset (nRESET).");
    });
    QObject::connect(mHelp->addAction("About"), &QAction::triggered, [&]() {
        QMessageBox::information(&win, "About",
            "TUE CMSIS-DAP Native C++ Programmer v2.1 (Ubuntu)\n"
            "Direct USB HID (/dev/hidraw*) + Virtual COM (/dev/ttyACM*)\n"
            "Supports direct SWD flashing (F1/F3/F4/G4/L4) without OpenOCD.");
    });

    postLog("[READY] TUE CMSIS-DAP Native C++ GUI ready. USB plug-and-play active.");
    win.show();
    return app.exec();
}
