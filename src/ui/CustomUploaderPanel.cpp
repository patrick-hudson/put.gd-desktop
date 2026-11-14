#include "CustomUploaderPanel.h"
#include "FileDialog.h"
#include "PreferenceRows.h"
#include "Ui.h"
#include "storage/http/HttpCommon.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
namespace putgd {
using Ui::space;
using Ui::Space;
namespace {
QString titled(const QString& word) { return word.left(1).toUpper() + word.mid(1); }
const QString storedPlaceholder = QString::fromUtf8("•••••••• stored");
// Leaving a multi-line field is its finished edit.
class FocusOut final : public QObject {
public:
    FocusOut(QWidget* watched, std::function<void()> done) : QObject(watched), done_(std::move(done)) { watched->installEventFilter(this); }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::FocusOut) done_();
        return false;
    }
private:
    std::function<void()> done_;
};
}

UploaderRows::UploaderRows(const QString& list, const QString& noun, QWidget* parent, bool alwaysSecret) : QWidget(parent), list_(list), noun_(noun), alwaysSecret_(alwaysSecret) {
    setObjectName("uploader" + titled(list));
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 0, 0);
    column_->setSpacing(space(Space::s));
    add_ = new QPushButton("Add " + noun, this);
    add_->setObjectName("uploaderAdd" + titled(list == "secrets" ? QStringLiteral("secret") : noun));
    Ui::role(add_, "textAction");
    add_->setProperty("uiFlush", true);
    column_->addWidget(add_, 0, Qt::AlignLeft);
    connect(add_, &QPushButton::clicked, this, [this] { addRow({}, {}, true, true); });
}
void UploaderRows::addRow(const QString& name, const QString& value, bool secret, bool focus) {
    if (rows_.size() >= http::maximumRows) return;
    Row row;
    row.box = new QWidget(this);
    auto* line = new QHBoxLayout(row.box);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(space(Space::s));
    row.name = new QLineEdit(name, row.box);
    row.name->setPlaceholderText("Name");
    row.name->setMaxLength(http::maximumNameChars);
    row.value = new QLineEdit(value, row.box);
    row.value->setMaxLength(http::maximumValueChars);
    for (auto* field : {row.name, row.value}) field->setMinimumHeight(Ui::scaled(Ui::Metric::control));
    Ui::setMono(row.name);
    row.secret = new QCheckBox("Secret", row.box);
    row.secret->setChecked(secret || alwaysSecret_);
    row.secret->setVisible(!alwaysSecret_);
    row.secret->setToolTip("A secret value is kept in the credential store, never in the configuration file. Type a new value to replace a stored one.");
    auto* reveal = row.value->addAction(Ui::icon("eye"), QLineEdit::TrailingPosition);
    reveal->setCheckable(true);
    reveal->setObjectName("reveal");
    auto* remove = new QToolButton(row.box);
    remove->setIcon(Ui::icon("remove"));
    remove->setProperty("uiFlush", true);
    remove->setFixedSize(Ui::scaled(Ui::Metric::control), Ui::scaled(Ui::Metric::control));
    line->addWidget(row.name, 2);
    line->addWidget(row.value, 3);
    line->addWidget(row.secret);
    line->addWidget(remove);
    column_->insertWidget(int(rows_.size()), row.box);
    rows_.append(row);
    renumber();
    refreshRow(row);
    auto* box = row.box;
    const auto find = [this, box]() -> const Row* { for (const auto& r : std::as_const(rows_)) if (r.box == box) return &r; return nullptr; };
    connect(reveal, &QAction::toggled, this, [this, find] { if (const auto* r = find()) refreshRow(*r); });
    connect(row.name, &QLineEdit::editingFinished, this, [this, find] {
        if (const auto* r = find()) { refreshRow(*r); renumber(); }
        if (!loading_) { emit changed(); emit typedChanged(); }
    });
    connect(row.value, &QLineEdit::textEdited, this, [this, find] { if (const auto* r = find(); r && r->secret->isChecked() && !loading_) emit typedChanged(); });
    connect(row.value, &QLineEdit::editingFinished, this, [this, find] { if (const auto* r = find(); r && !r->secret->isChecked() && !loading_) emit changed(); });
    connect(row.secret, &QCheckBox::toggled, this, [this, find] {
        if (const auto* r = find()) refreshRow(*r);
        if (!loading_) { emit changed(); emit typedChanged(); }
    });
    connect(remove, &QToolButton::clicked, this, [this, box] {
        for (qsizetype i = 0; i < rows_.size(); ++i)
            if (rows_[i].box == box) {
                rows_[i].value->clear();
                rows_.removeAt(i);
                break;
            }
        box->deleteLater();
        renumber();
        emit changed();
        emit typedChanged();
    });
    if (focus) row.name->setFocus(Qt::OtherFocusReason);
}
void UploaderRows::refreshRow(const Row& row) {
    const bool secret = row.secret->isChecked();
    auto* reveal = row.value->findChild<QAction*>("reveal");
    if (reveal) reveal->setVisible(secret);
    row.value->setEchoMode(secret && !(reveal && reveal->isChecked()) ? QLineEdit::Password : QLineEdit::Normal);
    const auto key = http::secretKey(list_, row.name->text().trimmed());
    row.value->setPlaceholderText(!secret ? QStringLiteral("Value") : stored_.contains(key) ? storedPlaceholder : storedKnown_ ? QStringLiteral("Not stored yet") : QStringLiteral("Secret value"));
}
void UploaderRows::renumber() {
    const auto noun = list_ == "secrets" ? QStringLiteral("Secret") : titled(noun_);
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        const auto index = QString::number(i);
        const auto name = row.name->text().trimmed();
        row.name->setObjectName("uploader" + noun + "Name" + index);
        row.value->setObjectName("uploader" + noun + "Value" + index);
        row.secret->setObjectName("uploader" + noun + "Secret" + index);
        row.name->setAccessibleName(noun + " name");
        row.value->setAccessibleName(name.isEmpty() ? noun + " value" : "Value of " + noun_ + " " + name);
        row.secret->setAccessibleName(name.isEmpty() ? QStringLiteral("Secret") : "Keep " + noun_ + " " + name + " secret");
        if (auto* remove = row.box->findChild<QToolButton*>()) {
            remove->setObjectName("uploader" + noun + "Remove" + index);
            remove->setAccessibleName(name.isEmpty() ? "Remove " + noun_ : "Remove " + noun_ + " " + name);
            remove->setToolTip(remove->accessibleName());
        }
    }
    add_->setVisible(rows_.size() < http::maximumRows);
}
void UploaderRows::setRows(const QJsonArray& rows) {
    loading_ = true;
    for (const auto& row : rows_) { row.value->clear(); row.box->deleteLater(); }
    rows_.clear();
    for (const auto& value : rows) {
        const auto row = value.toObject();
        addRow(row["name"].toString(), row["secret"].toBool() ? QString() : row["value"].toString(), row["secret"].toBool());
    }
    renumber();
    loading_ = false;
}
QJsonArray UploaderRows::rows() const {
    QJsonArray out;
    for (const auto& row : rows_) {
        const auto name = row.name->text().trimmed();
        if (name.isEmpty()) continue;
        if (row.secret->isChecked()) out.append(QJsonObject{{"name", name}, {"secret", true}});
        else out.append(QJsonObject{{"name", name}, {"secret", false}, {"value", row.value->text()}});
    }
    return out;
}
QMap<QString, QString> UploaderRows::typed() const {
    QMap<QString, QString> out;
    for (const auto& row : rows_) {
        const auto name = row.name->text().trimmed();
        if (!name.isEmpty() && row.secret->isChecked() && !row.value->text().isEmpty()) out.insert(http::secretKey(list_, name), row.value->text());
    }
    return out;
}
void UploaderRows::setTyped(const QMap<QString, QString>& values) {
    for (const auto& row : rows_)
        if (row.secret->isChecked()) row.value->setText(values.value(http::secretKey(list_, row.name->text().trimmed())));
}
void UploaderRows::clearTyped() {
    for (const auto& row : rows_) if (row.secret->isChecked()) row.value->clear();
}
void UploaderRows::setStored(const QSet<QString>& keys, bool known) {
    stored_ = keys;
    storedKnown_ = known;
    for (const auto& row : rows_) refreshRow(row);
}
QJsonArray UploaderRows::state() const {
    QJsonArray out;
    for (const auto& row : rows_) {
        const auto name = row.name->text().trimmed();
        const bool secret = row.secret->isChecked();
        QJsonObject entry{{"name", name}, {"secret", secret}};
        if (secret) { entry["typed"] = !row.value->text().isEmpty(); entry["stored"] = stored_.contains(http::secretKey(list_, name)); }
        else entry["value"] = row.value->text();
        out.append(entry);
    }
    return out;
}
QLineEdit* UploaderRows::missingValue() const {
    for (const auto& row : rows_)
        if (row.secret->isChecked() && row.value->text().isEmpty() && !stored_.contains(http::secretKey(list_, row.name->text().trimmed()))) return row.value;
    return nullptr;
}

QString CustomUploaderPanel::provider() { return QString::fromLatin1(http::providerId); }
QJsonObject CustomUploaderPanel::defaultSettings() { return http::defaults(); }
CustomUploaderPanel::CustomUploaderPanel(QWidget* parent) : QWidget(parent) { setObjectName("customUploaderEditor"); }
QList<UploaderRows*> CustomUploaderPanel::lists() const { return {headers_, parameters_, arguments_, secrets_}; }
void CustomUploaderPanel::finished() {
    syncVisibility();
    if (!loading_) emit edited();
}
void CustomUploaderPanel::build(const RowBuilder& row) {
    if (built_) return;
    built_ = true;
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(space(Space::xxl));
    requestForm_ = PreferenceRows::card(page, "Request", "The request put.gd sends for each upload. Fill it in, or import a ShareX custom uploader file (.sxcu).");
    requestForm_->parentWidget()->setObjectName("uploaderRequestSection");
    import_ = new QPushButton(QString::fromUtf8("Import .sxcu…"), this);
    import_->setObjectName("importSxcu");
    import_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    PreferenceRows::action(requestForm_, "ShareX custom uploader", "Fills this form from a .sxcu file or pasted text. Its token stays in this window until you store it.", import_);
    notice_ = new QLabel(this);
    notice_->setObjectName("uploaderNotice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setProperty("uiStatus", true);
    Ui::role(notice_, "secondary");
    requestForm_->addRow(notice_);
    notice_->hide();
    connect(import_, &QPushButton::clicked, this, [this] { openImport(); });

    const auto line = [this](const char* name, const QString& placeholder = {}) {
        auto* field = new QLineEdit(this);
        field->setObjectName(name);
        field->setPlaceholderText(placeholder);
        connect(field, &QLineEdit::editingFinished, this, [this] { finished(); });
        return field;
    };
    method_ = Ui::choiceBox(this);
    method_->setObjectName("uploaderMethod");
    for (const auto* verb : {"POST", "PUT", "PATCH", "GET"}) method_->addItem(verb, verb);
    row(requestForm_, "Method", method_, {});
    url_ = line("uploaderUrl", "https://example.com/api/upload");
    Ui::setMono(url_);
    row(requestForm_, "Request URL", url_, "Where uploads are sent. {filename} may fill part of the path.");
    insecure_ = new QCheckBox("Allow plain HTTP for this destination", this);
    insecure_->setObjectName("uploaderAllowHttp");
    insecureRow_ = row(requestForm_, "Plain HTTP", insecure_, "Plain HTTP sends files and their credentials unencrypted. Use it only on a network you trust.");
    body_ = Ui::choiceBox(this);
    body_->setObjectName("uploaderBody");
    for (const auto& [label, value] : QList<QPair<QString, QString>>{{"Form data (multipart)", "multipart"}, {"Binary (the file itself)", "binary"}, {"Form URL encoded", "form"}, {"JSON", "json"}, {"XML", "xml"}, {"No body", "none"}})
        body_->addItem(label, value);
    row(requestForm_, "Body", body_, {});
    fileForm_ = line("uploaderFileFormName", "file");
    Ui::setMono(fileForm_);
    fileFormRow_ = row(requestForm_, "File form name", fileForm_, "The form field that carries the file. Leave empty to send form fields only.");
    headers_ = new UploaderRows("headers", "header", this);
    row(requestForm_, "Headers", headers_, "Values are secret unless you clear Secret: they are kept in the credential store, never in the configuration file.");
    parameters_ = new UploaderRows("parameters", "parameter", this);
    row(requestForm_, "URL parameters", parameters_, {});
    arguments_ = new UploaderRows("arguments", "field", this);
    argumentsRow_ = row(requestForm_, "Form fields", arguments_, "{filename} is the file name; {input} is the text being uploaded.");
    secrets_ = new UploaderRows("secrets", "secret value", this, true);
    row(requestForm_, "Other secret values", secrets_, "A stored value used inside an address: write {secret:secrets/name} in the request URL or the deletion URL.");
    auto* dataBox = new QWidget(this);
    dataBox->setObjectName("uploaderDataBox");
    auto* dataColumn = new QVBoxLayout(dataBox);
    dataColumn->setContentsMargins(0, 0, 0, 0);
    dataColumn->setSpacing(space(Space::s));
    data_ = new QPlainTextEdit(dataBox);
    data_->setObjectName("uploaderData");
    data_->setAccessibleName("Body text");
    data_->setTabChangesFocus(true);
    data_->setFixedHeight(Ui::scaled(96));
    Ui::setMono(data_);
    dataSecret_ = new QCheckBox("Secret", dataBox);
    dataSecret_->setObjectName("uploaderDataSecret");
    dataSecret_->setAccessibleName("Keep the body text secret");
    dataColumn->addWidget(data_);
    dataColumn->addWidget(dataSecret_);
    dataRow_ = row(requestForm_, "Body text", dataBox, "The JSON or XML sent as the body. {input} is the text being uploaded and {filename} its name.");
    new FocusOut(data_, [this] { if (!loading_ && !dataSecret_->isChecked()) finished(); });
    connect(data_, &QPlainTextEdit::textChanged, this, [this] { if (!loading_ && dataSecret_->isChecked()) emit secretsEdited(); });

    auto* accepts = new QWidget(this);
    accepts->setObjectName("uploaderAccepts");
    auto* acceptsLine = new QHBoxLayout(accepts);
    acceptsLine->setContentsMargins(0, 0, 0, 0);
    acceptsLine->setSpacing(space(Space::l));
    images_ = new QCheckBox("Images", accepts);
    text_ = new QCheckBox("Text", accepts);
    files_ = new QCheckBox("Files", accepts);
    images_->setObjectName("uploaderAcceptsImages");
    text_->setObjectName("uploaderAcceptsText");
    files_->setObjectName("uploaderAcceptsFiles");
    for (auto* check : {images_, text_, files_}) { acceptsLine->addWidget(check); connect(check, &QCheckBox::clicked, this, [this] { finished(); }); }
    acceptsLine->addStretch(1);
    row(requestForm_, "Uploads", accepts, "What this destination takes. Files covers everything, recordings included.");
    const auto seconds = [this](const char* name, int maximum) {
        auto* spin = new QSpinBox(this);
        spin->setObjectName(name);
        spin->setRange(1, maximum);
        spin->setSuffix(" seconds");
        connect(spin, &QSpinBox::editingFinished, this, [this] { finished(); });
        return spin;
    };
    connectTimeout_ = seconds("uploaderConnectTimeout", 120);
    requestTimeout_ = seconds("uploaderRequestTimeout", 3600);
    row(requestForm_, "Connect timeout", connectTimeout_, {});
    row(requestForm_, "Request timeout", requestTimeout_, "How long one upload may take, the server's reply included.");

    responseForm_ = PreferenceRows::card(page, "Response",
        "Where the server's reply holds the link. Placeholders: {json:path}, {xml:xpath}, {regex:pattern|group}, {header:name}, {response} and {responseurl}. "
        "The server names each upload, so put.gd cannot look one up afterwards: if a reply is lost, Retry sends the file again and may leave a second copy on the server.");
    responseForm_->parentWidget()->setObjectName("uploaderResponseSection");
    responseUrl_ = line("uploaderResponseUrl", "{json:files[0].url}");
    row(responseForm_, "URL", responseUrl_, "The link to the upload. Leave empty when the whole reply is the link.");
    thumbnailUrl_ = line("uploaderThumbnailUrl");
    row(responseForm_, "Thumbnail URL", thumbnailUrl_, "Optional.");
    deletionUrl_ = line("uploaderDeletionUrl");
    row(responseForm_, "Deletion URL", deletionUrl_, "Optional. Without one, an upload to this destination can't be deleted from History.");
    errorMessage_ = line("uploaderErrorMessage");
    row(responseForm_, "Error message", errorMessage_, "Optional. Shown when the server refuses an upload.");
    for (auto* field : {responseUrl_, thumbnailUrl_, deletionUrl_, errorMessage_}) Ui::setMono(field);

    connect(method_, &QComboBox::activated, this, [this] {
        // A GET carries no body.
        if (method_->currentData() == "GET") body_->setCurrentIndex(body_->findData("none"));
        finished();
    });
    connect(body_, &QComboBox::activated, this, [this] {
        if (body_->currentData() != "none" && method_->currentData() == "GET") method_->setCurrentIndex(method_->findData("POST"));
        finished();
    });
    connect(insecure_, &QCheckBox::clicked, this, [this] { finished(); });
    connect(url_, &QLineEdit::textChanged, this, [this] { syncVisibility(); });
    connect(dataSecret_, &QCheckBox::toggled, this, [this](bool secret) {
        if (loading_) return;
        // The text moves with the switch: into the credential store's typed value, or back into settings.
        if (secret) dataTyped_.clear();
        else if (data_->toPlainText().isEmpty() && !dataTyped_.isEmpty()) { const QSignalBlocker block(data_); data_->setPlainText(dataTyped_); dataTyped_.clear(); }
        syncVisibility();
        emit edited();
        emit secretsEdited();
    });
    for (auto* list : lists()) {
        connect(list, &UploaderRows::changed, this, [this] { finished(); });
        connect(list, &UploaderRows::typedChanged, this, [this] { if (!loading_) emit secretsEdited(); });
    }
    setSettings(http::defaults());
}
void CustomUploaderPanel::syncVisibility() {
    if (!built_) return;
    const auto body = body_->currentData().toString();
    requestForm_->setRowVisible(fileFormRow_, body == "multipart");
    requestForm_->setRowVisible(argumentsRow_, body == "multipart" || body == "form");
    requestForm_->setRowVisible(dataRow_, body == "json" || body == "xml");
    const QUrl url(url_->text().trimmed());
    requestForm_->setRowVisible(insecureRow_, insecure_->isChecked() || (url.scheme() == "http" && !http::loopbackHost(url.host())));
    // A body kept secret cannot be masked like a one-line field; say so to whatever reads this form.
    data_->setProperty("putgdSensitive", dataSecret_->isChecked());
    data_->setPlaceholderText(!dataSecret_->isChecked() ? QString() : !dataTyped_.isEmpty() ? QString::fromUtf8("•••••••• imported, not stored yet")
                                                        : dataStored_ ? storedPlaceholder : QStringLiteral("Type the body text, then store credentials"));
}
void CustomUploaderPanel::setSettings(const QJsonObject& s) {
    if (!built_) return;
    loading_ = true;
    method_->setCurrentIndex(qMax(0, method_->findData(s["requestMethod"].toString("POST"))));
    url_->setText(s["requestUrl"].toString());
    insecure_->setChecked(s["allowInsecureHttp"].toBool());
    body_->setCurrentIndex(qMax(0, body_->findData(s["body"].toString("multipart"))));
    fileForm_->setText(s["fileFormName"].toString());
    headers_->setRows(s["headers"].toArray());
    parameters_->setRows(s["parameters"].toArray());
    arguments_->setRows(s["arguments"].toArray());
    secrets_->setRows(s["secrets"].toArray());
    dataSecret_->setChecked(s["dataSecret"].toBool());
    data_->setPlainText(s["dataSecret"].toBool() ? QString() : s["data"].toString());
    dataTyped_.clear();
    const auto accepts = s["accepts"].toArray();
    images_->setChecked(accepts.contains("image"));
    text_->setChecked(accepts.contains("text"));
    files_->setChecked(accepts.contains("file"));
    // The form shows whole seconds. A finer saved value is kept until its field is changed.
    loadedConnectMs_ = s["connectTimeoutMs"].toInt(10000);
    loadedRequestMs_ = s["requestTimeoutMs"].toInt(600000);
    connectTimeout_->setValue(qMax(1, loadedConnectMs_ / 1000));
    requestTimeout_->setValue(qMax(1, loadedRequestMs_ / 1000));
    responseUrl_->setText(s["responseUrl"].toString());
    thumbnailUrl_->setText(s["thumbnailUrl"].toString());
    deletionUrl_->setText(s["deletionUrl"].toString());
    errorMessage_->setText(s["errorMessage"].toString());
    loading_ = false;
    showProblem({}, {});
    syncVisibility();
}
QJsonObject CustomUploaderPanel::settings() const {
    auto s = http::defaults();
    if (!built_) return s;
    s["requestMethod"] = method_->currentData().toString();
    s["requestUrl"] = url_->text().trimmed();
    s["allowInsecureHttp"] = insecure_->isChecked();
    s["body"] = body_->currentData().toString();
    s["fileFormName"] = fileForm_->text();
    s["headers"] = headers_->rows();
    s["parameters"] = parameters_->rows();
    s["arguments"] = arguments_->rows();
    s["secrets"] = secrets_->rows();
    s["dataSecret"] = dataSecret_->isChecked();
    s["data"] = dataSecret_->isChecked() ? QString() : data_->toPlainText();
    QJsonArray accepts;
    if (images_->isChecked()) accepts.append("image");
    if (text_->isChecked()) accepts.append("text");
    if (files_->isChecked()) accepts.append("file");
    s["accepts"] = accepts;
    s["connectTimeoutMs"] = connectTimeout_->value() == qMax(1, loadedConnectMs_ / 1000) ? loadedConnectMs_ : connectTimeout_->value() * 1000;
    s["requestTimeoutMs"] = requestTimeout_->value() == qMax(1, loadedRequestMs_ / 1000) ? loadedRequestMs_ : requestTimeout_->value() * 1000;
    s["responseUrl"] = responseUrl_->text().trimmed();
    s["thumbnailUrl"] = thumbnailUrl_->text().trimmed();
    s["deletionUrl"] = deletionUrl_->text().trimmed();
    s["errorMessage"] = errorMessage_->text().trimmed();
    return s;
}
QMap<QString, QString> CustomUploaderPanel::typedSecrets() const {
    QMap<QString, QString> out;
    if (!built_) return out;
    for (auto* list : lists()) out.insert(list->typed());
    if (dataSecret_->isChecked()) {
        const auto text = data_->toPlainText().isEmpty() ? dataTyped_ : data_->toPlainText();
        if (!text.isEmpty()) out.insert(http::dataSecretKey(), text);
    }
    return out;
}
void CustomUploaderPanel::setTypedSecrets(const QMap<QString, QString>& values) {
    if (!built_) return;
    loading_ = true;
    for (auto* list : lists()) list->setTyped(values);
    if (dataSecret_->isChecked()) { data_->clear(); dataTyped_ = values.value(http::dataSecretKey()); }
    loading_ = false;
    syncVisibility();
}
void CustomUploaderPanel::clearTypedSecrets() {
    if (!built_) return;
    loading_ = true;
    for (auto* list : lists()) list->clearTyped();
    if (dataSecret_->isChecked()) data_->clear();
    dataTyped_.fill(QChar('\0'));
    dataTyped_.clear();
    loading_ = false;
    syncVisibility();
}
void CustomUploaderPanel::setStored(const QStringList& keys, bool known) {
    if (!built_) return;
    const QSet<QString> set(keys.cbegin(), keys.cend());
    storedKnown_ = known;
    dataStored_ = set.contains(http::dataSecretKey());
    for (auto* list : lists()) list->setStored(set, known);
    syncVisibility();
}
QWidget* CustomUploaderPanel::credentialField() const {
    if (!built_) return nullptr;
    for (auto* list : lists()) if (auto* field = list->missingValue(); field && field->isVisibleTo(this)) return field;
    if (dataSecret_->isChecked() && !dataStored_ && data_->isVisibleTo(this) && data_->toPlainText().isEmpty()) return data_;
    return nullptr;
}
QStringList CustomUploaderPanel::missingSecrets() const {
    QStringList out;
    const auto typed = typedSecrets();
    QSet<QString> stored;
    for (auto* list : lists())
        for (const auto& value : list->state()) if (value.toObject()["stored"].toBool()) stored.insert(http::secretKey(list->objectName().mid(8).toLower(), value.toObject()["name"].toString()));
    if (dataStored_) stored.insert(http::dataSecretKey());
    for (const auto& key : http::secretKeys(settings())) if (!typed.contains(key) && !stored.contains(key)) out.append(http::secretLabel(key));
    return out;
}
Error CustomUploaderPanel::importBytes(const QByteArray& bytes) {
    auto result = http::importSxcu(bytes);
    if (result.error) { setNotice({}); return result.error; }
    // This destination's own switch stays on: a person who allowed plain HTTP for it has
    // already made that choice, and the imported address is shown beside it.
    const bool plainHttpAllowed = insecure_->isChecked();
    setSettings(result.settings);
    if (plainHttpAllowed) { insecure_->setChecked(true); syncVisibility(); }
    setTypedSecrets(result.secrets);
    QStringList held;
    for (auto it = result.secrets.cbegin(); it != result.secrets.cend(); ++it) held.append(http::secretLabel(it.key()));
    QString text = "Imported" + (result.name.isEmpty() ? QString() : " " + result.name) + ".";
    if (!held.isEmpty())
        text += QString(" %1 secret value%2 held in this window until you store %3: %4.").arg(held.size()).arg(held.size() == 1 ? " is" : "s are", held.size() == 1 ? "it" : "them", held.join(", "));
    for (const auto& note : std::as_const(result.notes))
        text += " " + (plainHttpAllowed && note == http::plainHttpNote()
            ? QStringLiteral("This destination sends files and credentials over plain HTTP, as allowed for it.") : note);
    setNotice(text, true);
    emit edited();
    emit secretsEdited();
    emit imported(result.name);
    return {};
}
Error CustomUploaderPanel::importFile(const QString& path) {
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || info.size() > http::maximumSxcuBytes || !file.open(QIODevice::ReadOnly))
        return {"sxcu_unreadable", "The .sxcu file could not be read, or is too large to be one.", {}};
    auto bytes = file.read(http::maximumSxcuBytes + 1);
    const auto error = importBytes(bytes);
    bytes.fill('\0');
    return error;
}
void CustomUploaderPanel::openImport() {
    if (findChild<QDialog*>("importSxcuDialog")) return;
    auto* dialog = new QDialog(this);
    dialog->setObjectName("importSxcuDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Import a ShareX custom uploader");
    dialog->setModal(true);
    dialog->setMinimumWidth(Ui::scaled(520));
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(space(Space::xl), space(Space::xl), space(Space::xl), space(Space::l));
    root->setSpacing(space(Space::m));
    auto* heading = Ui::label("Import a ShareX custom uploader", "sectionTitle", dialog);
    root->addWidget(heading);
    auto* intro = Ui::label("Choose the .sxcu file your service gave you, or paste its contents. Tokens in it stay in this window until you store them; they are never written to the configuration file.", "muted", dialog);
    intro->setWordWrap(true);
    root->addWidget(intro);
    auto* text = new QPlainTextEdit(dialog);
    text->setObjectName("sxcuText");
    text->setAccessibleName("Contents of the .sxcu file");
    text->setPlaceholderText("Paste the contents of a .sxcu file here");
    text->setTabChangesFocus(true);
    text->setMinimumHeight(Ui::scaled(160));
    Ui::setMono(text);
    root->addWidget(text, 1);
    auto* problem = new QLabel(dialog);
    problem->setObjectName("sxcuProblem");
    problem->setWordWrap(true);
    problem->setTextFormat(Qt::PlainText);
    Ui::role(problem, "error");
    problem->hide();
    root->addWidget(problem);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(space(Space::s));
    auto* choose = new QPushButton(QString::fromUtf8("Choose file…"), dialog);
    choose->setObjectName("chooseSxcuFile");
    auto* cancel = new QPushButton("Cancel", dialog);
    cancel->setObjectName("cancelSxcuImport");
    auto* accept = new QPushButton("Import", dialog);
    accept->setObjectName("acceptSxcuImport");
    Ui::role(accept, "primary");
    accept->setDefault(true);
    accept->setEnabled(false);
    buttons->addWidget(choose);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(accept);
    root->addLayout(buttons);
    const auto fail = [problem](const Error& error) { Ui::feedback(problem, error.message, true); problem->show(); };
    connect(text, &QPlainTextEdit::textChanged, dialog, [text, accept] { accept->setEnabled(!text->toPlainText().trimmed().isEmpty()); });
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(accept, &QPushButton::clicked, dialog, [this, dialog, text, fail] {
        if (const auto error = importBytes(text->toPlainText().toUtf8())) { fail(error); return; }
        text->clear();
        dialog->accept();
    });
    auto* chooser = new FileDialog(dialog);
    connect(choose, &QPushButton::clicked, dialog, [this, dialog, chooser, fail] {
        chooser->open({FileDialog::Mode::Open, "Choose a ShareX custom uploader file", {}, {}, {{"ShareX custom uploader", {"*.sxcu", "*.json"}}, {"All files", {"*"}}}},
                      [this, dialog, fail](const FileDialog::Result& result) {
                          if (result.failed()) { fail({"file_dialog_failed", result.error, {}}); return; }
                          if (!result.chosen()) return;
                          // The file is read here and never shown: its token goes straight to the masked fields.
                          if (const auto error = importFile(result.path)) { fail(error); return; }
                          dialog->accept();
                      });
    });
    dialog->open();
    text->setFocus(Qt::OtherFocusReason);
}
void CustomUploaderPanel::showProblem(const QString& field, const QString& message) {
    if (!built_) return;
    const QHash<QString, QWidget*> fields{{"requestUrl", url_}, {"requestMethod", method_}, {"body", body_}, {"fileFormName", fileForm_}, {"headers", headers_}, {"parameters", parameters_},
                                          {"arguments", arguments_}, {"secrets", secrets_}, {"data", findChild<QWidget*>("uploaderDataBox")}, {"accepts", findChild<QWidget*>("uploaderAccepts")},
                                          {"responseUrl", responseUrl_}, {"thumbnailUrl", thumbnailUrl_}, {"deletionUrl", deletionUrl_}, {"errorMessage", errorMessage_},
                                          {"requestTimeoutMs", requestTimeout_}};
    auto* target = fields.value(field, url_);
    for (auto* control : fields) {
        auto* label = control ? findChild<QLabel*>(control->objectName() + "Error") : nullptr;
        if (!label) continue;
        const bool here = control == target && !message.isEmpty();
        label->setText(here ? message : QString());
        label->setVisible(here);
        control->setProperty("invalid", here);
    }
    // An http:// address can only be fixed with the switch beside it.
    if (field == "requestUrl" && !message.isEmpty()) syncVisibility();
}
void CustomUploaderPanel::setNotice(const QString& text, bool success) {
    if (!notice_) return;
    if (success) Ui::success(notice_, text);
    else Ui::feedback(notice_, text);
    notice_->setVisible(!text.isEmpty());
}
QJsonObject CustomUploaderPanel::state() const {
    if (!built_) return {};
    auto s = settings();
    QJsonObject out{{"requestMethod", s["requestMethod"]}, {"requestUrl", s["requestUrl"]}, {"allowInsecureHttp", s["allowInsecureHttp"]}, {"body", s["body"]},
                    {"fileFormName", s["fileFormName"]}, {"accepts", s["accepts"]}, {"responseUrl", s["responseUrl"]}, {"thumbnailUrl", s["thumbnailUrl"]},
                    {"deletionUrl", s["deletionUrl"]}, {"errorMessage", s["errorMessage"]}, {"headers", headers_->state()}, {"parameters", parameters_->state()},
                    {"arguments", arguments_->state()}, {"secrets", secrets_->state()}, {"dataSecret", s["dataSecret"]}, {"notice", notice_->isVisible() ? notice_->text() : QString()},
                    {"importOpen", findChild<QDialog*>("importSxcuDialog") != nullptr}};
    QJsonArray typed, missing;
    const auto values = typedSecrets();
    for (auto it = values.cbegin(); it != values.cend(); ++it) typed.append(http::secretLabel(it.key()));
    for (const auto& label : missingSecrets()) missing.append(label);
    out["typedSecrets"] = typed;
    out["missingSecrets"] = missing;
    return out;
}
}
