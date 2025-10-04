#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QFileSystemModel>
#include <QTreeView>
#include <QPlainTextEdit>
#include <QTableView>
#include <QToolBar>
#include <QDockWidget>
#include <QProcess>
#include <QSplitter>
#include <QTextEdit>
#include <QPointer>

#include "ConsoleWidget.h"
#include "ResultTableModel.h"
#include "SqlHighlighter.h"

// MiniDB SQL
#include "MiniDBSQL.h"        // sqlmini::SQLExecutor y utilidades
#include "GenericFixedTable.h"// gft::GenericFixedTable, gft::Value
#include "DiskBTreeMulti.h"
#include "MiniDatabase.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent=nullptr);
    ~MainWindow() override = default;

private slots:
    void onCreateDatabase();
    void onCreateTable();
    void onExecuteSQL();
    void onOpenTerminal();

    void onTreeActivated(const QModelIndex& idx);

private:
    // UI
    QToolBar* tb_ = nullptr;

    // Dock: File Tree
    QDockWidget* dockFiles_ = nullptr;
    QFileSystemModel* fsModel_ = nullptr;
    QTreeView* tree_ = nullptr;

    // Central: SQL editor
    QWidget* central_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    SqlHighlighter* highlighter_ = nullptr;

    // Bottom: Console + Results
    QDockWidget* dockConsole_ = nullptr;
    ConsoleWidget* console_ = nullptr;

    QDockWidget* dockResults_ = nullptr;
    QTableView* resultsView_ = nullptr;
    ResultTableModel* resultsModel_ = nullptr;

    // Backend / helpers
    sqlmini::SQLExecutor executor_;  // imprime a la consola (inyectamos stream)
    QString currentDbPath_;          // ruta de la DB abierta por USE

    // Aux: ejecutar una sentencia y, si es SELECT, poblar tabla
    void executeAndMaybeShowTable(const QString& sql);

    // Detección simple de SELECT
    static bool isSelect(const QString& sql);

    // Renderizar un SELECT en tabla (reutiliza lógica de MiniDBSQL.h)
    // Devuelve true si se pudo mostrar
    bool tryRenderSelect(const QString& sql);

    // Utilidades de parsing (usamos funciones libres de MiniDBSQL.h si están accesibles)
    // Aquí duplicamos una mínima parte necesaria (nombres idénticos a las del header):
    std::vector<std::string> split_csv_local(const std::string& s);
    std::string trim_local(const std::string& s);
    std::string to_upper_local(std::string s);

    bool applyUseFromSQL(const QString& sql);   // detecta y aplica USE
    void setCurrentDbPath(const QString& path); // normaliza y guarda ruta
};

#endif // MAINWINDOW_H
