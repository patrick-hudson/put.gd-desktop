#pragma once
#include "core/Error.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QWidget>
#include <functional>
class QCheckBox;class QComboBox;class QFormLayout;class QLabel;class QLineEdit;class QPlainTextEdit;class QPushButton;class QSpinBox;class QVBoxLayout;
namespace putgd {
// The header, parameter or argument rows of a custom uploader: a name, a value and a
// Secret switch each. A secret row's value is typed into a masked field that lives in
// this process's memory only; settings() never contains it.
class UploaderRows final : public QWidget {
    Q_OBJECT
public:
    // alwaysSecret: rows have no Secret switch; every value is one (the "secrets" list).
    UploaderRows(const QString& list, const QString& noun, QWidget* parent = nullptr, bool alwaysSecret = false);
    void setRows(const QJsonArray& rows);
    // Rows as the configuration stores them: a secret row has a name only.
    QJsonArray rows() const;
    // Credential-store key → the value typed into a secret row.
    QMap<QString, QString> typed() const;
    void setTyped(const QMap<QString, QString>&);
    void clearTyped();
    // Which secret rows the credential store holds; `known` is false while it is locked.
    void setStored(const QSet<QString>& keys, bool known);
    QJsonArray state() const;
signals:
    void changed();      // something the configuration stores changed
    void typedChanged(); // a secret value was typed or cleared
private:
    struct Row { QWidget* box; QLineEdit* name; QLineEdit* value; QCheckBox* secret; };
    void addRow(const QString& name, const QString& value, bool secret, bool focus = false);
    void refreshRow(const Row&);
    void renumber();
    QString list_, noun_;
    QVBoxLayout* column_ = nullptr;
    QPushButton* add_ = nullptr;
    QList<Row> rows_;
    QSet<QString> stored_;
    bool storedKnown_ = false, loading_ = false, alwaysSecret_ = false;
};

// Settings → Destinations → Custom uploader (HTTP) (#1361): the request, what the
// destination takes, and where the reply holds the link. Settings builds the rows
// with its own row style; this class owns the fields, the .sxcu import and the rule
// that a secret value never enters settings().
class CustomUploaderPanel final : public QWidget {
    Q_OBJECT
public:
    // (form, label, control, hint) → the row widget, in the Settings row style.
    using RowBuilder = std::function<QWidget*(QFormLayout*, const QString&, QWidget*, const QString&)>;
    static QString provider();
    static QJsonObject defaultSettings();
    explicit CustomUploaderPanel(QWidget* parent = nullptr);
    void build(const RowBuilder&);
    void setSettings(const QJsonObject&);
    QJsonObject settings() const;
    QMap<QString, QString> typedSecrets() const;
    void setTypedSecrets(const QMap<QString, QString>&);
    void clearTypedSecrets();
    // From setup.credentialHint: stored credential-store keys, or unknown while locked.
    void setStored(const QStringList& keys, bool known);
    // The names of secret values the request needs that are neither typed nor stored.
    QStringList missingSecrets() const;
    // Reads a .sxcu into the form. Its secret values become typed values, held here until stored.
    Error importBytes(const QByteArray&);
    Error importFile(const QString& path);
    // Opens the import dialog, as the button does.
    void openImport();
    // A refused save: the message beside the field it names.
    void showProblem(const QString& field, const QString& message);
    void setNotice(const QString& text, bool success = false);
    // Fields and which secret values are typed or stored, never a value; for agents.
    QJsonObject state() const;
signals:
    void edited();
    void secretsEdited();
    // A file was imported: its name, for a destination that still has its default one.
    void imported(const QString& name);
private:
    void syncVisibility();
    void finished();
    QList<UploaderRows*> lists() const;
    bool loading_ = false, built_ = false;
    QComboBox *method_ = nullptr, *body_ = nullptr;
    QLineEdit *url_ = nullptr, *fileForm_ = nullptr, *responseUrl_ = nullptr, *thumbnailUrl_ = nullptr, *deletionUrl_ = nullptr, *errorMessage_ = nullptr;
    QCheckBox *insecure_ = nullptr, *dataSecret_ = nullptr, *images_ = nullptr, *text_ = nullptr, *files_ = nullptr;
    QPlainTextEdit* data_ = nullptr;
    QSpinBox *connectTimeout_ = nullptr, *requestTimeout_ = nullptr;
    UploaderRows *headers_ = nullptr, *parameters_ = nullptr, *arguments_ = nullptr, *secrets_ = nullptr;
    QPushButton* import_ = nullptr;
    QLabel *notice_ = nullptr, *problem_ = nullptr;
    QFormLayout *requestForm_ = nullptr, *responseForm_ = nullptr;
    QWidget *fileFormRow_ = nullptr, *argumentsRow_ = nullptr, *dataRow_ = nullptr, *insecureRow_ = nullptr;
    QString dataTyped_;       // the secret request body as typed
    bool dataStored_ = false, storedKnown_ = false;
};
}
