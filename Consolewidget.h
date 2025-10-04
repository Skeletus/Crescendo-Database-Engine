#pragma once
#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QScrollBar>

class ConsoleWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConsoleWidget(QWidget* parent=nullptr)
        : QWidget(parent)
    {
        auto* lay = new QVBoxLayout(this);
        history_ = new QTextEdit(this);
        history_->setReadOnly(true);
        input_ = new QLineEdit(this);
        lay->addWidget(history_);
        lay->addWidget(input_);
        connect(input_, &QLineEdit::returnPressed, this, [this](){
            const QString cmd = input_->text().trimmed();
            if (cmd.isEmpty()) return;
            emit executeRequested(cmd);
            input_->clear();
        });
    }

    void appendLine(const QString& s){
        history_->append(s.toHtmlEscaped());
        history_->verticalScrollBar()->setValue(history_->verticalScrollBar()->maximum());
    }

signals:
    void executeRequested(const QString& sqlLine);

private:
    QTextEdit* history_ = nullptr;
    QLineEdit* input_ = nullptr;
};
