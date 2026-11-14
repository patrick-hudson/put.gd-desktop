#include "HttpTemplate.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <cmath>
namespace putgd::http {
namespace {
constexpr int maximumDepth = 16;
constexpr int maximumXmlNodes = 50000;
constexpr qsizetype maximumParsedBytes = 4 * 1024 * 1024;

bool parseSequence(const QString& t, qsizetype& i, bool parameter, int depth, Template& out, QString* problem);
bool parseCall(const QString& t, qsizetype& i, int depth, TemplateNode& node, QString* problem) {
    if (depth > maximumDepth) { if (problem) *problem = "Placeholders are nested too deeply."; return false; }
    node.call = true;
    QString name;
    for (; i < t.size(); ++i) {
        const auto c = t[i];
        if (c == '\\' && i + 1 < t.size()) { name.append(t[++i]); continue; }
        if (c == ':' || c == '}') break;
        if (c == '{' || c == '|') { if (problem) *problem = "A placeholder name is malformed."; return false; }
        name.append(c);
    }
    if (i >= t.size()) { if (problem) *problem = "A placeholder is missing its closing brace."; return false; }
    node.text = name.trimmed().toLower();
    if (node.text.isEmpty()) { if (problem) *problem = "A placeholder has no name."; return false; }
    if (t[i] == '}') { ++i; return true; }
    while (true) {
        ++i; // past ':' or '|'
        Template parameter;
        if (!parseSequence(t, i, true, depth + 1, parameter, problem)) return false;
        node.parameters.append(parameter);
        if (i >= t.size()) { if (problem) *problem = "A placeholder is missing its closing brace."; return false; }
        if (t[i] == '}') { ++i; return true; }
    }
}
bool parseSequence(const QString& t, qsizetype& i, bool parameter, int depth, Template& out, QString* problem) {
    QString literal;
    const auto flush = [&] { if (!literal.isEmpty()) { out.append(TemplateNode{false, literal, {}}); literal.clear(); } };
    while (i < t.size()) {
        const auto c = t[i];
        if (c == '\\') {
            // A trailing backslash is literal text.
            if (i + 1 < t.size()) { literal.append(t[i + 1]); i += 2; } else { literal.append(c); ++i; }
            continue;
        }
        if (c == '{') {
            flush();
            ++i;
            TemplateNode node;
            if (!parseCall(t, i, depth, node, problem)) return false;
            out.append(node);
            continue;
        }
        if (parameter && (c == '|' || c == '}')) break;
        literal.append(c);
        ++i;
    }
    flush();
    return true;
}

struct PathStep { bool index = false; QString name; qint64 position = 0; };
bool jsonSteps(const QString& text, QList<PathStep>& steps) {
    qsizetype i = 0;
    const auto n = text.size();
    if (i < n && text[i] == '$') ++i;
    bool first = true;
    while (i < n) {
        if (text[i] == '.') {
            ++i;
            if (i >= n || text[i] == '.' || text[i] == '[') return false; // "..", trailing dot
        } else if (text[i] == '[') {
            ++i;
            if (i < n && (text[i] == '\'' || text[i] == '"')) {
                const auto quote = text[i++];
                QString name;
                while (i < n && text[i] != quote) {
                    if (text[i] == '\\' && i + 1 < n) ++i;
                    name.append(text[i++]);
                }
                if (i + 1 >= n || text[i + 1] != ']') return false;
                i += 2;
                steps.append(PathStep{false, name, 0});
            } else {
                QString digits;
                while (i < n && text[i].isDigit()) digits.append(text[i++]);
                if (digits.isEmpty() || digits.size() > 9 || i >= n || text[i] != ']') return false;
                ++i;
                steps.append(PathStep{true, {}, digits.toLongLong()});
            }
            first = false;
            continue;
        } else if (!first) return false;
        QString name;
        while (i < n && text[i] != '.' && text[i] != '[') {
            if (QStringLiteral("*?()@,: '\"").contains(text[i])) return false;
            name.append(text[i++]);
        }
        if (name.isEmpty()) return false;
        steps.append(PathStep{false, name, 0});
        first = false;
    }
    return !steps.isEmpty();
}
QString jsonText(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const auto number = value.toDouble();
        if (std::isfinite(number) && std::floor(number) == number && std::fabs(number) < 9.0e15) return QString::number(qint64(number));
        return QString::number(number, 'g', 17);
    }
    return {};
}

// A minimal element tree read with QXmlStreamReader: Qt 6 has no XPath engine, and a
// reply only needs the location paths below.
struct XmlNode {
    QString name, local;
    QList<QPair<QString, QString>> attributes; // qualified name → value
    QList<QPair<int, QString>> content;        // (child index, {}) or (-1, text), in document order
    QList<int> children;
};
struct XmlTree {
    QList<XmlNode> nodes; // node 0 is the document
    bool ok = false;
    QString value(int index, int depth = 0) const {
        QString out;
        if (depth > 256) return out;
        for (const auto& [child, text] : nodes[index].content) out += child < 0 ? text : value(child, depth + 1);
        return out;
    }
};
XmlTree readXml(const QString& xml) {
    XmlTree tree;
    tree.nodes.append(XmlNode{});
    if (xml.size() > maximumParsedBytes) return tree;
    QXmlStreamReader reader(xml);
    reader.setNamespaceProcessing(false);
    QList<int> open{0};
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (tree.nodes.size() >= maximumXmlNodes || open.size() > 200) return tree;
            XmlNode node;
            node.name = reader.qualifiedName().toString();
            node.local = node.name.mid(node.name.indexOf(':') + 1);
            for (const auto& attribute : reader.attributes()) node.attributes.append(QPair<QString, QString>(attribute.qualifiedName().toString(), attribute.value().toString()));
            const int index = int(tree.nodes.size());
            tree.nodes.append(node);
            tree.nodes[open.last()].children.append(index);
            tree.nodes[open.last()].content.append(QPair<int, QString>(index, QString()));
            open.append(index);
        } else if (token == QXmlStreamReader::EndElement) {
            if (open.size() > 1) open.removeLast();
        } else if (token == QXmlStreamReader::Characters) {
            tree.nodes[open.last()].content.append(QPair<int, QString>(-1, reader.text().toString()));
        }
    }
    tree.ok = !reader.hasError() && tree.nodes.size() > 1;
    return tree;
}
struct XPathPredicate { enum Kind { Position, Last, Attribute, Child } kind = Position; int position = 0; QString name, value; };
struct XPathStep {
    bool descendant = false; // reached through "//"
    enum Kind { Element, Attribute, Text } kind = Element;
    QString name;            // "*" matches any element
    QList<XPathPredicate> predicates;
};
bool xpathName(const QString& text, qsizetype& i, QString& name) {
    while (i < text.size() && (text[i].isLetterOrNumber() || QStringLiteral("_-.:").contains(text[i]))) name.append(text[i++]);
    // One colon separates a namespace prefix; "::" is an axis, which is not supported.
    return !name.isEmpty() && name.count(':') <= 1 && !name.startsWith(':') && !name.endsWith(':');
}
bool xpathSteps(const QString& input, QList<XPathStep>& steps) {
    const auto text = input.trimmed();
    qsizetype i = 0;
    const auto n = text.size();
    if (n == 0 || n > 1024) return false;
    bool first = true;
    while (i < n) {
        XPathStep step;
        if (text[i] == '/') {
            ++i;
            if (i < n && text[i] == '/') { step.descendant = true; ++i; }
        } else if (!first) return false;
        first = false;
        if (i >= n) return false;
        if (text[i] == '@') {
            ++i;
            step.kind = XPathStep::Attribute;
            if (!xpathName(text, i, step.name)) return false;
        } else if (text.mid(i, 6) == QStringLiteral("text()")) {
            step.kind = XPathStep::Text;
            i += 6;
        } else if (text[i] == '*') {
            step.name = "*";
            ++i;
        } else if (!xpathName(text, i, step.name)) return false;
        while (i < n && text[i] == '[') {
            if (step.kind != XPathStep::Element) return false;
            ++i;
            XPathPredicate predicate;
            if (text.mid(i, 6) == QStringLiteral("last()")) { predicate.kind = XPathPredicate::Last; i += 6; }
            else if (i < n && text[i].isDigit()) {
                QString digits;
                while (i < n && text[i].isDigit()) digits.append(text[i++]);
                if (digits.size() > 6 || digits.toInt() < 1) return false;
                predicate.position = digits.toInt();
            } else {
                predicate.kind = XPathPredicate::Child;
                if (i < n && text[i] == '@') { predicate.kind = XPathPredicate::Attribute; ++i; }
                if (!xpathName(text, i, predicate.name)) return false;
                if (i >= n || text[i] != '=') return false;
                ++i;
                if (i >= n || (text[i] != '\'' && text[i] != '"')) return false;
                const auto quote = text[i++];
                while (i < n && text[i] != quote) predicate.value.append(text[i++]);
                if (i >= n) return false;
                ++i;
            }
            if (i >= n || text[i] != ']') return false;
            ++i;
            step.predicates.append(predicate);
        }
        // An attribute or text() ends the path.
        if (step.kind != XPathStep::Element && i < n) return false;
        steps.append(step);
    }
    return !steps.isEmpty();
}
bool nameMatches(const XmlNode& node, const QString& name) {
    return name == "*" || node.name == name || node.local == name.mid(name.indexOf(':') + 1);
}
void descendants(const XmlTree& tree, int index, QList<int>& out, int depth = 0) {
    out.append(index);
    if (depth > 256) return;
    for (const int child : tree.nodes[index].children) descendants(tree, child, out, depth + 1);
}
QList<int> filter(const XmlTree& tree, QList<int> group, const XPathPredicate& predicate) {
    if (predicate.kind == XPathPredicate::Position) return predicate.position <= group.size() ? QList<int>{group[predicate.position - 1]} : QList<int>{};
    if (predicate.kind == XPathPredicate::Last) return group.isEmpty() ? group : QList<int>{group.last()};
    QList<int> kept;
    for (const int index : std::as_const(group)) {
        const auto& node = tree.nodes[index];
        bool match = false;
        if (predicate.kind == XPathPredicate::Attribute) {
            for (const auto& [name, value] : node.attributes)
                if ((name == predicate.name || name.mid(name.indexOf(':') + 1) == predicate.name) && value == predicate.value) match = true;
        } else {
            for (const int child : node.children)
                if (nameMatches(tree.nodes[child], predicate.name) && tree.value(child) == predicate.value) match = true;
        }
        if (match) kept.append(index);
    }
    return kept;
}

QString base64(const QString& text) { return QString::fromLatin1(text.toUtf8().toBase64()); }
QString encodeValue(const QString& value, RequestValues::Encode encode) {
    switch (encode) {
    case RequestValues::Encode::Url: return QString::fromLatin1(QUrl::toPercentEncoding(value));
    case RequestValues::Encode::Json: {
        // The value sits inside a JSON string literal in the Data template.
        const auto quoted = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
        return quoted.mid(2, quoted.size() - 4);
    }
    case RequestValues::Encode::Xml: return value.toHtmlEscaped();
    case RequestValues::Encode::None: break;
    }
    return value;
}
QString pick(const QStringList& values) { return values.isEmpty() ? QString() : values[QRandomGenerator::global()->bounded(int(values.size()))]; }

QString escaped(const QString& text) {
    QString out;
    for (const auto c : text) { if (QStringLiteral("{}|\\").contains(c)) out.append('\\'); out.append(c); }
    return out;
}
struct Renderer {
    const RequestValues* request = nullptr;
    const ResponseValues* response = nullptr;
    QString* problem = nullptr;
    // A response rendered without stored values stays a template: see ResponseValues.
    bool symbolic() const { return response && response->keepSecrets; }
    QString sequence(const Template& nodes, bool top = true) const {
        QString out;
        for (const auto& node : nodes) {
            if (!node.call) out += top && symbolic() ? escaped(node.text) : node.text;
            else if (node.text == "secret") out += secret(node, top);
            else out += top && symbolic() ? escaped(call(node)) : call(node);
        }
        return out;
    }
    QString secret(const TemplateNode& node, bool top) const {
        bool plain = true;
        QString key;
        if (!node.parameters.isEmpty()) for (const auto& part : node.parameters[0]) { if (part.call) plain = false; else key += part.text; }
        if (!plain || key.isEmpty()) return {};
        if (request && request->sampleSecrets) return QStringLiteral("secret");
        // Kept as written; inside another placeholder's parameter it has no value yet.
        if (symbolic()) return top ? "{secret:" + escaped(key) + "}" : QString();
        const auto* values = request ? request->secrets : response->secrets;
        if (!values) return {};
        if (!values->contains(key) && problem && problem->isEmpty()) *problem = "A stored value this destination refers to is missing.";
        return values->value(key);
    }
    QString call(const TemplateNode& node) const {
        QStringList p;
        for (const auto& parameter : node.parameters) p.append(sequence(parameter, false));
        const auto& name = node.text;
        if (name == "random") return pick(p);
        if (name == "base64") return p.isEmpty() ? QString() : base64(p[0]);
        if (name == "filename") {
            if (request) return encodeValue(request->fileName, request->encode);
            return QString::fromLatin1(QUrl::toPercentEncoding(response->fileName));
        }
        if (name == "input") {
            if (!request || !request->hasInput) {
                if (problem && problem->isEmpty()) *problem = "This uploader sends text ({input}); the file being uploaded is not text.";
                return {};
            }
            return encodeValue(request->input, request->encode);
        }
        if (!response) return {};
        if (name == "response") return response->body;
        if (name == "responseurl") return response->responseUrl;
        if (name == "header") {
            if (p.isEmpty()) return {};
            for (const auto& [key, value] : response->headers)
                if (QString::fromLatin1(key).compare(p[0].trimmed(), Qt::CaseInsensitive) == 0) return QString::fromUtf8(value);
            return {};
        }
        if (name == "json") {
            if (p.isEmpty()) return {};
            return p.size() > 1 ? jsonPathValue(p[0], p[1]) : jsonPathValue(response->body, p[0]);
        }
        if (name == "xml") {
            if (p.isEmpty()) return {};
            return p.size() > 1 ? xpathValue(p[0], p[1]) : xpathValue(response->body, p[0]);
        }
        if (name == "regex") {
            if (p.isEmpty()) return {};
            const auto input = p.size() > 2 ? p[0] : response->body;
            const auto pattern = p.size() > 2 ? p[1] : p[0];
            const auto group = p.size() > 2 ? p[2] : p.value(1);
            if (input.isEmpty() || pattern.isEmpty() || input.size() > maximumParsedBytes) return {};
            const QRegularExpression expression(pattern);
            if (!expression.isValid()) return {};
            const auto match = expression.match(input);
            if (!match.hasMatch()) return {};
            if (group.isEmpty()) return match.captured(0);
            bool number = false;
            const int index = group.toInt(&number);
            return number ? match.captured(index) : match.captured(group);
        }
        return {};
    }
};

const QStringList interactive{"select", "prompt", "inputbox", "outputbox", "output"};
const QStringList requestFunctions{"filename", "input", "random", "base64", "secret"};
const QStringList responseFunctions{"json", "xml", "regex", "header", "response", "responseurl", "filename", "random", "base64", "secret"};
QString literalOf(const Template& nodes, bool& plain) {
    QString out;
    for (const auto& node : nodes) { if (node.call) plain = false; else out += node.text; }
    return out;
}
// The most parameters each placeholder reads; one more is a mistake, not something to ignore.
int maximumParameters(const QString& name) {
    if (name == "random") return 8192;                // any number of choices
    if (name == "regex") return 3;                    // text | pattern | group
    if (name == "json" || name == "xml") return 2;    // text | path
    if (name == "base64" || name == "header" || name == "secret") return 1;
    return 0;                                         // filename, input, response, responseurl
}
QString checkNodes(const Template& nodes, TemplateUse use, bool nested = false) {
    for (const auto& node : nodes) {
        if (!node.call) continue;
        const auto& name = node.text;
        if (interactive.contains(name)) return QString("The placeholder {%1} asks the user a question while uploading; put.gd does not support it.").arg(name);
        if (!(use == TemplateUse::Request ? requestFunctions : responseFunctions).contains(name)) {
            if (use == TemplateUse::Request && responseFunctions.contains(name))
                return QString("The placeholder {%1} reads the reply and cannot be used in the request.").arg(name);
            if (use == TemplateUse::Response && name == "input") return QStringLiteral("The placeholder {input} cannot be used when reading the reply.");
            return QString("The placeholder {%1} is not supported.").arg(name.left(32));
        }
        const int count = int(node.parameters.size());
        if (name == "random" && count < 2) return QStringLiteral("{random} needs at least two choices.");
        if (QStringList{"base64", "json", "xml", "regex", "header", "secret"}.contains(name) && count < 1) return QString("{%1} needs a parameter.").arg(name);
        if (name == "secret") {
            bool plain = true;
            const auto key = literalOf(node.parameters[0], plain);
            if (!plain || count != 1 || key.trimmed().isEmpty()) return QStringLiteral("{secret} takes the name of one stored value, such as {secret:arguments/token}.");
            // A value read from the reply is recorded with its stored values still named, not
            // filled in; a name inside another placeholder would have nothing to stand for.
            if (nested && use == TemplateUse::Response) return QStringLiteral("{secret:…} stands on its own here; it cannot be used inside another placeholder.");
            continue;
        }
        if (const int most = maximumParameters(name); count > most)
            return most == 0 ? QString("{%1} takes no parameter.").arg(name) : QString("{%1} takes at most %2 parameter%3.").arg(name).arg(most).arg(most == 1 ? "" : "s");
        if (name == "json" || name == "xml") {
            bool plain = true;
            const auto path = literalOf(node.parameters.last(), plain);
            if (plain && name == "json" && !jsonPathSupported(path))
                return QStringLiteral("This JSON path is not supported. Use names, [index] and ['name'] steps, such as files[0].url.");
            if (plain && name == "xml" && !xpathSupported(path))
                return QStringLiteral("This XPath is not supported. Use steps such as /files/file[1]/url, //url or /file/@href.");
        }
        if (name == "regex") {
            bool plain = true;
            const auto pattern = literalOf(node.parameters[count > 2 ? 1 : 0], plain);
            if (plain && !QRegularExpression(pattern).isValid()) return QStringLiteral("The regular expression is not valid.");
        }
        for (const auto& parameter : node.parameters)
            if (const auto problem = checkNodes(parameter, use, true); !problem.isEmpty()) return problem;
    }
    return {};
}
bool nodesReadInput(const Template& nodes) {
    for (const auto& node : nodes) {
        if (!node.call) continue;
        if (node.text == "input") return true;
        for (const auto& parameter : node.parameters) if (nodesReadInput(parameter)) return true;
    }
    return false;
}
const QRegularExpression& oldPlaceholder() {
    static const QRegularExpression value("(?<!\\\\)\\$(?:json|xml|regex|header|random|select|prompt|inputbox|base64):[^$]*\\$|(?<!\\\\)\\$(?:response|responseurl|filename|input)\\$",
                                          QRegularExpression::CaseInsensitiveOption);
    return value;
}
}

Template parseTemplate(const QString& text, QString* problem) {
    Template out;
    qsizetype i = 0;
    if (problem) problem->clear();
    if (!parseSequence(text, i, false, 0, out, problem)) return {};
    return out;
}
QString templateProblem(const QString& text, TemplateUse use) {
    if (text.size() > 65536) return QStringLiteral("The value is too long.");
    QString problem;
    const auto nodes = parseTemplate(text, &problem);
    if (!problem.isEmpty()) return problem;
    return checkNodes(nodes, use);
}
bool usesOldSyntax(const QString& text) { return oldPlaceholder().match(text).hasMatch(); }
QString escapeTemplate(const QString& text) { return escaped(text); }
namespace {
void collectSecrets(const Template& nodes, QStringList& out) {
    for (const auto& node : nodes) {
        if (!node.call) continue;
        if (node.text == "secret" && !node.parameters.isEmpty()) {
            bool plain = true;
            const auto key = literalOf(node.parameters[0], plain);
            if (plain && !key.isEmpty() && !out.contains(key)) out.append(key);
            continue;
        }
        for (const auto& parameter : node.parameters) collectSecrets(parameter, out);
    }
}
}
QStringList secretReferences(const QString& text) {
    QStringList out;
    if (text.contains("secret", Qt::CaseInsensitive)) collectSecrets(parseTemplate(text), out);
    return out;
}
bool readsInput(const QString& text) { return nodesReadInput(parseTemplate(text)); }
QString migrateOldSyntax(const QString& text, const QStringList& regexList) {
    QString out;
    bool open = false;
    qsizetype start = 0; // where the current placeholder began in `out`
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            // Old escapes keep their character as literal text.
            const auto next = text[++i];
            if (QStringLiteral("{}|\\").contains(next)) out.append('\\');
            out.append(next);
            continue;
        }
        if (c == '$') {
            if (!open) { start = out.size(); out.append('{'); }
            else {
                out.append('}');
                // $regex:N|group$ named an entry of RegexList; the {…} syntax carries the pattern.
                // Early versions wrote the group after a comma ($regex:1,1$).
                static const QRegularExpression indexed("^\\{regex:(\\d{1,3})(?:[|,]([^|,{}]*))?\\}$", QRegularExpression::CaseInsensitiveOption);
                const auto match = indexed.match(out.mid(start));
                const int index = match.hasMatch() ? match.captured(1).toInt() : 0;
                if (index >= 1 && index <= regexList.size()) {
                    QString pattern;
                    for (const auto ch : regexList[index - 1]) { if (QStringLiteral("{}|\\").contains(ch)) pattern.append('\\'); pattern.append(ch); }
                    out = out.left(start) + "{regex:" + pattern + (match.captured(2).isEmpty() ? QString() : "|" + match.captured(2)) + "}";
                }
            }
            open = !open;
            continue;
        }
        if (c == '{' || c == '}' || (c == '|' && !open)) out.append('\\');
        out.append(c);
    }
    // An unpaired $ leaves an unclosed placeholder, which the parser reports.
    return out;
}
QString renderRequest(const QString& text, const RequestValues& values, QString* problem) {
    QString parse;
    const auto nodes = parseTemplate(text, &parse);
    if (!parse.isEmpty()) { if (problem) *problem = parse; return {}; }
    Renderer renderer;
    renderer.request = &values;
    QString local;
    renderer.problem = problem ? problem : &local;
    return renderer.sequence(nodes);
}
QString renderResponse(const QString& text, const ResponseValues& values) {
    const auto nodes = parseTemplate(text);
    Renderer renderer;
    renderer.response = &values;
    return renderer.sequence(nodes);
}
bool jsonPathSupported(const QString& path) {
    QList<PathStep> steps;
    return path.size() <= 1024 && jsonSteps(path.trimmed(), steps);
}
QString jsonPathValue(const QString& json, const QString& path) {
    QList<PathStep> steps;
    if (json.size() > maximumParsedBytes || !jsonSteps(path.trimmed(), steps)) return {};
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    QJsonValue value = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
    if (document.isNull()) return {};
    for (const auto& step : std::as_const(steps)) {
        if (step.index) {
            if (!value.isArray() || step.position >= value.toArray().size()) return {};
            value = value.toArray().at(step.position);
        } else {
            if (!value.isObject() || !value.toObject().contains(step.name)) return {};
            value = value.toObject().value(step.name);
        }
    }
    return jsonText(value);
}
bool xpathSupported(const QString& path) {
    QList<XPathStep> steps;
    return xpathSteps(path, steps);
}
QString xpathValue(const QString& xml, const QString& path) {
    QList<XPathStep> steps;
    if (!xpathSteps(path, steps)) return {};
    const auto tree = readXml(xml);
    if (!tree.ok) return {};
    QList<int> context{0};
    for (const auto& step : std::as_const(steps)) {
        if (step.kind != XPathStep::Element) {
            // The parents of the attribute or text: every context node, or its descendants after "//".
            QList<int> owners;
            for (const int index : context) { if (step.descendant) descendants(tree, index, owners); else owners.append(index); }
            for (const int owner : std::as_const(owners)) {
                const auto& node = tree.nodes[owner];
                if (step.kind == XPathStep::Text) {
                    for (const auto& [child, text] : node.content) if (child < 0 && !text.trimmed().isEmpty()) return text;
                } else {
                    for (const auto& [name, value] : node.attributes)
                        if (name == step.name || name.mid(name.indexOf(':') + 1) == step.name.mid(step.name.indexOf(':') + 1)) return value;
                }
            }
            return {};
        }
        QList<int> next;
        QSet<int> seen;
        for (const int index : context) {
            QList<int> parents;
            if (step.descendant) descendants(tree, index, parents); else parents.append(index);
            for (const int parent : std::as_const(parents)) {
                QList<int> group;
                for (const int child : tree.nodes[parent].children) if (nameMatches(tree.nodes[child], step.name)) group.append(child);
                for (const auto& predicate : step.predicates) group = filter(tree, group, predicate);
                for (const int child : std::as_const(group)) if (!seen.contains(child)) { seen.insert(child); next.append(child); }
            }
        }
        if (next.isEmpty()) return {};
        // Node indexes follow document order.
        std::sort(next.begin(), next.end());
        context = next;
    }
    return context.isEmpty() ? QString() : tree.value(context.first());
}
}
