#pragma once
#include <QtCore>
#include <QtNetwork>
#include <gst/gst.h>

namespace sd {
QString dataRoot();
QString serverName();
QString workerName(const QString &id);
void setupRuntime();
void initGst();
QJsonObject readJson(const QString &path);
void saveJson(const QString &path, const QJsonObject &value, bool protect = false);
QJsonObject loadProtected(const QString &path);
QString redact(QString text);
QJsonObject publicConfig(QJsonObject config);
QString validate(const QJsonObject &config);
QString gstQuote(const QString &text);
QString timeText(double seconds);
void printJson(const QJsonObject &object);
QString encoderFactory(const QString &backend, const QString &codec);
}
