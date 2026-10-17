#pragma once
class QApplication;class QIODevice;
namespace putgd {
// `putgd-ui --edit-image`: one interactive editor session for one private
// staged PNG. It owns no settings, providers, credentials or history.
int runImageEditor(QApplication&,QIODevice& pipes);
}
