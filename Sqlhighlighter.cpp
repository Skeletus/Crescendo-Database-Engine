#include "SqlHighlighter.h"
#include <QRegularExpression>
#include <QColor>
#include <QTextCharFormat>

SqlHighlighter::SqlHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    kw_.setForeground(Qt::darkBlue);
    kw_.setFontWeight(QFont::Bold);

    lit_.setForeground(Qt::darkGreen);
    com_.setForeground(Qt::gray);
    id_.setForeground(Qt::darkMagenta);
}

void SqlHighlighter::highlightBlock(const QString& text) {
    // Palabras clave básicas
    static const QStringList kws = {
        "SELECT","FROM","WHERE","AND","OR","CREATE","TABLE","DATABASE","INDEX","ON",
        "INSERT","INTO","VALUES","USE","CLOSE","SHOW","DELETE","UPDATE","SET","TABLES"
    };
    for (const auto& k : kws) {
        QRegularExpression re("\\b" + k + "\\b", QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), kw_);
        }
    }

    // Literales entre comillas simples
    {
        QRegularExpression re("'[^']*'");
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), lit_);
        }
    }

    // Comentarios estilo -- hasta fin de línea
    {
        int pos = text.indexOf("--");
        if (pos >= 0) setFormat(pos, text.size()-pos, com_);
    }
}
