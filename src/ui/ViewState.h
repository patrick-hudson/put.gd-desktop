#pragma once
#include "core/Error.h"
namespace putgd {
constexpr qsizetype maximumUiStateBytes=8*1024*1024;
Error validateUiState(const QJsonObject&);
}
