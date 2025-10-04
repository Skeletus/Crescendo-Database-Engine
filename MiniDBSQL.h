// MiniDBSQL.h
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <limits>

#include "MiniDatabase.h"
#include "GenericFixedTable.h"
#include "DiskBTreeMulti.h"

namespace sqlmini {

namespace fs = std::filesystem;
using minidb::MiniDatabase;
using gft::GenericFixedTable;
using gft::ColumnDef;
using gft::ColType;
using gft::Value;

// ---------- util ----------
inline std::string trim(const std::string& s){
    size_t a=0,b=s.size();
    while (a<b && std::isspace((unsigned char)s[a])) ++a;
    while (b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}
inline std::string to_upper(std::string s){ for(char& c:s) c=std::toupper((unsigned char)c); return s; }

inline bool parse_type(const std::string& t, ColType& out_type, int& out_len){
    auto T = to_upper(trim(t));
    if (T=="INT" || T=="INTEGER") { out_type=ColType::INT32;  out_len=0; return true; }
    if (T=="FLOAT" || T=="REAL")  { out_type=ColType::FLOAT32; out_len=0; return true; }
    if (T.rfind("CHAR",0)==0){
        auto p = T.find('(');
        auto q = T.find(')');
        if (p!=std::string::npos && q!=std::string::npos && q>p+1){
            int n = std::stoi(T.substr(p+1, q-p-1));
            if (n>0){ out_type=ColType::CHAR; out_len=n; return true; }
        }
    }
    return false;
}

#pragma pack(push,1)
struct _GFT_FileHeader {
    char     magic[8];
    char     table_name[32];
    int32_t  ncols;
    int32_t  row_size;
    int32_t  reserved;
};
struct _GFT_ColMetaDisk {
    char     name[32];
    int32_t  type;    // ColType
    int32_t  width;
    int32_t  offset;
};
#pragma pack(pop)

struct TableSchema {
    std::string table_name;
    int32_t ncols{};
    int32_t row_size{};
    struct Col { std::string name; ColType type; int32_t width; int32_t offset; };
    std::vector<Col> cols;
};

inline bool load_schema_from_tbl(const fs::path& tbl_path, TableSchema& out){
    std::ifstream in(tbl_path, std::ios::binary);
    if (!in.is_open()) return false;
    _GFT_FileHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in.good() || std::memcmp(hdr.magic,"GFTABv1",7)!=0) return false;
    out.table_name = std::string(hdr.table_name, strnlen(hdr.table_name,32));
    out.ncols = hdr.ncols; out.row_size = hdr.row_size;
    out.cols.clear(); out.cols.reserve(hdr.ncols);
    for (int i=0;i<hdr.ncols;++i){
        _GFT_ColMetaDisk c{}; in.read(reinterpret_cast<char*>(&c), sizeof(c));
        out.cols.push_back({std::string(c.name, strnlen(c.name,32)), (ColType)c.type, c.width, c.offset});
    }
    return in.good();
}

inline bool index_exists(const fs::path& dbdir, const std::string& table, const std::string& col, ColType t){
    fs::path tdir = dbdir / table;
    std::string base = table + "_" + col;
    fs::path f;
    if (t==ColType::INT32)   f = tdir / (base + ".bti");
    else if (t==ColType::FLOAT32) f = tdir / (base + ".btf");
    else f = tdir / (base + ".bts");
    return fs::exists(f);
}

inline std::vector<std::string> split_csv(const std::string& s){
    std::vector<std::string> out; std::string cur; bool inq=false;
    for(char c : s){
        if (c=='\''){ inq=!inq; cur.push_back(c); }
        else if (!inq && c==','){ out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(trim(cur));
    return out;
}

inline Value parse_value_literal(const std::string& lit, ColType t){
    std::string L = trim(lit);
    if (t==ColType::INT32)  return Value::Int(std::stoi(L));
    if (t==ColType::FLOAT32) return Value::Flt(std::stof(L));
    if (L.size()>=2 && L.front()=='\'' && L.back()=='\'') L = L.substr(1, L.size()-2);
    return Value::Chr(L);
}

enum class Cmp { EQ, GE, LE, GT, LT, NE };

struct Pred { std::string col; Cmp cmp; std::string lit; };
struct Where { std::optional<Pred> p1; std::optional<Pred> p2; std::string op; };

inline bool parse_where(const std::string& expr, Where& w){
    auto up = to_upper(expr);
    size_t pos_and = up.find(" AND ");
    size_t pos_or  = up.find(" OR ");

    auto parse_pred = [&](std::string s)->std::optional<Pred>{
        s = trim(s); size_t p;
        if ((p=s.find("!="))!=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::NE, trim(s.substr(p+2)) };
        if ((p=s.find(">="))!=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::GE, trim(s.substr(p+2)) };
        if ((p=s.find("<="))!=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::LE, trim(s.substr(p+2)) };
        if ((p=s.find("=="))!=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::EQ, trim(s.substr(p+2)) };
        // aceptar '=' simple como EQ
        if ((p=s.find('=')) !=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::EQ, trim(s.substr(p+1)) };
        if ((p=s.find('>')) !=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::GT, trim(s.substr(p+1)) };
        if ((p=s.find('<')) !=std::string::npos) return Pred{ trim(s.substr(0,p)), Cmp::LT, trim(s.substr(p+1)) };
        return std::nullopt;
    };

    if (pos_and!=std::string::npos){
        auto a = expr.substr(0,pos_and), b = expr.substr(pos_and+5);
        w.p1 = parse_pred(a); w.p2 = parse_pred(b); w.op="AND";
        return w.p1.has_value() && w.p2.has_value();
    }
    if (pos_or!=std::string::npos){
        auto a = expr.substr(0,pos_or), b = expr.substr(pos_or+4);
        w.p1 = parse_pred(a); w.p2 = parse_pred(b); w.op="OR";
        return w.p1.has_value() && w.p2.has_value();
    }
    w.p1 = parse_pred(expr); w.op="";
    return w.p1.has_value();
}

// eval secuencial (usa tipos del schema)
inline bool eval_pred_row(const Pred& p, const TableSchema& sc, const std::vector<Value>& row){
    int idx=-1; ColType t{};
    for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==p.col){ idx=i; t=sc.cols[i].type; break; }
    if (idx==-1) return false;
    const auto& v = row[idx];

    if (t==ColType::INT32){
        int rhs = std::stoi(p.lit);
        switch(p.cmp){
        case Cmp::EQ: return v.i==rhs;
        case Cmp::GE: return v.i>=rhs;
        case Cmp::LE: return v.i<=rhs;
        case Cmp::GT: return v.i> rhs;
        case Cmp::LT: return v.i< rhs;
        case Cmp::NE: return v.i!=rhs;
        }
    } else if (t==ColType::FLOAT32){
        float rhs = std::stof(p.lit);
        switch(p.cmp){
        case Cmp::EQ: return v.f==rhs;
        case Cmp::GE: return v.f>=rhs;
        case Cmp::LE: return v.f<=rhs;
        case Cmp::GT: return v.f> rhs;
        case Cmp::LT: return v.f< rhs;
        case Cmp::NE: return v.f!=rhs;
        }
    } else { // CHAR
        std::string rhs = p.lit;
        if (rhs.size()>=2 && rhs.front()=='\'' && rhs.back()=='\'') rhs = rhs.substr(1,rhs.size()-2);
        switch(p.cmp){
        case Cmp::EQ: return v.s==rhs;
        case Cmp::GE: return v.s>=rhs;
        case Cmp::LE: return v.s<=rhs;
        case Cmp::GT: return v.s> rhs;
        case Cmp::LT: return v.s< rhs;
        case Cmp::NE: return v.s!=rhs;
        }
    }
    return false;
}

// --- helper: halla el ')' correspondiente a '(' en open_pos (respeta anidamiento) ---
inline size_t find_matching_rparen(const std::string& s, size_t open_pos) {
    if (open_pos == std::string::npos || s[open_pos] != '(') return std::string::npos;
    int depth = 0;
    for (size_t i = open_pos; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

class SQLExecutor {
public:
    explicit SQLExecutor(std::ostream& out = std::cout)
        : os(out) {}

    void execute(const std::string& sql) {
        auto s = trim(sql);
        if (s.empty()) return;
        std::string up = to_upper(s);
        if (s.back()==';') { s.pop_back(); up.pop_back(); }

        if      (up.rfind("CREATE DATABASE ",0)==0) cmd_CREATE_DATABASE(trim(s.substr(16)));
        else if (up.rfind("USE ",0)==0)              cmd_USE(trim(s.substr(4)));
        else if (up=="CLOSE DATABASE" || up=="CLOSE") cmd_CLOSE();
        else if (up=="SHOW TABLES")                  cmd_SHOW_TABLES();
        else if (up.rfind("CREATE TABLE",0)==0)      cmd_CREATE_TABLE(s);
        else if (up.rfind("INSERT INTO",0)==0)       cmd_INSERT_INTO(s);
        else if (up.rfind("SELECT",0)==0)            cmd_SELECT(s);
        else if (up.rfind("DELETE FROM",0)==0)       cmd_DELETE_FROM(s);
        else if (up.rfind("UPDATE ",0)==0)            cmd_UPDATE(s);
        else if (up.rfind("CREATE INDEX",0)==0)      cmd_CREATE_INDEX(s);
        else os << "Comando no soportado.\n";
    }

private:
    MiniDatabase db;
    bool opened=false;
    fs::path dbdir;
    std::string dbname;
    std::ostream& os;

    // ---- CREATE DATABASE ----
    void cmd_CREATE_DATABASE(const std::string& name){
        auto n = trim(name);
        if (n.empty()){ os << "Falta nombre de base de datos.\n"; return; }
        try{
            if (fs::exists(n)) { os << "La base ya existe: " << n << "\n"; return; }
            db.crear_base_de_datos(n);
            os << "Base de datos creada: " << n << "\n";
        } catch(const std::exception& e){
            os << "Error: " << e.what() << "\n";
        }
    }

    // ---- USE / CLOSE / SHOW ----
    void cmd_USE(const std::string& name){
        try{
            if (!fs::exists(name) || !fs::is_directory(name)){
                os << "No existe carpeta DB: " << name << "\n"; return;
            }
            db.abrir_base_de_datos(name);
            opened=true; dbdir = fs::path(name); dbname=name;
            os << "Usando base de datos: " << name << "\n";
        }catch(const std::exception& e){ os << "Error: " << e.what() << "\n"; }
    }
    void cmd_CLOSE(){
        if (!opened){ os << "No hay base abierta.\n"; return; }
        db.cerrar_base_de_datos();
        opened=false; dbdir.clear(); dbname.clear();
        os << "Base cerrada.\n";
    }
    void cmd_SHOW_TABLES(){
        if (!opened){ os << "Abra una base con USE.\n"; return; }
        os << "Tablas:\n";
        for (auto& p : fs::directory_iterator(dbdir)){
            if (p.is_directory()){
                auto tname = p.path().filename().string();
                if (fs::exists(p.path()/(tname+".tbl")))
                    os << "  - " << tname << "\n";
            }
        }
    }

    // ---- CREATE TABLE ----
    void cmd_CREATE_TABLE(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }

        std::string up = to_upper(full);
        size_t p1 = up.find("CREATE TABLE");
        if (p1 != 0) { os << "Sintaxis CREATE TABLE inválida.\n"; return; }

        size_t p2 = full.find('(', p1);
        if (p2 == std::string::npos){ os << "Sintaxis CREATE TABLE inválida (falta '(').\n"; return; }

        auto name = trim(full.substr(13, p2 - 13));
        if (name.empty()){ os << "Nombre de tabla vacío.\n"; return; }

        size_t p3 = find_matching_rparen(full, p2);
        if (p3 == std::string::npos){ os << "Sintaxis CREATE TABLE inválida (paréntesis desbalanceados).\n"; return; }

        auto inside = full.substr(p2 + 1, p3 - p2 - 1);
        auto parts = split_csv(inside);
        if (parts.empty()){ os << "Debe definir al menos una columna.\n"; return; }

        std::vector<ColumnDef> schema;
        schema.push_back({"id", ColType::INT32, 0});

        for (auto& def : parts){
            auto def_trim = trim(def);
            auto sp = def_trim.find(' ');
            if (sp == std::string::npos){
                os << "Definición de columna inválida: " << def_trim << "\n"; return;
            }
            auto cname = trim(def_trim.substr(0, sp));
            auto ctype = trim(def_trim.substr(sp + 1));

            ColType ct; int w;
            if (!parse_type(ctype, ct, w)){
                os << "Tipo inválido: " << ctype << "\n"; return;
            }
            schema.push_back({cname, ct, (ct == ColType::CHAR ? w : 0)});
        }

        try{
            db.crear_tabla(name, schema);
            os << "Tabla creada: " << name << " (con columna id INT por defecto)\n";
        } catch (const std::exception& e){
            os << "Error: " << e.what() << "\n";
        }
    }

    // ---- INSERT INTO ----
    // ---- INSERT INTO ----
    void cmd_INSERT_INTO(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }
        auto up = to_upper(full);
        size_t pinto = up.find("INSERT INTO");
        if (pinto!=0){ os << "Sintaxis INSERT inválida.\n"; return; }

        // Nombre de tabla y lista de columnas
        size_t p1 = full.find('(', pinto+11);
        if (p1==std::string::npos){ os << "Faltan columnas.\n"; return; }
        auto tname = trim(full.substr(11, p1-11));
        size_t p2 = full.find(')', p1);
        if (p2==std::string::npos){ os << "Faltan columnas.\n"; return; }
        auto cols_s = full.substr(p1+1, p2-p1-1);
        auto cols = split_csv(cols_s);

        // Lista de valores
        size_t pval = to_upper(full).find("VALUES", p2);
        if (pval==std::string::npos){ os << "Falta VALUES.\n"; return; }
        size_t p3 = full.find('(', pval);
        size_t p4 = full.find(')', p3);
        if (p3==std::string::npos || p4==std::string::npos){ os << "Valores inválidos.\n"; return; }
        auto vals = split_csv(full.substr(p3+1, p4-p3-1));

        if (cols.size()!=vals.size()){ os << "Número de columnas/valores no coincide.\n"; return; }

        // Cargar esquema de la tabla
        fs::path tfile = dbdir / tname / (tname + ".tbl");
        TableSchema sc;
        if (!load_schema_from_tbl(tfile, sc)){ os << "Tabla no existe.\n"; return; }

        // Mapa nombre->índice de columna
        std::unordered_map<std::string,int> cix;
        for (int i=0;i<sc.ncols;++i) cix[sc.cols[i].name]=i;

        // Preparar fila por defecto
        std::vector<Value> row; row.resize(sc.ncols);
        for (int i=0;i<sc.ncols;++i){
            if (sc.cols[i].type==ColType::INT32) row[i]=Value::Int(0);
            else if (sc.cols[i].type==ColType::FLOAT32) row[i]=Value::Flt(0.f);
            else row[i]=Value::Chr("");
        }

        // Asignar valores provistos por el usuario
        bool id_provided = false;
        int id_idx = -1;
        auto it_id = cix.find("id");
        if (it_id != cix.end()) id_idx = it_id->second;

        for (size_t k=0;k<cols.size();++k){
            auto cname = trim(cols[k]);
            auto it = cix.find(cname);
            if (it==cix.end()){ os << "Columna desconocida: " << cname << "\n"; return; }
            int i = it->second;
            row[i] = parse_value_literal(vals[k], sc.cols[i].type);
            if (i == id_idx) id_provided = true;
        }

        try{
            // Autoincrement id si no se proveyó
            GenericFixedTable tbl(tfile.string(), tname, std::vector<ColumnDef>{}, /*create_new*/false);
            if (!id_provided && id_idx >= 0) {
                long current = tbl.Count();
                int next_id = (int)current + 1;
                row[id_idx] = Value::Int(next_id);
            }

            // Insertar y ACTUALIZAR índices a través de MiniDatabase
            long pid = db.insertar_fila(tname, row);
            os << "Insertado pageID=" << pid << " en " << tname << "\n";
        }catch(const std::exception& e){
            os << "Error insertando: " << e.what() << "\n";
        }
    }


    void ensure_default_id_index(const std::string& tname){
        fs::path tfile = dbdir / tname / (tname + ".tbl");
        TableSchema sc; if (!load_schema_from_tbl(tfile, sc)) return;
        int idx=-1;
        for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name=="id") { idx=i; break; }
        if (idx==-1) return;

        // si no existe, créalo; si existe, asegúralo en memoria
        if (!index_exists(dbdir, tname, "id", ColType::INT32)){
            try { db.crear_indice(tname, "id"); os << "(Se creó índice default sobre id)\n"; }
            catch(...) {}
        }
        // NUEVO: asegurar que esté cargado en memoria para poder usarse/actualizarse
        try { db.ensure_indices_loaded(tname); } catch (...) {}
    }


    // ---- SELECT ----
    void cmd_SELECT(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }
        auto up = to_upper(full);
        size_t psel = up.find("SELECT "); if (psel!=0){ os << "Sintaxis SELECT inválida.\n"; return; }
        size_t pfrom = up.find(" FROM ", 7); if (pfrom==std::string::npos){ os << "Falta FROM.\n"; return; }
        auto proj = trim(full.substr(7, pfrom-7));

        size_t pwhere = up.find(" WHERE ", pfrom+6);
        std::string tname, wexpr;
        if (pwhere==std::string::npos){
            tname = trim(full.substr(pfrom+6));
        } else {
            tname = trim(full.substr(pfrom+6, pwhere-(pfrom+6)));
            wexpr = trim(full.substr(pwhere+7));
        }

        fs::path tfile = dbdir / tname / (tname + ".tbl");
        TableSchema sc;
        if (!load_schema_from_tbl(tfile, sc)){ os << "Tabla no existe.\n"; return; }

        std::vector<int> proj_idx;
        if (proj=="*"){
            for (int i=0;i<sc.ncols;++i) proj_idx.push_back(i);
            ensure_default_id_index(tname);
        } else {
            auto cs = split_csv(proj);
            for (auto& c : cs){
                int idx=-1;
                for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==c){ idx=i; break; }
                if (idx==-1){ os << "Columna no existe: " << c << "\n"; return; }
                proj_idx.push_back(idx);
            }
        }

        GenericFixedTable tbl(tfile.string(), tname, std::vector<ColumnDef>{}, /*create_new*/false);
        long n = tbl.Count();
        std::vector<int> pids;

        if (wexpr.empty()){
            pids.resize(n);
            for (long i=0;i<n;++i) pids[i]=(int)i;
        } else {
            // asegurar índices cargados si los vamos a usar
            try { db.ensure_indices_loaded(tname); } catch (...) {}

            Where w{};
            if (!parse_where(wexpr, w)){ os << "WHERE inválido.\n"; return; }
            auto use_pred = w.p1.value();
            int cidx=-1; ColType ct{};
            for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==use_pred.col){ cidx=i; ct=sc.cols[i].type; break; }

            bool used_index=false;
            if (cidx!=-1 && index_exists(dbdir, tname, use_pred.col, ct)){
                try {
                    if (ct==ColType::INT32){
                        int key = std::stoi(use_pred.lit);
                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            auto t2 = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;

                    } else if (ct==ColType::FLOAT32){
                        float key = std::stof(use_pred.lit);
                        auto ninf = -std::numeric_limits<float>::infinity();
                        auto pinf =  std::numeric_limits<float>::infinity();

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            float a = std::nextafter(key, pinf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, a, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            float b = std::nextafter(key, ninf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, b);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            float a = std::nextafter(key, ninf);
                            float b = std::nextafter(key, pinf);
                            auto t1 = db.buscar_rango(tname, use_pred.col, ninf, a);
                            auto t2 = db.buscar_rango(tname, use_pred.col, b, pinf);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;

                    } else { // CHAR
                        std::string L = use_pred.lit;
                        if (L.size()>=2 && L.front()=='\'' && L.back()=='\'') L = L.substr(1,L.size()-2);
                        std::string lo = "";
                        std::string hi = std::string(1, char(127));

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, L);
                            if (pid>=0) pids.push_back(pid);
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, lo, L);
                            auto t2 = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                            used_index=true;
                        }
                    }
                } catch(...) { used_index=false; }
            }
            if (!used_index){
                for (long i=0;i<n;++i) pids.push_back((int)i);
            }

            // Filtro final por predicados (AND/OR) sobre las filas concretas
            std::vector<int> filtered; filtered.reserve(pids.size());
            for (int pid : pids){
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                // saltar tombstones
                int id_idx = -1; for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name=="id"){ id_idx=i; break; }
                if (id_idx>=0 && row[id_idx].i==-1) continue;

                bool r1 = w.p1 ? eval_pred_row(*w.p1, sc, row) : true;
                bool r2 = w.p2 ? eval_pred_row(*w.p2, sc, row) : true;
                bool ok = w.op=="AND" ? (r1 && r2) : (w.op=="OR" ? (r1 || r2) : r1);
                if (ok) filtered.push_back(pid);
            }
            pids.swap(filtered);
        }

        // Encabezado
        for (size_t j=0;j<proj_idx.size();++j){
            os << sc.cols[proj_idx[j]].name << (j+1<proj_idx.size() ? " | " : "\n");
        }

        // Cuerpo + conteo real
        size_t printed = 0;
        for (int pid : pids){
            std::vector<Value> row; if (!tbl.ReadRowByPageID(pid, row)) continue;
            // saltar tombstones también aquí por seguridad
            int id_idx=-1; for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name=="id"){ id_idx=i; break; }
            if (id_idx>=0 && row[id_idx].i==-1) continue;

            for (size_t j=0;j<proj_idx.size();++j){
                int i = proj_idx[j];
                if (sc.cols[i].type==ColType::INT32) os << row[i].i;
                else if (sc.cols[i].type==ColType::FLOAT32) os << row[i].f;
                else os << row[i].s;
                os << (j+1<proj_idx.size() ? " | " : "\n");
            }
            ++printed;
        }
        os << "(filas: " << printed << ")\n";

    }

    // ---- DELETE FROM ----
    // ---- DELETE FROM ----
    void cmd_DELETE_FROM(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }

        // Sintaxis soportada: DELETE FROM table_name [WHERE <expr>]
        auto up = to_upper(full);
        size_t pdel = up.find("DELETE FROM");
        if (pdel != 0){ os << "Sintaxis DELETE inválida.\n"; return; }

        // obtener nombre de tabla y WHERE
        size_t pwhere_kw = up.find(" WHERE ", 12);
        std::string tname, wexpr;
        if (pwhere_kw == std::string::npos) {
            tname = trim(full.substr(12));
            wexpr = "";
        } else {
            tname = trim(full.substr(12, pwhere_kw-12));
            wexpr = trim(full.substr(pwhere_kw+7));
        }

        // abrir esquema
        fs::path tfile = dbdir / tname / (tname + ".tbl");
        TableSchema sc;
        if (!load_schema_from_tbl(tfile, sc)){ os << "Tabla no existe.\n"; return; }

        GenericFixedTable tbl(tfile.string(), tname, std::vector<ColumnDef>{}, /*create_new*/false);
        long n = tbl.Count();

        // asegurar índices cargados si los vamos a usar
        try { db.ensure_indices_loaded(tname); } catch (...) {}

        // recolectar candidatos a borrar (pageIDs)
        std::vector<int> pids; pids.reserve((size_t)n);

        auto is_tombstoned = [&](long pid)->bool{
            std::vector<Value> row;
            if (!tbl.ReadRowByPageID(pid, row)) return true; // si falla lectura, ignora
            int id_idx = -1; for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name=="id"){ id_idx=i; break; }
            return (id_idx>=0 && row[id_idx].i==-1);
        };

        if (wexpr.empty()){
            // DELETE sin WHERE => borrar TODAS las filas "vivas"
            for (long i=0;i<n;++i) {
                if (!is_tombstoned(i)) pids.push_back((int)i);
            }
        } else {
            // DELETE con WHERE => recolectar candidatos, usar índice si es posible (igual que en SELECT)
            Where w{};
            if (!parse_where(wexpr, w)){ os << "WHERE inválido.\n"; return; }

            auto use_pred = w.p1.value();
            int cidx=-1; ColType ct{};
            for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==use_pred.col){ cidx=i; ct=sc.cols[i].type; break; }

            bool used_index=false;
            if (cidx!=-1 && index_exists(dbdir, tname, use_pred.col, ct)){
                try {
                    if (ct==ColType::INT32){
                        int key = std::stoi(use_pred.lit);
                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            auto t2 = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;

                    } else if (ct==ColType::FLOAT32){
                        float key = std::stof(use_pred.lit);
                        auto ninf = -std::numeric_limits<float>::infinity();
                        auto pinf =  std::numeric_limits<float>::infinity();

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            float a = std::nextafter(key, pinf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, a, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            float b = std::nextafter(key, ninf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, b);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            float a = std::nextafter(key, ninf);
                            float b = std::nextafter(key, pinf);
                            auto t1 = db.buscar_rango(tname, use_pred.col, ninf, a);
                            auto t2 = db.buscar_rango(tname, use_pred.col, b, pinf);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;

                    } else { // CHAR
                        std::string L = use_pred.lit;
                        if (L.size()>=2 && L.front()=='\'' && L.back()=='\'') L = L.substr(1,L.size()-2);
                        std::string lo = "";
                        std::string hi = std::string(1, char(127));

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, L);
                            if (pid>=0) pids.push_back(pid);
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                            used_index=true;
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, lo, L);
                            auto t2 = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                            used_index=true;
                        }
                    }
                } catch(...) { used_index=false; }
            }

            if (!used_index){
                for (long i=0;i<n;++i) pids.push_back((int)i);
            }

            // Filtrar exacto (AND/OR) y descartar tombstones
            std::vector<int> filtered; filtered.reserve(pids.size());
            for (int pid : pids){
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                int id_idx = -1; for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name=="id"){ id_idx=i; break; }
                if (id_idx>=0 && row[id_idx].i==-1) continue;

                bool r1 = w.p1 ? eval_pred_row(*w.p1, sc, row) : true;
                bool r2 = w.p2 ? eval_pred_row(*w.p2, sc, row) : true;
                bool ok = w.op=="AND" ? (r1 && r2) : (w.op=="OR" ? (r1 || r2) : r1);
                if (ok) filtered.push_back(pid);
            }
            pids.swap(filtered);
        }

        // deduplicar pids por si vienen de NE/rangos superpuestos
        std::sort(pids.begin(), pids.end());
        pids.erase(std::unique(pids.begin(), pids.end()), pids.end());

        // ejecutar borrado lógico + actualización de índices
        int borradas = 0;
        for (int pid : pids) {
            if (db.borrar_por_pageid(tname, pid)) ++borradas;
        }

        os << "(filas borradas: " << borradas << ")\n";
    }

    // ---- UPDATE ----
    void cmd_UPDATE(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }

        std::string up = to_upper(full);
        // UPDATE <tname> SET <asignaciones> [WHERE <expr>]
        size_t p_update = up.find("UPDATE ");
        if (p_update != 0){ os << "Sintaxis UPDATE inválida.\n"; return; }

        size_t p_set_kw = up.find(" SET ", 7);
        if (p_set_kw == std::string::npos){ os << "Falta SET en UPDATE.\n"; return; }

        std::string tname = trim(full.substr(7, p_set_kw-7));

        // localizar WHERE (si existe) **en la original** pero buscando con up
        size_t p_where_kw_up = up.find(" WHERE ", p_set_kw+5);
        std::string set_part, wexpr;
        if (p_where_kw_up == std::string::npos) {
            set_part = trim(full.substr(p_set_kw+5));
            wexpr.clear();
        } else {
            set_part = trim(full.substr(p_set_kw+5, p_where_kw_up-(p_set_kw+5)));
            wexpr    = trim(full.substr(p_where_kw_up+7));
        }
        if (tname.empty() || set_part.empty()){ os << "Sintaxis UPDATE inválida.\n"; return; }

        // Parseo de asignaciones: "col = valor, col2 = valor2, ..."
        auto assigns = split_csv(set_part);
        if (assigns.empty()){ os << "SET vacío.\n"; return; }

        // Abrir esquema
        fs::path tfile = dbdir / tname / (tname + ".tbl");
        TableSchema sc;
        if (!load_schema_from_tbl(tfile, sc)){ os << "Tabla no existe.\n"; return; }

        // Mapa nombre->idx para tipos
        std::unordered_map<std::string,int> cix;
        for (int i=0;i<sc.ncols;++i) cix[sc.cols[i].name]=i;

        // construir setlist tipado
        std::vector<std::pair<std::string, Value>> setlist; setlist.reserve(assigns.size());
        for (auto& a : assigns){
            auto eqp = a.find('=');
            if (eqp==std::string::npos){ os << "Asignación inválida en SET: " << a << "\n"; return; }
            std::string cname = trim(a.substr(0, eqp));
            std::string lit   = trim(a.substr(eqp+1));
            auto it = cix.find(cname);
            if (it==cix.end()){ os << "Columna desconocida en SET: " << cname << "\n"; return; }
            ColType ct = sc.cols[it->second].type;
            setlist.emplace_back(cname, parse_value_literal(lit, ct));
        }

        // recolectar PIDs como en SELECT
        GenericFixedTable tbl(tfile.string(), tname, std::vector<ColumnDef>{}, /*create_new*/false);
        long n = tbl.Count();
        std::vector<int> pids;

        if (wexpr.empty()){
            pids.resize(n);
            for (long i=0;i<n;++i) pids[i]=(int)i;
        } else {
            Where w{};
            if (!parse_where(wexpr, w)){ os << "WHERE inválido.\n"; return; }
            auto use_pred = w.p1.value();
            int cidx=-1; ColType ct{};
            for (int i=0;i<sc.ncols;++i) if (sc.cols[i].name==use_pred.col){ cidx=i; ct=sc.cols[i].type; break; }

            bool used_index=false;
            if (cidx!=-1 && index_exists(dbdir, tname, use_pred.col, ct)){
                try {
                    if (ct==ColType::INT32){
                        int key = std::stoi(use_pred.lit);
                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, std::numeric_limits<int>::min(), key-1);
                            auto t2 = db.buscar_rango(tname, use_pred.col, key+1, std::numeric_limits<int>::max());
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;
                    } else if (ct==ColType::FLOAT32){
                        float key = std::stof(use_pred.lit);
                        auto ninf = -std::numeric_limits<float>::infinity();
                        auto pinf =  std::numeric_limits<float>::infinity();

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, key);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, key, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, key);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            float a = std::nextafter(key, pinf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, a, pinf);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            float b = std::nextafter(key, ninf);
                            auto tmp = db.buscar_rango(tname, use_pred.col, ninf, b);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            float a = std::nextafter(key, ninf);
                            float b = std::nextafter(key, pinf);
                            auto t1 = db.buscar_rango(tname, use_pred.col, ninf, a);
                            auto t2 = db.buscar_rango(tname, use_pred.col, b, pinf);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;
                    } else { // CHAR
                        std::string L = use_pred.lit;
                        if (L.size()>=2 && L.front()=='\'' && L.back()=='\'') L = L.substr(1,L.size()-2);
                        std::string lo = "";
                        std::string hi = std::string(1, char(127));

                        if (use_pred.cmp==Cmp::EQ){
                            int pid = db.buscar_unitaria(tname, use_pred.col, L);
                            if (pid>=0) pids.push_back(pid);
                        } else if (use_pred.cmp==Cmp::GE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LE){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::GT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else if (use_pred.cmp==Cmp::LT){
                            auto tmp = db.buscar_rango(tname, use_pred.col, lo, L);
                            pids.insert(pids.end(), tmp.begin(), tmp.end());
                        } else { // NE
                            auto t1 = db.buscar_rango(tname, use_pred.col, lo, L);
                            auto t2 = db.buscar_rango(tname, use_pred.col, L, hi);
                            pids.insert(pids.end(), t1.begin(), t1.end());
                            pids.insert(pids.end(), t2.begin(), t2.end());
                        }
                        used_index=true;
                    }
                } catch(...) { used_index=false; }
            }
            if (!used_index){
                for (long i=0;i<n;++i) pids.push_back((int)i);
            }
            // Filtrado exacto con eval_pred_row (+AND/OR)
            std::vector<int> filtered; filtered.reserve(pids.size());
            for (int pid : pids){
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                bool r1 = w.p1 ? eval_pred_row(*w.p1, sc, row) : true;
                bool r2 = w.p2 ? eval_pred_row(*w.p2, sc, row) : true;
                bool ok = w.op=="AND" ? (r1 && r2) : (w.op=="OR" ? (r1 || r2) : r1);
                if (ok) filtered.push_back(pid);
            }
            pids.swap(filtered);
        }

        // Ejecutar UPDATE (escritura + refresco de índices)
        try{
            int nact = db.update_filas_by_pageIDs(tname, pids, setlist);
            os << "(filas actualizadas: " << nact << ")\n";
        } catch(const std::exception& e){
            os << "Error en UPDATE: " << e.what() << "\n";
        }
    }

    // ---- CREATE INDEX ----
    void cmd_CREATE_INDEX(const std::string& full){
        if (!opened){ os << "Abra una base con USE.\n"; return; }
        auto up = to_upper(full);
        size_t p1 = up.find("CREATE INDEX");
        size_t p2 = up.find(" ON ", p1+12);
        if (p1!=0 || p2==std::string::npos){ os << "Sintaxis CREATE INDEX inválida.\n"; return; }
        size_t p3 = full.find('(', p2+4);
        size_t p4 = full.find(')', p3);
        if (p3==std::string::npos || p4==std::string::npos){ os << "Sintaxis CREATE INDEX inválida.\n"; return; }
        auto tname = trim(full.substr(p2+4, p3-(p2+4)));
        auto col = trim(full.substr(p3+1, p4-p3-1));
        try{
            db.crear_indice(tname, col);
            os << "Índice creado para " << tname << "." << col << "\n";
        } catch(const std::exception& e){ os << "Error: " << e.what() << "\n"; }
    }
};

} // namespace sqlmini
