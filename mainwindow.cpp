#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QTextStream>
#include <QRegularExpression>
#include <filesystem>

#include <QProcess>
#include <QLibraryInfo>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>

using namespace std;
namespace fs = std::filesystem;

// ===== Helpers locales (copian semántica de las del headers SQL, para no depender de privados) =====
std::string MainWindow::trim_local(const std::string& s){
    size_t a = 0, b = s.size();
    while (a<b && isspace((unsigned char)s[a])) ++a;
    while (b>a && isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b-a);
}
std::string MainWindow::to_upper_local(std::string s){
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}
std::vector<std::string> MainWindow::split_csv_local(const std::string& s){
    std::vector<std::string> out; std::string cur; bool inq=false;
    for(char c : s){
        if (c=='\''){ inq=!inq; cur.push_back(c); }
        else if (!inq && c==','){ out.push_back(trim_local(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(trim_local(cur));
    return out;
}

// ===== MainWindow =====
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , executor_(std::cout) // por defecto escribe a stdout; ConsoleWidget intercepta redirección
{
    // ---- Toolbar
    tb_ = addToolBar("Acciones");
    auto actCreateDb  = tb_->addAction("Crear BD");
    auto actCreateTbl = tb_->addAction("Crear Tabla");
    auto actExec      = tb_->addAction("Ejecutar");
    auto actTerm      = tb_->addAction("Abrir Terminal");

    connect(actCreateDb,  &QAction::triggered, this, &MainWindow::onCreateDatabase);
    connect(actCreateTbl, &QAction::triggered, this, &MainWindow::onCreateTable);
    connect(actExec,      &QAction::triggered, this, &MainWindow::onExecuteSQL);
    connect(actTerm,      &QAction::triggered, this, &MainWindow::onOpenTerminal);

    // ---- Dock izquierdo: Árbol de archivos
    dockFiles_ = new QDockWidget("Archivos", this);
    fsModel_ = new QFileSystemModel(this);
    fsModel_->setRootPath(QDir::currentPath()); // raíz = cwd
    tree_ = new QTreeView(dockFiles_);
    tree_->setModel(fsModel_);
    tree_->setRootIndex(fsModel_->index(QDir::currentPath()));
    tree_->setHeaderHidden(false);
    tree_->setColumnWidth(0, 280);
    connect(tree_, &QTreeView::activated, this, &MainWindow::onTreeActivated);
    dockFiles_->setWidget(tree_);
    addDockWidget(Qt::LeftDockWidgetArea, dockFiles_);

    // ---- Central: Editor SQL
    editor_ = new QPlainTextEdit(this);
    editor_->setPlaceholderText("-- Escribe comandos SQL aquí. Usa 'help' en la consola para ver ejemplos.");
    highlighter_ = new SqlHighlighter(editor_->document());
    setCentralWidget(editor_);

    // ---- Dock inferior: Consola
    dockConsole_ = new QDockWidget("Consola SQL", this);
    console_ = new ConsoleWidget(this);
    dockConsole_->setWidget(console_);
    addDockWidget(Qt::BottomDockWidgetArea, dockConsole_);

    // ---- Dock inferior: Resultados
    dockResults_ = new QDockWidget("Resultados", this);
    resultsView_ = new QTableView(this);
    resultsModel_ = new ResultTableModel(this);
    resultsView_->setModel(resultsModel_);
    resultsView_->horizontalHeader()->setStretchLastSection(true);
    dockResults_->setWidget(resultsView_);
    addDockWidget(Qt::BottomDockWidgetArea, dockResults_);

    // Consola: conectar ejecución directa (REPL)
    connect(console_, &ConsoleWidget::executeRequested, this, [this](const QString& sqlLine){
        executeAndMaybeShowTable(sqlLine);
    });

    // Atajo: Ctrl+Enter ejecuta
    editor_->installEventFilter(this);
}

void MainWindow::onCreateDatabase() {
    bool ok=false;
    QString name = QInputDialog::getText(this, "Crear Base de Datos",
                                         "Nombre de carpeta (DB):", QLineEdit::Normal,
                                         "mi_db", &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    QString cmd = "CREATE DATABASE " + name.trimmed();
    console_->appendLine(">> " + cmd);
    executor_.execute(cmd.toStdString());

    // Auto-USE (opcional)
    QString useCmd = "USE " + name.trimmed();
    console_->appendLine(">> " + useCmd);
    executor_.execute(useCmd.toStdString());
    applyUseFromSQL(useCmd);
}


void MainWindow::onCreateTable() {
    // Presenta un diálogo simple para escribir el CREATE TABLE completo
    bool ok=false;
    QString sql = QInputDialog::getMultiLineText(this, "Crear Tabla",
                                                 "Ejemplo:\nCREATE TABLE ventas (cliente CHAR(32), total FLOAT, producto INT)", "", &ok);
    if (!ok || sql.trimmed().isEmpty()) return;
    console_->appendLine(">> " + sql);
    executor_.execute(sql.toStdString());
}

void MainWindow::onExecuteSQL() {
    const QString sql = editor_->toPlainText().trimmed();
    if (sql.isEmpty()) return;
    executeAndMaybeShowTable(sql);
}

void MainWindow::onOpenTerminal() {
#ifdef Q_OS_WIN
    const QString workbenchDir = QCoreApplication::applicationDirPath();
    const QString demoCli = QDir(workbenchDir).filePath("demo_cli.exe");
    if (!QFileInfo::exists(demoCli)) {
        QMessageBox::warning(this, "Terminal",
                             QString("No se encontró demo_cli.exe en:\n%1").arg(demoCli));
        return;
    }

    // Asegura que el proceso vea las mismas DLL (Qt/MinGW) que tu Workbench
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString qtBin = QLibraryInfo::path(QLibraryInfo::BinariesPath);
    QString path = env.value("PATH");
    path = QDir::toNativeSeparators(workbenchDir) + ";" +
           QDir::toNativeSeparators(qtBin) + ";" + path;
    env.insert("PATH", path);

    // Lanza demo_cli.exe directamente (es app de consola -> Windows abre su ventana)
    auto *p = new QProcess(this);
    p->setProcessEnvironment(env);
    p->setWorkingDirectory(workbenchDir);
    p->setProgram(demoCli);
    if (!p->startDetached()) {
        QMessageBox::warning(this, "Terminal",
                             QString("No se pudo iniciar:\n%1").arg(demoCli));
    }
    p->deleteLater();

#elif defined(Q_OS_MACOS)
    // Abre Terminal y ejecuta el binario desde la carpeta del Workbench
    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("demo_cli");
    QProcess::startDetached("open", {"-a", "Terminal", exe},
                            QCoreApplication::applicationDirPath());
#else
    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("demo_cli");
    if (!QProcess::startDetached("gnome-terminal", {"--", exe},
                                 QCoreApplication::applicationDirPath())) {
        QProcess::startDetached("xterm", {"-e", exe},
                                QCoreApplication::applicationDirPath());
    }
#endif
}

void MainWindow::onTreeActivated(const QModelIndex& idx) {
    const QString path = fsModel_->filePath(idx);
    if (QFileInfo(path).isDir()) {
        // Sugerir USE db si parece ser carpeta de DB (tiene *.tbl dentro)
        QDir d(path);
        auto list = d.entryList(QStringList() << "*.tbl", QDir::Files);
        if (!list.isEmpty()) {
            QString cmd = "USE " + QFileInfo(path).fileName();
            console_->appendLine(">> " + cmd);
            executor_.execute(cmd.toStdString());
            currentDbPath_ = path;
        }
    }
}

bool MainWindow::isSelect(const QString& sql) {
    QString s = sql.trimmed();
    return s.left(6).toUpper()=="SELECT";
}

void MainWindow::executeAndMaybeShowTable(const QString& sql) {
    console_->appendLine(">> " + sql);

    // 1) Ejecuta el SQL (si es USE, MiniDBSQL cambiará internamente)
    executor_.execute(sql.toStdString());

    // 2) Actualiza el GUI si era un USE (para que tryRenderSelect sepa la ruta)
    applyUseFromSQL(sql);

    // 3) Si es SELECT, intenta renderizarlo en la tabla
    if (isSelect(sql)) {
        if (!tryRenderSelect(sql)) {
            // Si falla, deja solo el output textual del executor
            // (opcional: console_->appendLine("[GUI] No se pudo renderizar SELECT");
        }
    }
}


// --------- SELECT renderer (no modifica tu executor; lee directamente la tabla) ----------
#include "MiniDBSQL.h"  // reutilizamos helpers públicos

bool MainWindow::tryRenderSelect(const QString& qsql) {
    using namespace sqlmini;
    using gft::GenericFixedTable; using gft::Value; using gft::ColType;

    std::string full = qsql.toStdString();
    auto up = to_upper_local(full);
    size_t psel = up.find("SELECT "); if (psel!=0) return false;
    size_t pfrom = up.find(" FROM ", 7); if (pfrom==std::string::npos) return false;
    auto proj = trim_local(full.substr(7, pfrom-7));

    size_t pwhere = up.find(" WHERE ", pfrom+6);
    std::string tname, wexpr;
    if (pwhere==std::string::npos) tname = trim_local(full.substr(pfrom+6));
    else {
        tname = trim_local(full.substr(pfrom+6, pwhere-(pfrom+6)));
        wexpr = trim_local(full.substr(pwhere+7));
    }

    // Localiza ruta de tabla bajo currentDbPath_
    if (currentDbPath_.isEmpty()) return false;
    fs::path tfile = fs::path(currentDbPath_.toStdString()) / tname / (tname + ".tbl");

    // Reutilizar la lectura del esquema desde MiniDBSQL.h (función global)
    sqlmini::TableSchema sc;
    if (!sqlmini::load_schema_from_tbl(tfile, sc)) return false;

    // Proyección
    std::vector<int> proj_idx;
    if (proj == "*") {
        for (int i=0;i<sc.ncols;++i) proj_idx.push_back(i);
    } else {
        auto cs = split_csv_local(proj);
        for (auto& c : cs){
            int idx=-1;
            for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==c){ idx=i; break; }
            if (idx==-1) return false;
            proj_idx.push_back(idx);
        }
    }

    // Localiza índice de la columna 'id' (para filtrar borrados lógicos id == -1)
    int id_idx = -1;
    for (int i=0;i<sc.ncols;++i) {
        if (sc.cols[i].name == "id" && sc.cols[i].type == ColType::INT32) { id_idx = i; break; }
    }
    auto is_deleted = [&](const std::vector<Value>& row)->bool {
        if (id_idx < 0) return false;
        return row[id_idx].i == -1;
    };

    // Cargar toda la tabla (para simplicidad; si quieres, puedes replicar el uso de índices aquí)
    GenericFixedTable tbl(tfile.string(), tname, std::vector<gft::ColumnDef>{}, /*create_new*/false);
    const long n = tbl.Count();

    // Si hay WHERE, filtramos con helpers del executor:
    std::vector<int> pids;
    if (wexpr.empty()) {
        for (long i=0;i<n;++i) {
            std::vector<Value> row;
            if (!tbl.ReadRowByPageID(i, row)) continue;
            if (is_deleted(row)) continue;              // <<--- filtra borradas
            pids.push_back((int)i);
        }
    } else {
        sqlmini::Where w{};
        if (!sqlmini::parse_where(wexpr, w)) return false;
        for (long i=0;i<n;++i){
            std::vector<Value> row;
            if (!tbl.ReadRowByPageID(i, row)) continue;
            if (is_deleted(row)) continue;              // <<--- filtra borradas antes de evaluar WHERE
            bool r1 = w.p1 ? sqlmini::eval_pred_row(*w.p1, sc, row) : true;
            bool r2 = w.p2 ? sqlmini::eval_pred_row(*w.p2, sc, row) : true;
            bool ok = w.op=="AND" ? (r1 && r2) : (w.op=="OR" ? (r1 || r2) : r1);
            if (ok) pids.push_back((int)i);
        }
    }

    // Construye headers y filas para el QTableView
    QStringList headers;
    for (int j : proj_idx) headers << QString::fromStdString(sc.cols[j].name);

    std::vector<std::vector<Value>> data;
    data.reserve(pids.size());
    for (int pid : pids){
        std::vector<Value> row;
        if (!tbl.ReadRowByPageID(pid, row)) continue;
        if (is_deleted(row)) continue;
        std::vector<Value> out;
        out.reserve(proj_idx.size());
        for (int j : proj_idx) out.push_back(row[j]);
        data.push_back(std::move(out));
    }

    resultsModel_->setData(headers, data, sc); // muestra
    resultsView_->resizeColumnsToContents();
    return true;
}

bool MainWindow::applyUseFromSQL(const QString& sql) {
    // Acepta "USE name" (con o sin ';'), espacios extras, case-insensitive.
    QString s = sql.trimmed();
    if (s.endsWith(';')) s.chop(1);
    if (!s.startsWith("USE ", Qt::CaseInsensitive)) return false;

    QString dbname = s.mid(4).trimmed(); // después de "USE "
    if (dbname.isEmpty()) return false;

    // Si es ruta absoluta, úsala; si es nombre, asume subcarpeta del cwd
    QString path = dbname;
    if (QDir::isAbsolutePath(dbname)) {
        path = dbname;
    } else {
        path = QDir::current().absoluteFilePath(dbname);
    }

    // Verifica que exista y sea carpeta
    QDir d(path);
    if (!d.exists()) {
        console_->appendLine(QString("[GUI] Carpeta DB no existe: %1").arg(path));
        return false;
    }

    setCurrentDbPath(path);
    return true;
}

void MainWindow::setCurrentDbPath(const QString& path) {
    currentDbPath_ = QDir(path).absolutePath();

    // Opcional: mover el árbol a esa carpeta
    if (fsModel_) {
        QModelIndex idx = fsModel_->index(currentDbPath_);
        if (idx.isValid()) {
            tree_->setRootIndex(idx);
        }
    }

    // Feedback visual
    console_->appendLine(QString("[GUI] DB actual: %1").arg(currentDbPath_));
}
