#include "disc_metadata.h"
#include <QtCore>
#include <algorithm>

namespace sd {
namespace {
constexpr qint64 maxFileBytes = 1024 * 1024;
constexpr qint64 maxTotalBytes = 8 * 1024 * 1024;
constexpr int maxFiles = 128;
constexpr int maxCandidates = 4096;
constexpr int maxNames = 4096;
constexpr int maxTextLength = 4096;
struct Candidate {
    QString path, relative, language;
    int playlist = -1;
};
struct Names {
    bool valid = false;
    QString disc;
    QMap<int, QString> titles;
    QStringList chapters;
};
int languageRank(const QString &language) {
    const auto index = QStringList{"zho", "jpn", "eng"}.indexOf(language);
    return index < 0 ? 3 : int(index);
}
bool preferred(const Candidate &a, const Candidate &b) {
    const int ar = languageRank(a.language), br = languageRank(b.language);
    if (ar != br)
        return ar < br;
    if (a.language != b.language)
        return a.language < b.language;
    return a.relative < b.relative;
}
QList<Candidate> candidates(const QString &root, bool chapters) {
    const QString relative = chapters ? "BDMV/META/TN/" : "BDMV/META/DL/";
    const QRegularExpression pattern(chapters ? "^tnmt_([a-z]{3})_([0-9]{5})\\.xml$" : "^bdmt_([a-z]{3})\\.xml$",
                                     QRegularExpression::CaseInsensitiveOption);
    QList<Candidate> result;
    QDirIterator iterator(QDir(root).filePath(relative), QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot);
    int visited = 0;
    while (iterator.hasNext() && visited++ < maxCandidates) {
        iterator.next();
        const auto match = pattern.match(iterator.fileName());
        if (!match.hasMatch())
            continue;
        result.append({iterator.filePath(), relative + iterator.fileName(), match.captured(1).toLower(),
                       chapters ? match.captured(2).toInt() : -1});
    }
    std::sort(result.begin(), result.end(), preferred);
    return result;
}
Names parse(const QByteArray &bytes, bool chapters) {
    Names result;
    QXmlStreamReader xml(bytes);
    xml.setEntityExpansionLimit(4096);
    QStringList stack;
    QString text;
    int captureDepth = 0, captureKind = 0, captureTitle = -1, nodes = 0, nameCount = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (++nodes > 100000 || token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference)
            return {};
        if (token == QXmlStreamReader::StartElement) {
            const QString parent = stack.isEmpty() ? QString() : stack.last();
            const QString name = xml.name().toString();
            stack.append(name);
            if (stack.size() > 64)
                return {};
            if (!captureDepth) {
                int kind = 0;
                if (chapters && parent == "chapters" && name == "name")
                    kind = 1;
                if (!chapters && parent == "title" && name == "name")
                    kind = 2;
                if (!chapters && parent == "tableOfContents" && name == "titleName")
                    kind = 3;
                if (kind) {
                    if (++nameCount > maxNames)
                        return {};
                    captureKind = kind;
                    captureDepth = int(stack.size());
                    text.clear();
                    captureTitle = -1;
                    if (kind == 3) {
                        bool ok = false;
                        const int number = xml.attributes().value("titleNumber").toInt(&ok);
                        if (ok && number > 0 && number <= 65535)
                            captureTitle = number;
                    }
                }
            }
        } else if (token == QXmlStreamReader::Characters && captureDepth) {
            if (text.size() + xml.text().size() > maxTextLength)
                return {};
            text += xml.text();
        } else if (token == QXmlStreamReader::EndElement) {
            if (captureDepth == stack.size()) {
                const auto value = text.simplified();
                if (captureKind == 1)
                    result.chapters.append(value); // Empty names still occupy a chapter slot.
                else if (captureKind == 2 && result.disc.isEmpty())
                    result.disc = value;
                else if (captureKind == 3 && captureTitle > 0 && !value.isEmpty() &&
                         !result.titles.contains(captureTitle))
                    result.titles.insert(captureTitle, value);
                captureDepth = 0;
            }
            if (!stack.isEmpty())
                stack.removeLast();
        }
    }
    result.valid = !xml.hasError();
    return result;
}
QJsonObject source(const Candidate &file) {
    return {{"kind", "bdmv-xml"}, {"language", file.language}, {"file", file.relative}};
}
struct Reader {
    int count = 0;
    int names = 0;
    qint64 bytes = 0;
    Names read(const Candidate &candidate, bool chapters) {
        if (count >= maxFiles || bytes >= maxTotalBytes)
            return {};
        ++count;
        QFile file(candidate.path);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maxFileBytes ||
            file.size() > maxTotalBytes - bytes)
            return {};
        const auto data = file.read(qMin(maxFileBytes, maxTotalBytes - bytes) + 1);
        bytes += data.size();
        if (data.size() > maxFileBytes || bytes > maxTotalBytes)
            return {};
        const auto parsed = parse(data, chapters);
        const int entries = int(parsed.chapters.size() + parsed.titles.size()) + (parsed.disc.isEmpty() ? 0 : 1);
        if (names + entries > 8192)
            return {};
        names += entries;
        return parsed;
    }
};
} // namespace

QJsonObject enrichDiscMetadata(const QString &root, const QJsonObject &catalog) {
    auto result = catalog;
    Reader reader;
    QJsonArray sources;
    for (const auto &candidate : candidates(root, false)) {
        const auto names = reader.read(candidate, false);
        if (!names.valid || (names.disc.isEmpty() && names.titles.isEmpty()))
            continue;
        const auto provenance = source(candidate);
        if (!names.disc.isEmpty()) {
            result["discName"] = names.disc;
            result["discNameSource"] = provenance;
        }
        auto titles = result["discTitles"].toArray();
        for (qsizetype i = 0; i < titles.size(); ++i) {
            auto title = titles[i].toObject();
            const int number = title["number"].toInt(-1);
            if (names.titles.contains(number)) {
                title["name"] = names.titles[number];
                title["nameSource"] = provenance;
                titles[i] = title;
            }
        }
        result["discTitles"] = titles;
        sources.append(provenance);
        break;
    }
    QMap<int, QList<Candidate>> playlists;
    for (const auto &candidate : candidates(root, true))
        playlists[candidate.playlist].append(candidate);
    auto titles = result["titles"].toArray();
    for (qsizetype i = 0; i < titles.size(); ++i) {
        auto title = titles[i].toObject();
        const int playlist = title["playlist"].toInt(-1);
        for (const auto &candidate : playlists.value(playlist)) {
            const auto names = reader.read(candidate, true);
            if (!names.valid || std::none_of(names.chapters.begin(), names.chapters.end(),
                                             [](const QString &name) { return !name.isEmpty(); }))
                continue;
            auto chapters = title["chapters"].toArray();
            const auto provenance = source(candidate);
            int matched = 0;
            for (qsizetype c = 0; c < qMin(chapters.size(), names.chapters.size()); ++c) {
                if (names.chapters[c].isEmpty())
                    continue;
                auto chapter = chapters[c].toObject();
                chapter["name"] = names.chapters[c];
                chapter["nameSource"] = provenance;
                chapters[c] = chapter;
                ++matched;
            }
            if (!matched)
                continue;
            title["chapters"] = chapters;
            title["chapterNames"] = QJsonObject{{"source", provenance},
                                                {"xmlCount", names.chapters.size()},
                                                {"chapterCount", chapters.size()},
                                                {"matchedCount", matched},
                                                {"countMatches", names.chapters.size() == chapters.size()}};
            titles[i] = title;
            sources.append(provenance);
            break;
        }
    }
    if (sources.isEmpty())
        return catalog;
    result["titles"] = titles;
    result["metadataSources"] = sources;
    auto navigation = result["navigation"].toObject();
    navigation["metadataAvailable"] = true;
    navigation["xmlMetadataAvailable"] = true;
    result["navigation"] = navigation;
    return result;
}
} // namespace sd
