#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace sd {
// Optional BDMV/META/DL and TN XML enrichment, independent of libxml2.
// Names attach only to existing navigation title numbers / existing MPLS chapters.
// No XML entities, network access, menu execution, or inferred timing/mappings.
QJsonObject enrichDiscMetadata(const QString &root, const QJsonObject &catalog);
} // namespace sd
