#pragma once
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class SqlHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit SqlHighlighter(QTextDocument* parent=nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    QTextCharFormat kw_, lit_, com_, id_;
};
