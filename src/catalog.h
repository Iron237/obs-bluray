#pragma once
#include "common.h"
#include <libbluray/bluray.h>
namespace sd {
QString discRoot(QString path);
bool isDisc(const QString &path);
QJsonObject inspectSource(const QString &path);
QJsonObject titleJson(const BLURAY_TITLE_INFO *info);
QByteArray codecHeaders(const QString &clipPath,int videoPid,bool hevc);
}
