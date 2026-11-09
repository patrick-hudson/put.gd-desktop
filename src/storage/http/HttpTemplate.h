#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
namespace putgd::http {
// ShareX custom-uploader placeholders (#1361): literal text and {function:parameter|parameter}
// calls, nested, with '\' escaping the next character. Qt Core only.
struct TemplateNode {
    bool call = false;
    QString text;                           // literal text, or the function name in lower case
    QList<QList<TemplateNode>> parameters;  // each parameter is itself a template
};
using Template = QList<TemplateNode>;

// Where a template is used: request values may name the file and the text being sent;
// response values read the reply.
enum class TemplateUse { Request, Response };

// Parses the {…} syntax. On failure returns an empty template and sets `problem` to a
// sentence without the template's text (it may hold a credential).
Template parseTemplate(const QString& text, QString* problem = nullptr);
// Parses and checks function names, parameter counts and the supported JSON path, XPath
// and regular-expression forms. Empty when the template is usable.
QString templateProblem(const QString& text, TemplateUse use);
// The older $function:parameter$ syntax (ShareX 13.7.1 and earlier) rewritten in the
// {…} syntax. `regexList` resolves $regex:N|group$ to the Nth pattern (1-based).
QString migrateOldSyntax(const QString& text, const QStringList& regexList = {});
// True when the text holds an old-syntax placeholder such as $json:url$.
bool usesOldSyntax(const QString& text);
// Whether a request template reads {input}, the text being uploaded.
bool readsInput(const QString& text);

struct RequestValues {
    QString fileName;
    QString input;          // the text being uploaded; only when hasInput
    bool hasInput = false;
    enum class Encode { None, Url, Json, Xml } encode = Encode::None;  // applied to file name and input only
};
// Renders a request value. `problem` is set when it needs {input} and none is available.
QString renderRequest(const QString& text, const RequestValues&, QString* problem = nullptr);

struct ResponseValues {
    QString body;                                       // decoded reply text, bounded by the caller
    QList<QPair<QByteArray, QByteArray>> headers;
    QString responseUrl;                                // the final address after redirects
    QString fileName;
};
// Renders a response value; a placeholder that finds nothing renders as empty text.
QString renderResponse(const QString& text, const ResponseValues&);

// The supported JSON path subset: an optional leading "$", then .name, ['name'], ["name"]
// and [index] steps. Returns false for anything else (wildcards, "..", filters, slices).
bool jsonPathSupported(const QString& path);
QString jsonPathValue(const QString& json, const QString& path);
// The supported XPath subset: /a/b, //a, *, a[2], a[last()], a[@k='v'], and a final
// @attribute or text(). Namespace prefixes are ignored. Returns false for anything else.
bool xpathSupported(const QString& path);
QString xpathValue(const QString& xml, const QString& path);
}
