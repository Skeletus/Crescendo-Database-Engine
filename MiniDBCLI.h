// MiniDBCLI.h
#pragma once
#include <iostream>
#include <string>
#include <limits>
#include <algorithm>
#include "MiniDBSQL.h"   // usa el ejecutor SQL que ya creamos

namespace cli {

inline std::string _trim(const std::string& s){
    size_t a=0,b=s.size();
    while (a<b && std::isspace((unsigned char)s[a])) ++a;
    while (b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}
inline std::string _upper(std::string s){
    for(char& c : s) c = std::toupper((unsigned char)c);
    return s;
}

class MiniDBCLI {
public:
    MiniDBCLI(std::istream& in = std::cin, std::ostream& out = std::cout)
        : is(in), os(out), executor_(out) {}

    void run() {
        print_welcome();
        std::string line;
        for (;;) {
            os << "SQL> ";
            if (!std::getline(is, line)) break;
            auto cmd = _trim(line);
            if (cmd.empty()) continue;

            auto up = _upper(cmd);
            if (up=="HELP") { print_help(); continue; }
            if (up=="EXIT" || up=="QUIT") { os << "bye.\n"; break; }

            // Ejecuta 1 sentencia (con o sin ';')
            executor_.execute(cmd);
        }
    }

private:
    std::istream& is;
    std::ostream& os;
    sqlmini::SQLExecutor executor_;

    void print_welcome(){
        os << "MiniDB SQL Console\n"
              "Escribe 'help' para ver comandos. 'exit' o 'quit' para salir.\n\n";
    }

    void print_help(){
        os <<
            "Comandos soportados:\n"
            "  CREATE DATABASE\n"
            "  USE database\n"
            "  CLOSE DATABASE\n"
            "  SHOW TABLES\n"
            "  CREATE TABLE table_name (col1 TYPE, col2 TYPE, ...)\n"
            "      * Se agrega automaticamente la columna 'id INT' al crear una tabla.\n"
            "  INSERT INTO table_name (col1,col2,...) VALUES (v1,v2,...)\n"
            "  SELECT * FROM table_name\n"
            "  SELECT col1,col2 FROM table_name\n"
            "  SELECT * FROM table_name WHERE id == 1\n"
            "  SELECT * FROM table_name WHERE id >= 2 AND id <= 6\n"
            "  SELECT * FROM table_name WHERE id == 3 OR id == 8\n"
            "  CREATE INDEX idx_name ON table_name (columna)\n"
            "\n"
            "Notas:\n"
            "  • En el primer SELECT * de una tabla se crea un indice B-Tree 'default' sobre la columna 'id'.\n"
            "  • SELECT con WHERE usará un indice existente para esa columna; si no hay, hara escaneo secuencial.\n"
            "  • Tipos soportados en CREATE TABLE: INT, FLOAT, CHAR(n)\n"
            "\n"
            "Ejemplos:\n"
            "  USE mi_db\n"
            "  CREATE TABLE ventas (cliente CHAR(32), total FLOAT, producto CHAR(32))\n"
            "  INSERT INTO ventas (id,cliente,total,producto) VALUES (1,'Ana',120.5,'AURIS')\n"
            "  INSERT INTO ventas (id,cliente,total,producto) VALUES (2,'Bruno',900,'KITE')\n"
            "  SHOW TABLES\n"
            "  SELECT * FROM ventas\n"
            "  CREATE INDEX ix_cli ON ventas (cliente)\n"
            "  SELECT * FROM ventas WHERE id >= 1 AND id <= 10\n"
            "  SELECT cliente,total FROM ventas\n"
            "  CLOSE DATABASE\n"
            "  quit\n\n";
    }
};

} // namespace cli
