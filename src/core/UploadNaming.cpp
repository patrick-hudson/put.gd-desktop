#include "UploadNaming.h"
#include <QFileInfo>
#include <QHash>
#include <QUuid>

namespace putgd {
namespace {
const QStringList tokens{"name", "ext", "yyyy", "MM", "dd", "HH", "mm", "ss", "uuid"};

bool safeCharacter(QChar character) {
 return character != '/' && character != '\\' && character.category() != QChar::Other_Control;
}

Error invalidName() {
 return {"filename_invalid", "A filename must be nonempty and contain no path separators or control characters", {}};
}

Error checkName(const QString& name) {
 if (name.isEmpty() || name == "." || name == "..") return invalidName();
 for (const auto character : name) if (!safeCharacter(character)) return invalidName();
 return {};
}

// Bound UTF-8 bytes while retaining the extension and complete Unicode characters.
QString boundedName(QString name, int maximumBytes, const QString& addition = {}) {
 const auto suffix = QFileInfo(name).suffix();
 QString extension = suffix.isEmpty() ? QString{} : "." + suffix;
 auto base = extension.isEmpty() ? name : name.chopped(extension.size());
 const auto chopCharacter = [](QString& text, qsizetype& bytes) {
  if (text.isEmpty()) return;
  const bool pair = text.at(text.size()-1).isLowSurrogate() && text.size() > 1 && text.at(text.size()-2).isHighSurrogate();
  const auto count=pair?2:1;
  bytes-=text.right(count).toUtf8().size();
  text.chop(count);
 };
 maximumBytes = qMax(1, maximumBytes);
 auto extensionBytes=extension.toUtf8().size();
 while (extensionBytes > qMax(1, maximumBytes/3)) chopCharacter(extension,extensionBytes);
 if (extension == ".") {extension.clear();extensionBytes=0;}
 auto tail = addition;
 auto tailBytes=tail.toUtf8().size();
 // When a provider cannot fit a UUID suffix, use a compact UUID instead of
 // retaining just the separator and producing the same collision every time.
 if (!tail.isEmpty() && tailBytes+extensionBytes >= maximumBytes) {
  extension.clear();extensionBytes=0;
  if (tail.startsWith('-')) {tail.remove(0, 1);--tailBytes;}
  while (tailBytes > maximumBytes) chopCharacter(tail,tailBytes);
 }
 auto baseBytes=base.toUtf8().size();
 while (baseBytes+tailBytes+extensionBytes > maximumBytes && !base.isEmpty()) chopCharacter(base,baseBytes);
 const auto output = base + tail + extension;
 return output.isEmpty() || output == "." || output == ".." ? "_" : output;
}
}

Error UploadNaming::validateTemplate(const QString& format) {
 if (auto error = checkName(format)) return error;
 for (qsizetype index = 0; index < format.size(); ++index) {
  if (format[index] == '}') return {"filename_template_invalid", "A filename template contains an unmatched closing brace", {}};
  if (format[index] != '{') continue;
  const auto end = format.indexOf('}', index + 1);
  if (end < 0 || !tokens.contains(format.mid(index + 1, end - index - 1)))
   return {"filename_template_invalid", "A filename template contains an unknown or unclosed token", {}};
  index = end;
 }
 return {};
}

FilenameResult UploadNaming::renderTemplate(const QString& format, const Artifact& artifact,
                                          const QDateTime& acquired, const QString& unique) {
 if (auto error = validateTemplate(format)) return {{}, error};
 const auto filename = QFileInfo(artifact.name).fileName();
 const auto extension = QFileInfo(filename).suffix();
 const auto name = extension.isEmpty() ? filename : filename.chopped(extension.size() + 1);
 const QHash<QString, QString> values{{"name", name}, {"ext", extension},
  {"yyyy", acquired.toString("yyyy")}, {"MM", acquired.toString("MM")},
  {"dd", acquired.toString("dd")}, {"HH", acquired.toString("HH")},
  {"mm", acquired.toString("mm")}, {"ss", acquired.toString("ss")},
  {"uuid", unique.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : unique}};
 QString output;
 for (qsizetype index = 0; index < format.size(); ++index) {
  if (format[index] != '{') { output += format[index]; continue; }
  const auto end = format.indexOf('}', index + 1);
  output += values.value(format.mid(index + 1, end - index - 1));
  index = end;
 }
 if (auto error = checkName(output)) return {{}, error};
 return {output, {}};
}

FilenameResult UploadNaming::acquiredName(const Artifact& artifact, const Settings& settings,
                                         const QString& action, const QDateTime& acquired) {
 QString format;
 if (settings.namingMode == "generated") format = QFileInfo(artifact.name).suffix().isEmpty() ? "{uuid}" : "{uuid}.{ext}";
 else if (artifact.originalFile) {
  if (settings.namingMode == "original") {
   if (auto error = checkName(artifact.name)) return {{}, error};
   return {artifact.name, {}};
  }
  format = settings.fileTemplate;
 } else if (artifact.mime == "image/png")
  format = action == "clipboard" ? settings.imageTemplate : settings.captureTemplate;
 else format = settings.textTemplate;
 auto result = renderTemplate(format, artifact, acquired, artifact.id);
 if (!result.error) result.name = boundedName(result.name, 255);
 return result;
}

QString UploadNaming::candidate(const Artifact& artifact, const QString& mode, bool rename, int maximumBytes) {
 auto name = QFileInfo(artifact.name).fileName();
 name.replace(QChar(0), QChar('_'));
 if (!artifact.namingPrepared && mode == "generated") {
  const auto extension = QFileInfo(name).suffix();
  name = QUuid::createUuid().toString(QUuid::WithoutBraces) + (extension.isEmpty() ? QString{} : "." + extension);
 }
 return rename ? renamedCandidate(name, maximumBytes) : boundedName(name, maximumBytes);
}

QString UploadNaming::renamedCandidate(const QString& name, int maximumBytes) {
 return boundedName(name, maximumBytes, "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QList<NamedItem> UploadNaming::batch(const QList<Artifact>& artifacts, const QString& mode,
                                   const QString& policy, int maximumBytes) {
 QList<NamedItem> result;
 QHash<QString, QList<int>> groups;
 for (const auto& artifact : artifacts) {
  auto name = candidate(artifact, mode, false, maximumBytes);
  const int index = result.size();
  result.append({name, "pending"});
  if (groups.contains(name)) {
   if (policy == "overwrite") {
    result[index].status = "conflict";
    for (const int previous : groups[name]) result[previous].status = "conflict";
   } else if (policy == "skip") result[index].status = "skipped";
   else {
    int attempts=0;
    do { name = renamedCandidate(result[index].name, maximumBytes); ++attempts; }
    while (groups.contains(name) && attempts < 32);
    if (groups.contains(name)) result[index].status = "conflict";
    result[index].name = name;
   }
  }
  groups[name].append(index);
 }
 return result;
}
}
