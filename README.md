# MiniDB — motor de BD en C++ + GUI tipo “Workbench” (Qt 6)

MiniDB es un proyecto didáctico escrito en **C++ moderno** que implementa un **motor de base de datos embebido** con tablas de ancho fijo, **índices B-Tree** en disco y un **mini intérprete SQL**. Incluye además un **cliente CLI** y un **Workbench GUI** (Qt 6) con editor SQL, resultados tabulares y explorador de carpetas.

> Objetivo: demostrar dominio práctico de C++ (I/O binario, estructuras en disco, B-Trees, modelos Qt, CMake) construyendo un stack BD completo y funcional.

---

## ✨ Funcionalidades

### Motor de BD

* Tablas de **registros de ancho fijo** persistidas en archivos `.tbl`.
* Tipos soportados: `INT`/`INTEGER` (32-bit), `FLOAT`/`REAL` (32-bit), `CHAR(n)` (fijo).
* **Esquema** auto-contenido al inicio del archivo (cabecera + metadatos de columnas).
* **Columna `id INT` autoincremental** agregada por defecto en `CREATE TABLE`.
* **Borrado lógico** por `pageID` marcando `id = -1`.
* **Índices en disco (B-Tree)** por columna:

  * `*.bti` para `INT`, `*.btf` para `FLOAT`, `*.bts` para `CHAR(32)`.
  * Búsqueda exacta y por rango; duplicados permitidos (multi-valor por clave).
* Intérprete SQL con soporte para:

  * `CREATE DATABASE`, `USE`, `CLOSE`, `SHOW TABLES`
  * `CREATE TABLE`, `CREATE INDEX`
  * `INSERT INTO … VALUES (…)`
  * `SELECT … FROM … [WHERE …]` con operadores `==`, `!=`, `<=`, `>=`, `<`, `>`

    * Usa índice si existe; cae a escaneo secuencial si no.
  * `DELETE FROM … [WHERE …]` (borrado lógico; sincroniza índices)
  * `UPDATE … SET … [WHERE …]` (actualiza archivo, reindexa columnas afectadas)
* Estrategias para **mantener índices frescos** tras `INSERT/DELETE/UPDATE`.

### GUI (Qt 6)

* **Mini-Workbench**:

  * Árbol de archivos (QFileSystemModel + QTreeView).
  * Editor SQL con resaltado (QPlainTextEdit + `SqlHighlighter`).
  * Consola integrada (historial + ejecución).
  * Resultados `SELECT` en tabla (QTableView + `QAbstractTableModel` propio).
  * Botones/acciones: Crear BD, Crear Tabla, Ejecutar, Abrir Terminal.
  * Renderizado de SELECT filtrando filas borradas lógicamente (`id == -1`).
* **Terminal externa opcional**: lanza `demo_cli` desde el Workbench (Windows/macOS/Linux).

---

## 🗂️ Estructura del proyecto

```
MiniDB/
├─ engine/
│  ├─ GenericFixedTable.h         # Tabla de ancho fijo (I/O en disco).
│  ├─ DiskBTreeMulti.h            # B-Tree genérico en disco (int/float/char).
│  ├─ MiniDatabase.h              # Orquestador: DB, tablas, índices.
│  └─ MiniDBSQL.h                 # Intérprete/ejecutor SQL.
│
├─ cli/
│  ├─ MiniDBCLI.h                 # CLI simple (REPL).
│  └─ main_cli_demo.cpp           # demo_cli (main) -> usa MiniDBCLI.
│
├─ gui/ (Qt 6)
│  ├─ mainwindow.h/.cpp           # QMainWindow + toolbar + docks.
│  ├─ resulttablemodel.h/.cpp     # Modelo de resultados para QTableView.
│  ├─ consolewidget.*             # Consola (historial + prompt).
│  ├─ sqlhighlighter.*            # Resaltado de sintaxis básico.
│  └─ MiniDBWorkbench.pro / CMakeLists.txt
│
└─ CMakeLists.txt                 # Build principal
```

> Nota: algunos nombres pueden variar según tu árbol; el README refleja los **archivos reales** utilizados: `GenericFixedTable.h`, `DiskBTreeMulti.h`, `MiniDatabase.h`, `MiniDBSQL.h`, `mainwindow.cpp`, `resulttablemodel.cpp`, `main_cli_demo.cpp`, etc.

---

## ⚙️ Arquitectura técnica

### Tabla de ancho fijo — `GenericFixedTable.h`

* Cabecera `FileHeader` + metadatos de columnas `ColMetaDisk` (packed, `#pragma pack(push,1)`).
* Cada fila se serializa/deserializa con `pack_row`/`unpack_row`.
* API por `pageID`: `AppendRow`, `ReadRowByPageID`, `WriteRowInDisk`, `Count`.
* Getters por nombre de columna (`ReadInt/Float/Char`).
* **Borrado lógico**: la fila se considera “borrada” si su campo `id` vale `-1`.
  (El Workbench y el executor filtran esas filas para `SELECT`.)

### B-Tree en disco — `DiskBTreeMulti.h`

* Plantilla parametrizada por **traits** (`KeyInt`, `KeyFloat`, `KeyChar32`).
* Nodos en disco `NodeDiskGeneric<KEY_BYTES>` con fanout configurable.
* Operaciones: `insert`, `search_get_value`, `range_search_values`, `remove_key`.
* Archivos con cabecera `FileHeader` propia (MAGIC por tipo y metadatos de nodo).

### Capa DB — `MiniDatabase.h`

* Gestiona directorio raíz de la BD (`CREATE/USE/CLOSE`).
* Crea/abre tablas (`GenericFixedTable`) y **construye índices** por columna.
* **Mantiene y reutiliza** índices abiertos en la sesión.
* Exposición de búsquedas indexadas (`buscar_unitaria`, `buscar_rango`) y
  **hooks de mantenimiento** tras `INSERT/DELETE/UPDATE`.

### Intérprete SQL — `MiniDBSQL.h`

* Parser ligero por slicing de strings: `trim`, `to_upper`, `split_csv`.
* `TableSchema` se rellena **leyendo del archivo `.tbl`** (no hay metastore aparte).
* `CREATE TABLE`: inserta siempre `id INT` al frente.
* `INSERT`: autoincrementa `id` si no fue provisto (basado en `Count()+1`).
* `SELECT`: proyección, WHERE (`==`, `!=`, `<=`, `>=`, `<`, `>`) con `AND`/`OR`.
  Usa índice si existe, luego **filtra exacto** por tipos.
* `DELETE FROM`: resuelve `WHERE`, marca filas como borradas (`id=-1`) y **actualiza índices**.
* `UPDATE`: aplica `SET` (int/float/char), reescribe fila en disco y **reindexa** las columnas afectadas.

---

## 🔧 Compilación

### Requisitos

* **CMake ≥ 3.20**
* **Qt 6.x** (Widgets, Core, Gui)
* Compilador C++17 (GCC/Clang/MSVC o MinGW 64-bit en Windows)

### Build (CLI + Engine)

```bash
git clone <repo>
cd MiniDB
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Esto genera, entre otros, `demo_cli` (o `demo_cli.exe` en Windows).

### Build del Workbench (Qt 6)

Con CMake (recomendado):

```bash
cmake -S gui -B build-gui -DCMAKE_PREFIX_PATH="<ruta a Qt6>"
cmake --build build-gui -j
```

> En Windows (MinGW), compila **demo_cli** y el Workbench con el **mismo kit** para evitar problemas de DLL.

---

## ▶️ Ejecución

### CLI

```text
./demo_cli
MiniDB SQL Console
help
```

### Workbench (Qt)

* Abre la app.
* Usa el árbol de archivos para navegar a la carpeta DB, o crea una con **Crear BD**.
* Escribe comandos en el editor y pulsa **Ejecutar**.
* Abre la **Terminal** para lanzar `demo_cli` directamente (la app ajusta el `PATH` a las DLL de Qt/MinGW).

---

## 🧪 Ejemplos de uso

```sql
-- Crear base y tabla
CREATE DATABASE mi_db
USE mi_db
CREATE TABLE ventas (cliente INT, total FLOAT, producto CHAR(32))

-- Insertar (id autoincrementa)
INSERT INTO ventas (cliente,total,producto) VALUES (40, 85.12, 'ANA')
INSERT INTO ventas (cliente,total,producto) VALUES (41, 8127, 'JUAN')

-- Índice por id (se crea automático al primer SELECT *, o explícito)
CREATE INDEX idx_ventas_id ON ventas (id)

-- Consultas
SELECT * FROM ventas
SELECT cliente,total FROM ventas WHERE total >= 100 AND total < 1000
SELECT * FROM ventas WHERE producto != 'ANA'

-- Actualizar
UPDATE ventas SET total = 293.12 WHERE cliente == 44
UPDATE ventas SET producto = 'UPDATED' WHERE producto == 'OK'

-- Borrado lógico
DELETE FROM ventas WHERE id == 9
```

---

## 🧩 Detalles de consistencia de índices

* **INSERT**: después de persistir la fila nueva, se insertan las claves de columnas indexadas (incluida `id`) con su `pageID`.
* **DELETE**: tras marcar `id = -1`, se remueven del índice **todas** las columnas indexadas para ese `pageID`.
* **UPDATE**: para cada columna afectada por `SET` que esté indexada, se elimina la clave vieja y se inserta la nueva (misma `pageID`).

> El Workbench y el executor **filtran en memoria** cualquier fila con `id == -1` para que no aparezca en `SELECT`.

---

## ⚠️ Limitaciones actuales

* No hay `NULL` ni tipos compuestos.
* `CHAR(n)` es ancho fijo en disco (relleno con `\0`), `KeyChar32` indexa a 32 bytes (se trunca/pad).
* El parser SQL es **minimalista** (sin comillas dobles, sin escapes complejos).
* No hay control de concurrencia; pensado para uso embebido/monoproceso.
* No hay vacuum/compactación física (el borrado es lógico).

---

## 🛠️ Solución de problemas

* **En Windows:** si la Terminal del Workbench no arranca `demo_cli.exe`, asegúrate de:

  * Compilar **con el mismo kit** (Qt 6.x MinGW 64-bit).
  * Tener `demo_cli.exe` **junto al ejecutable** del Workbench (misma carpeta `build\...\Debug\` o `Release`).
  * La función `onOpenTerminal()` ajusta el `PATH` en tiempo de ejecución para que el proceso encuentre las DLL.

* **Ves filas borradas en la GUI:** confirma que estás usando las versiones de `mainwindow.cpp` y `resulttablemodel.cpp` que filtran `id == -1`.

---

## 🧭 Roadmap

* `ALTER TABLE` básico (añadir columna al final).
* `ORDER BY` y `LIMIT`.
* `VACUUM` para compactar y reciclar `pageID` de filas borradas.
* Índices compuestos y UNIQUE.
* Tests automatizados (GoogleTest) para B-Tree, Insert/Update/Delete/Select.

---

## 🙌 Contribuir

1. Haz un fork & crea una rama.
2. Respeta el estilo (C++17, warnings altos, `-Wall -Wextra`).
3. Incluye pruebas o casos reproducibles en PRs.

---

## 👤 Autor

Proyecto de demostración de conocimientos sólidos en **C++** (memoria, archivos binarios, estructuras de datos, Qt).
Contact: ja2958110@gmail.com / [LinkedIn](https://www.linkedin.com/in/jesus-arias-9b82902aa/) / https://skeletus.github.io/.

---

Si te interesa profundizar en alguna capa (B-Tree, pack/unpack, o el modelo Qt), avísame y documento esa sección con diagramas y pseudocódigo.
