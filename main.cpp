// main.cpp - GRBL Sender with Serial Port, Jogging, Axis Display, G-code File Playback and Recovery

#include <QApplication>
#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QTextEdit>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QLineEdit>
#include <QInputDialog>
#include <QTableWidget>
#include <QDebug>
#include <QHeaderView>

class SimpleOpenGLView : public QOpenGLWidget {
    float x = 0, y = 0, z = 0;

public:
    void setPosition(float nx, float ny, float nz) {
        x = nx; y = ny; z = nz;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        p.setPen(Qt::green);
        p.drawText(10, 20, QString("Position: X=%1 Y=%2 Z=%3").arg(x).arg(y).arg(z));
    }
};

class GRBLSender : public QMainWindow {
    Q_OBJECT

    QSerialPort* serial;
    QTextEdit* log;
    QLabel* posLabel;
    QComboBox* portList;
    QTimer* statusTimer;
    SimpleOpenGLView* glView;
    QLineEdit* manualInput;

    QPushButton* loadBtn;
    QPushButton* playBtn;
    QPushButton* pauseBtn;
    QPushButton* resumeBtn;
    QPushButton* recoverBtn;

    QStringList gcodeLines;
    int currentLine = 0;
    int lastSentLine = -1;
    bool paused = false;

    float posX = 0, posY = 0, posZ = 0;
    float minX = 0, maxX = 200, minY = 0, maxY = 200, minZ = 0, maxZ = 100;

public:
    GRBLSender() {
        serial = new QSerialPort(this);
        QWidget* central = new QWidget;
        QVBoxLayout* layout = new QVBoxLayout(central);

        QHBoxLayout* topRow = new QHBoxLayout;
        portList = new QComboBox;
        QPushButton* refreshBtn = new QPushButton("Refresh");
        QPushButton* connectBtn = new QPushButton("Connect");
        topRow->addWidget(portList);
        topRow->addWidget(refreshBtn);
        topRow->addWidget(connectBtn);
        layout->addLayout(topRow);

        posLabel = new QLabel("X:0 Y:0 Z:0");
        layout->addWidget(posLabel);

        glView = new SimpleOpenGLView();
        glView->setMinimumHeight(100);
        layout->addWidget(glView);

        QHBoxLayout* gcodeControl = new QHBoxLayout;
        loadBtn = new QPushButton("Load G-code");
        playBtn = new QPushButton("Play");
        pauseBtn = new QPushButton("Pause");
        resumeBtn = new QPushButton("Resume");
        recoverBtn = new QPushButton("Recover Pos");
        gcodeControl->addWidget(loadBtn);
        gcodeControl->addWidget(playBtn);
        gcodeControl->addWidget(pauseBtn);
        gcodeControl->addWidget(resumeBtn);
        gcodeControl->addWidget(recoverBtn);
        layout->addLayout(gcodeControl);

        QHBoxLayout* manualRow = new QHBoxLayout;
        manualInput = new QLineEdit;
        QPushButton* sendManualBtn = new QPushButton("Send");
        manualRow->addWidget(new QLabel("Manual Command:"));
        manualRow->addWidget(manualInput);
        manualRow->addWidget(sendManualBtn);
        layout->addLayout(manualRow);

        log = new QTextEdit;
        log->setReadOnly(true);
        layout->addWidget(log);

        setCentralWidget(central);
        setWindowTitle("GRBL Sender");
        resize(500, 400);

        statusTimer = new QTimer(this);

        connect(refreshBtn, &QPushButton::clicked, this, &GRBLSender::refreshPortList);
        connect(connectBtn, &QPushButton::clicked, this, &GRBLSender::connectSerial);
        connect(serial, &QSerialPort::readyRead, this, &GRBLSender::readSerial);
        connect(statusTimer, &QTimer::timeout, this, &GRBLSender::requestStatus);

        connect(loadBtn, &QPushButton::clicked, this, &GRBLSender::loadGCode);
        connect(playBtn, &QPushButton::clicked, this, &GRBLSender::playGCode);
        connect(pauseBtn, &QPushButton::clicked, this, [=](){ paused = true; });
        connect(resumeBtn, &QPushButton::clicked, this, [=](){ paused = false; QTimer::singleShot(10, this, &GRBLSender::sendNextLine); });
        connect(recoverBtn, &QPushButton::clicked, this, &GRBLSender::recoverPosition);

        QPushButton* settingsBtn = new QPushButton("Settings");
        layout->addWidget(settingsBtn);
        connect(settingsBtn, &QPushButton::clicked, this, &GRBLSender::openSettingsDialog);

        connect(sendManualBtn, &QPushButton::clicked, this, [=](){
            QString cmd = manualInput->text().trimmed();
            if (!cmd.isEmpty()) sendCommand(cmd);
        });

        refreshPortList();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (!serial->isOpen()) return;
        QString cmd;
        float step = 1.0f;
        if (event->key() == Qt::Key_W && posY + step <= maxY) {
            posY += step; cmd = "G91 G0 Y" + QString::number(step);
        } else if (event->key() == Qt::Key_S && posY - step >= minY) {
            posY -= step; cmd = "G91 G0 Y-" + QString::number(step);
        } else if (event->key() == Qt::Key_A && posX - step >= minX) {
            posX -= step; cmd = "G91 G0 X-" + QString::number(step);
        } else if (event->key() == Qt::Key_D && posX + step <= maxX) {
            posX += step; cmd = "G91 G0 X" + QString::number(step);
        } else if (event->key() == Qt::Key_Q && posZ + step <= maxZ) {
            posZ += step; cmd = "G91 G0 Z" + QString::number(step);
        } else if (event->key() == Qt::Key_E && posZ - step >= minZ) {
            posZ -= step; cmd = "G91 G0 Z-" + QString::number(step);
        }
        if (!cmd.isEmpty()) {
            sendCommand(cmd);
            updatePosition();
        }
    }

private slots:
    void refreshPortList() {
        portList->clear();
        for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
            portList->addItem(info.portName());
    }

    void connectSerial() {
        if (serial->isOpen()) serial->close();
        serial->setPortName(portList->currentText());
        serial->setBaudRate(QSerialPort::Baud115200);
        if (serial->open(QIODevice::ReadWrite)) {
            log->append("Connected to " + serial->portName());
            statusTimer->start(500);
        } else {
            log->append("Failed to connect");
        }
    }

    void sendCommand(const QString& cmd) {
        if (serial->isOpen()) {
            QString fullCmd = cmd + "\n";
            serial->write(fullCmd.toUtf8());
            log->append(">> " + cmd);
        }
    }

    void readSerial() {
        QByteArray data = serial->readAll();
        QString str(data);
        log->append("<< " + str);
        if (str.contains("<")) {
            QRegExp rx("MPos:([0-9.\-]+),([0-9.\-]+),([0-9.\-]+)");
            if (rx.indexIn(str) >= 0) {
                posX = rx.cap(1).toFloat();
                posY = rx.cap(2).toFloat();
                posZ = rx.cap(3).toFloat();
                updatePosition();
            }
        }
        if (!paused && !gcodeLines.isEmpty()) QTimer::singleShot(10, this, &GRBLSender::sendNextLine);
    }

    void openSettingsDialog() {
        QDialog* dialog = new QDialog(this);
        dialog->setWindowTitle("GRBL Settings");
        dialog->resize(600, 500);

        QVBoxLayout* layout = new QVBoxLayout(dialog);
        QPushButton* fetchBtn = new QPushButton("Fetch Settings");
        QPushButton* applyBtn = new QPushButton("Apply Changes");

        layout->addWidget(fetchBtn);

        QTableWidget* table = new QTableWidget(0, 3);
        table->setHorizontalHeaderLabels({"Key", "Value", "Description"});
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table);
        layout->addWidget(applyBtn);

        dialog->setLayout(layout);
        dialog->show();

        // Descriptions for known GRBL settings
        QMap<QString, QString> descriptions = {
            {"$0", "Step pulse time (μs)"}, {"$1", "Step idle delay (ms)"},
            {"$2", "Step port invert mask"}, {"$3", "Direction port invert mask"},
            {"$4", "Step enable invert"}, {"$5", "Limit pins invert"},
            {"$6", "Probe pin invert"}, {"$10", "Status report mask"},
            {"$11", "Junction deviation"}, {"$12", "Arc tolerance"},
            {"$13", "Report inches"}, {"$20", "Soft limits"},
            {"$21", "Hard limits"}, {"$22", "Homing cycle"},
            {"$23", "Homing direction invert"}, {"$24", "Homing feed (mm/min)"},
            {"$25", "Homing seek (mm/min)"}, {"$26", "Homing debounce (ms)"},
            {"$27", "Homing pull-off (mm)"}, {"$30", "Max spindle speed"},
            {"$31", "Min spindle speed"}, {"$32", "Laser mode"},
            {"$100", "X steps/mm"}, {"$101", "Y steps/mm"}, {"$102", "Z steps/mm"},
            {"$110", "X max rate (mm/min)"}, {"$111", "Y max rate (mm/min)"},
            {"$112", "Z max rate (mm/min)"}, {"$120", "X accel (mm/s^2)"},
            {"$121", "Y accel (mm/s^2)"}, {"$122", "Z accel (mm/s^2)"},
            {"$130", "X max travel (mm)"}, {"$131", "Y max travel (mm)"},
            {"$132", "Z max travel (mm)"}
        };

        // To store original values for comparison
        QMap<QString, QString> originalValues;

        // Custom sorter for settings
        auto numericKeyLessThan = [](const QString& a, const QString& b) {
            return a.mid(1).toInt() < b.mid(1).toInt();
        };

        connect(fetchBtn, &QPushButton::clicked, this, [=]() mutable {
            table->setRowCount(0);
            QString buffer;
            QMap<QString, QString> found;

            statusTimer->stop();
            sendCommand("$$\n");

            disconnect(serial, &QSerialPort::readyRead, this, &GRBLSender::readSerial);

            QMetaObject::Connection* reader = new QMetaObject::Connection;
            *reader = connect(serial, &QSerialPort::readyRead, this, [=]() mutable {
                buffer += QString::fromUtf8(serial->readAll());

                if (buffer.contains("ok")) {
                    disconnect(*reader);
                    delete reader;

                    QRegExp rx("\\$(\\d+)=([\\d\\.\\-]+)");
                    int pos = 0;
                    while ((pos = rx.indexIn(buffer, pos)) != -1) {
                        QString key = "$" + rx.cap(1);
                        QString val = rx.cap(2);
                        found[key] = val;
                        originalValues[key] = val;
                        pos += rx.matchedLength();
                    }

                    // Sort keys
                    QStringList sortedKeys = found.keys();
                    std::sort(sortedKeys.begin(), sortedKeys.end(), numericKeyLessThan);

                    for (const QString& key : sortedKeys) {
                        QString desc = descriptions.value(key, "");
                        QString val = found[key];

                        int row = table->rowCount();
                        table->insertRow(row);
                        table->setItem(row, 0, new QTableWidgetItem(key));
                        table->setItem(row, 1, new QTableWidgetItem(val));
                        table->setItem(row, 2, new QTableWidgetItem(desc));

                        // Make value cell editable
                        table->item(row, 1)->setFlags(table->item(row, 1)->flags() | Qt::ItemIsEditable);
                    }

                    statusTimer->start(500);
                }
            });

            connect(serial, &QSerialPort::readyRead, this, &GRBLSender::readSerial);
        });

        connect(applyBtn, &QPushButton::clicked, this, [=]() {
            for (int row = 0; row < table->rowCount(); ++row) {
                QString key = table->item(row, 0)->text();
                QString val = table->item(row, 1)->text();

                if (originalValues.contains(key) && originalValues[key] != val) {
                    sendCommand(key + "=" + val + "\n");
                    qDebug() << "Sent:" << key + "=" + val;
                }
            }
            log->append("Sent modified GRBL settings.");
        });
    }



    void updatePosition() {
        posLabel->setText(QString("X:%1 Y:%2 Z:%3").arg(posX).arg(posY).arg(posZ));
        glView->setPosition(posX, posY, posZ);
    }

    void requestStatus() {
        sendCommand("?");
    }

    void loadGCode() {
        QString fileName = QFileDialog::getOpenFileName(this, "Open G-code", "", "G-code Files (*.nc *.gcode *.txt)");
        if (fileName.isEmpty()) return;
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            gcodeLines.clear();
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (!line.isEmpty())
                    gcodeLines << line;
            }
            log->append(QString("Loaded %1 lines").arg(gcodeLines.size()));
            currentLine = 0;
            lastSentLine = -1;
        }
    }

    void playGCode() {
        if (gcodeLines.isEmpty()) return;
        paused = false;
        currentLine = 0;
        sendNextLine();
    }

    void sendNextLine() {
        if (paused || currentLine >= gcodeLines.size()) return;
        lastSentLine = currentLine;
        sendCommand(gcodeLines[currentLine]);
        currentLine++;
    }

    void recoverPosition2() {
        if (lastSentLine >= 0 && lastSentLine < gcodeLines.size()) {
            currentLine = lastSentLine;
            log->append(QString("Recovering to line %1: %2").arg(currentLine).arg(gcodeLines[currentLine]));
            paused = false;
            sendNextLine();
        } else {
            log->append("No recovery point available.");
        }
    }

    void recoverPosition() {
        bool okX, okY, okZ;
        float rx = QInputDialog::getDouble(this, "Recover - X", "Enter X:", posX, -10000, 10000, 3, &okX);
        float ry = QInputDialog::getDouble(this, "Recover - Y", "Enter Y:", posY, -10000, 10000, 3, &okY);
        float rz = QInputDialog::getDouble(this, "Recover - Z", "Enter Z:", posZ, -10000, 10000, 3, &okZ);

        if (!(okX && okY && okZ)) {
            log->append("Recovery cancelled.");
            return;
        }

        int closestLine = findClosestGCodeLine(rx, ry, rz);
        if (closestLine >= 0) {
            currentLine = closestLine;
            log->append(QString("Recovering to line %1: %2").arg(currentLine).arg(gcodeLines[currentLine]));
            paused = false;
            sendNextLine();
        } else {
            log->append("No matching G-code line found for recovery.");
        }
    }

    int findClosestGCodeLine(float targetX, float targetY, float targetZ) {
        float bestDist = 1e9;
        int bestLine = -1;

        float curX = 0, curY = 0, curZ = 0;
        for (int i = 0; i < gcodeLines.size(); ++i) {
            QString line = gcodeLines[i];
            if (line.startsWith("G0") || line.startsWith("G1")) {
                QRegExp rxX("X(-?\\d*\\.?\\d+)");
                QRegExp rxY("Y(-?\\d*\\.?\\d+)");
                QRegExp rxZ("Z(-?\\d*\\.?\\d+)");

                if (rxX.indexIn(line) != -1) curX = rxX.cap(1).toFloat();
                if (rxY.indexIn(line) != -1) curY = rxY.cap(1).toFloat();
                if (rxZ.indexIn(line) != -1) curZ = rxZ.cap(1).toFloat();

                float dx = curX - targetX;
                float dy = curY - targetY;
                float dz = curZ - targetZ;
                float dist = dx*dx + dy*dy + dz*dz;

                if (dist < bestDist) {
                    bestDist = dist;
                    bestLine = i;
                }
            }
        }
        return bestLine;
    }

};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    GRBLSender sender;
    sender.show();
    return app.exec();
}

#include "main.moc"
