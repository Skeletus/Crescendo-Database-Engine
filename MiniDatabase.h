// MiniDatabase.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <iostream>

#include "GenericFixedTable.h"
#include "DiskBTreeMulti.h"

namespace minidb {

namespace fs = std::filesystem;
using gft::GenericFixedTable;
using gft::ColumnDef;
using gft::ColType;
using gft::Value;

struct TablaInfo {
    std::unique_ptr<GenericFixedTable> tabla; // tabla abierta en esta sesión
    // Índices por columna (nombre de columna -> índice)
    std::unordered_map<std::string, std::unique_ptr<diskbtree::BTreeInt>>    idx_int;
    std::unordered_map<std::string, std::unique_ptr<diskbtree::BTreeFloat>>  idx_float;
    std::unordered_map<std::string, std::unique_ptr<diskbtree::BTreeChar32>> idx_char;
    // Mapa: nombre columna -> tipo
    std::unordered_map<std::string, ColType> col_tipos;
};

class MiniDatabase {
public:
    MiniDatabase() = default;

    // --------- Gestión de base de datos (carpeta) ---------
    void crear_base_de_datos(const std::string& ruta) {
        fs::path p(ruta);
        if (fs::exists(p)) throw std::runtime_error("La ruta ya existe: " + ruta);
        fs::create_directories(p);
    }

    void abrir_base_de_datos(const std::string& ruta) {
        root = fs::path(ruta);
        if (!fs::exists(root) || !fs::is_directory(root)) {
            throw std::runtime_error("No existe carpeta de DB: " + ruta);
        }
        abierta = true;
        tablas.clear();
    }

    void cerrar_base_de_datos() {
        tablas.clear();
        abierta = false;
        root.clear();
    }

    // --------- Tablas ---------
    // Crea una tabla nueva dentro de la DB y la deja abierta en esta sesión
    void crear_tabla(const std::string& nombre, const std::vector<ColumnDef>& esquema) {
        asegurar_abierta();
        fs::path tdir = root / nombre;
        fs::create_directories(tdir);
        fs::path tfile = tdir / (nombre + ".tbl");

        auto tabla = std::make_unique<GenericFixedTable>(tfile.string(), nombre, esquema, /*create_new*/true);
        TablaInfo ti;
        ti.tabla = std::move(tabla);
        for (const auto& c : esquema) {
            ti.col_tipos[c.name] = c.type;
        }
        tablas[nombre] = std::move(ti);
    }

    // Abre una tabla ya existente (si la cerraste o en una nueva sesión)
    void abrir_tabla(const std::string& nombre) {
        asegurar_abierta();
        fs::path tdir = root / nombre;
        fs::path tfile = tdir / (nombre + ".tbl");
        if (!fs::exists(tfile)) throw std::runtime_error("No existe tabla: " + nombre);

        auto tabla = std::make_unique<GenericFixedTable>(tfile.string(), nombre, std::vector<ColumnDef>{}, /*create_new*/false);
        TablaInfo ti;
        ti.tabla = std::move(tabla);
        // tipos se irán llenando bajo demanda (crear_indice / ensure_indices_loaded / detectar_tipo_columna)
        tablas[nombre] = std::move(ti);
    }

    // --------- Índices ---------
    // Crea índice para una columna; detecta ColType y construye el índice apropiado
    void crear_indice(const std::string& nombre_tabla, const std::string& columna, int t_btree = 8) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        auto& tbl = *ti.tabla;

        ColType tipo = detectar_tipo_columna(tbl, ti, columna);

        fs::path tdir = root / nombre_tabla;
        fs::create_directories(tdir);
        std::string base = nombre_tabla + "_" + columna;
        fs::path idx_file;

        long n = tbl.Count();

        if (tipo == ColType::INT32) {
            idx_file = tdir / (base + ".bti");
            auto idx = std::make_unique<diskbtree::BTreeInt>(idx_file.string(), t_btree, /*create_new*/true);
            for (long pid = 0; pid < n; ++pid) {
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                // saltar tombstones (id == -1)
                int id_idx = tbl.col_index("id");
                if (id_idx >= 0 && row[id_idx].i == -1) continue;
                int v = tbl.ReadInt(pid, columna);
                idx->insert(v, (int)pid);
            }
            ti.idx_int[columna] = std::move(idx);
            ti.col_tipos[columna] = ColType::INT32;
        } else if (tipo == ColType::FLOAT32) {
            idx_file = tdir / (base + ".btf");
            auto idx = std::make_unique<diskbtree::BTreeFloat>(idx_file.string(), t_btree, /*create_new*/true);
            for (long pid = 0; pid < n; ++pid) {
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                int id_idx = tbl.col_index("id");
                if (id_idx >= 0 && row[id_idx].i == -1) continue;
                float v = tbl.ReadFloat(pid, columna);
                idx->insert(v, (int)pid);
            }
            ti.idx_float[columna] = std::move(idx);
            ti.col_tipos[columna] = ColType::FLOAT32;
        } else if (tipo == ColType::CHAR) {
            idx_file = tdir / (base + ".bts");
            auto idx = std::make_unique<diskbtree::BTreeChar32>(idx_file.string(), t_btree, /*create_new*/true);
            for (long pid = 0; pid < n; ++pid) {
                std::vector<Value> row;
                if (!tbl.ReadRowByPageID(pid, row)) continue;
                int id_idx = tbl.col_index("id");
                if (id_idx >= 0 && row[id_idx].i == -1) continue;
                std::string v = tbl.ReadChar(pid, columna);
                idx->insert(v, (int)pid);
            }
            ti.idx_char[columna] = std::move(idx);
            ti.col_tipos[columna] = ColType::CHAR;
        } else {
            throw std::runtime_error("Tipo de columna no soportado para índice");
        }
    }

    // ---------- NUEVO: Carga perezosa de índices existentes ----------
    void ensure_indices_loaded(const std::string& nombre_tabla) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        fs::path tdir = root / nombre_tabla;
        if (!fs::exists(tdir)) return;

        for (auto& e : fs::directory_iterator(tdir)) {
            if (!e.is_regular_file()) continue;
            auto fn = e.path().filename().string();     // ventas_col.bti / .btf / .bts
            auto ext = e.path().extension().string();   // .bti etc.
            if (ext!=".bti" && ext!=".btf" && ext!=".bts") continue;

            // obtener nombre de columna a partir de "<tabla>_<col>.<ext>"
            auto stem = e.path().stem().string(); // ventas_col
            std::string prefix = nombre_tabla + "_";
            if (stem.rfind(prefix, 0) != 0) continue;
            std::string col = stem.substr(prefix.size());

            try {
                if (ext == ".bti" && ti.idx_int.find(col)==ti.idx_int.end()) {
                    ti.idx_int[col] = std::make_unique<diskbtree::BTreeInt>(e.path().string(), /*t ignored*/2, /*create_new*/false);
                    ti.col_tipos[col] = ColType::INT32;
                } else if (ext == ".btf" && ti.idx_float.find(col)==ti.idx_float.end()) {
                    ti.idx_float[col] = std::make_unique<diskbtree::BTreeFloat>(e.path().string(), 2, false);
                    ti.col_tipos[col] = ColType::FLOAT32;
                } else if (ext == ".bts" && ti.idx_char.find(col)==ti.idx_char.end()) {
                    ti.idx_char[col] = std::make_unique<diskbtree::BTreeChar32>(e.path().string(), 2, false);
                    ti.col_tipos[col] = ColType::CHAR;
                }
            } catch (...) {
                // ignorar errores de carga individual para no romper la sesión completa
            }
        }
    }

    // ---------- NUEVO: Insert que actualiza índices ----------
    long insertar_fila(const std::string& nombre_tabla, const std::vector<Value>& row) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        ensure_indices_loaded(nombre_tabla); // para que actualicemos todo lo existente

        long pid = ti.tabla->AppendRow(row);

        // actualizar índices que tengamos cargados
        for (auto& kv : ti.idx_int) {
            const std::string& col = kv.first;
            try {
                int v = ti.tabla->ReadInt(pid, col);
                kv.second->insert(v, (int)pid);
            } catch(...) {}
        }
        for (auto& kv : ti.idx_float) {
            const std::string& col = kv.first;
            try {
                float v = ti.tabla->ReadFloat(pid, col);
                kv.second->insert(v, (int)pid);
            } catch(...) {}
        }
        for (auto& kv : ti.idx_char) {
            const std::string& col = kv.first;
            try {
                std::string v = ti.tabla->ReadChar(pid, col);
                kv.second->insert(v, (int)pid);
            } catch(...) {}
        }
        return pid;
    }

    // ---------- NUEVO: Borrado lógico por pageID con actualización de índices ----------
    bool borrar_por_pageid(const std::string& nombre_tabla, long pid) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        ensure_indices_loaded(nombre_tabla);

        std::vector<Value> row;
        if (!ti.tabla->ReadRowByPageID(pid, row)) return false;

        // si ya está borrada (id == -1), no hacer nada
        int id_idx = ti.tabla->col_index("id");
        if (id_idx >= 0 && row[id_idx].i == -1) return false;

        // quitar de índices (nota: remove_key quita una ocurrencia; con claves duplicadas puede quitar cualquiera)
        for (auto& kv : ti.idx_int) {
            const std::string& col = kv.first;
            try { int v = ti.tabla->ReadInt(pid, col); kv.second->remove_key(v); } catch(...) {}
        }
        for (auto& kv : ti.idx_float) {
            const std::string& col = kv.first;
            try { float v = ti.tabla->ReadFloat(pid, col); kv.second->remove_key(v); } catch(...) {}
        }
        for (auto& kv : ti.idx_char) {
            const std::string& col = kv.first;
            try { std::string v = ti.tabla->ReadChar(pid, col); kv.second->remove_key(v); } catch(...) {}
        }

        // tombstone: id = -1
        if (id_idx >= 0) row[id_idx] = Value::Int(-1);
        ti.tabla->WriteRowInDisk(pid, row);
        return true;
    }

    // --------- Operaciones sobre índices (lectura) ---------
    int buscar_unitaria(const std::string& nombre_tabla, const std::string& columna, int clave_int) {
        ensure_indices_loaded(nombre_tabla);
        auto* idx = obtener_indice_int(nombre_tabla, columna);
        return idx->search_get_value(clave_int);
    }
    int buscar_unitaria(const std::string& nombre_tabla, const std::string& columna, float clave_flt) {
        ensure_indices_loaded(nombre_tabla);
        auto* idx = obtener_indice_float(nombre_tabla, columna);
        return idx->search_get_value(clave_flt);
    }
    int buscar_unitaria(const std::string& nombre_tabla, const std::string& columna, const std::string& clave_str) {
        ensure_indices_loaded(nombre_tabla);
        auto* idx = obtener_indice_char(nombre_tabla, columna);
        return idx->search_get_value(clave_str);
    }

    std::vector<int> buscar_rango(const std::string& nt, const std::string& col, int a, int b) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_int(nt, col);
        return idx->range_search_values(a, b);
    }
    std::vector<int> buscar_rango(const std::string& nt, const std::string& col, float a, float b) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_float(nt, col);
        return idx->range_search_values(a, b);
    }
    std::vector<int> buscar_rango(const std::string& nt, const std::string& col, const std::string& a, const std::string& b) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_char(nt, col);
        return idx->range_search_values(a, b);
    }

    // Eliminación de una ocurrencia por clave (en el índice)
    void eliminar_por_clave(const std::string& nt, const std::string& col, int clave_int) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_int(nt, col);
        idx->remove_key(clave_int);
    }
    void eliminar_por_clave(const std::string& nt, const std::string& col, float clave_flt) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_float(nt, col);
        idx->remove_key(clave_flt);
    }
    void eliminar_por_clave(const std::string& nt, const std::string& col, const std::string& clave_str) {
        ensure_indices_loaded(nt);
        auto* idx = obtener_indice_char(nt, col);
        idx->remove_key(clave_str);
    }

    // Lectura de una fila por pageID (devuelve vector<Value>)
    bool leer_fila(const std::string& nombre_tabla, long pageID, std::vector<Value>& out) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        return ti.tabla->ReadRowByPageID(pageID, out);
    }

    // ---- UPDATE por pageID con mantenimiento de índices ----
    bool actualizar_por_pageid(const std::string& nombre_tabla,
                               long pageID,
                               const std::vector<std::pair<std::string, Value>>& cambios)
    {
        if (cambios.empty()) return false;

        TablaInfo& ti = obtener_tabla(nombre_tabla);
        auto& tbl = *ti.tabla;
        ensure_indices_loaded(nombre_tabla);

        std::vector<Value> row;
        if (!tbl.ReadRowByPageID(pageID, row)) return false;

        int id_col = tbl.col_index("id");
        if (id_col>=0 && row[id_col].t==ColType::INT32 && row[id_col].i==-1) return false; // borrado

        // Aplicar cambios sobre copia y preparar actualizaciones de índices
        struct DeltaIdx { std::string col; ColType t; Value oldv; Value newv; };
        std::vector<DeltaIdx> deltas;

        auto eq_val = [](const Value& a, const Value& b)->bool{
            if (a.t!=b.t) return false;
            if (a.t==ColType::INT32)   return a.i==b.i;
            if (a.t==ColType::FLOAT32) return a.f==b.f;
            return a.s==b.s;
        };

        for (auto& [cname, vnew] : cambios){
            int cix = tbl.col_index(cname);
            if (cix<0) throw std::runtime_error("Columna no existe en UPDATE: " + cname);

            // validar tipo
            if (row[cix].t != vnew.t)
                throw std::runtime_error("Tipo incompatible en UPDATE para columna: " + cname);

            Value vold = row[cix];
            if (eq_val(vold, vnew)) continue; // sin cambio

            // Registrar delta solo si esa columna tiene índice
            bool has_index = (ti.idx_int.count(cname) || ti.idx_float.count(cname) || ti.idx_char.count(cname));
            if (has_index) deltas.push_back({cname, vnew.t, vold, vnew});

            row[cix] = vnew; // aplicar cambio en memoria
        }

        // Si no hubo cambios reales, nada que hacer
        bool any_change = false;
        for (auto& [cname, vnew] : cambios){
            int cix = tbl.col_index(cname);
            if (cix<0) continue;
            if (!eq_val(row[cix], vnew)) { any_change = true; break; }
        }
        if (!any_change && deltas.empty()) return false;

        // Persistir fila
        tbl.WriteRowInDisk(pageID, row);

        // Actualizar índices (remove old -> insert new)
        for (auto& d : deltas){
            if (d.t==ColType::INT32){
                auto it = ti.idx_int.find(d.col); if (it!=ti.idx_int.end()){
                    it->second->remove_key(d.oldv.i);
                    it->second->insert(d.newv.i, (int)pageID);
                }
            } else if (d.t==ColType::FLOAT32){
                auto it = ti.idx_float.find(d.col); if (it!=ti.idx_float.end()){
                    it->second->remove_key(d.oldv.f);
                    it->second->insert(d.newv.f, (int)pageID);
                }
            } else { // CHAR
                auto it = ti.idx_char.find(d.col); if (it!=ti.idx_char.end()){
                    it->second->remove_key(d.oldv.s);
                    it->second->insert(d.newv.s, (int)pageID);
                }
            }
        }

        return true;
    }

    // --- NUEVO: expone tipo de columna por nombre (leyéndolo del archivo de la tabla) ---
    ColType tipo_columna(const std::string& nombre_tabla, const std::string& col) {
        TablaInfo& ti = obtener_tabla(nombre_tabla);
        // cache si ya lo conocemos
        auto it = ti.col_tipos.find(col);
        if (it != ti.col_tipos.end()) return it->second;

        // Detectar vía lectura 0..N hasta que encuentre la col
        // Usamos las helpers "getters" que ya verifican tipo
        try { (void)ti.tabla->ReadInt(0,  col); ti.col_tipos[col]=ColType::INT32;  return ColType::INT32; } catch(...) {}
        try { (void)ti.tabla->ReadFloat(0,col); ti.col_tipos[col]=ColType::FLOAT32; return ColType::FLOAT32; } catch(...) {}
        try { (void)ti.tabla->ReadChar(0, col); ti.col_tipos[col]=ColType::CHAR;    return ColType::CHAR; } catch(...) {}
        throw std::runtime_error("No se pudo determinar tipo de columna: " + nombre_tabla + "." + col);
    }

    // --- NUEVO: refresca índices para una fila y un conjunto de columnas cambiadas ---
    void actualizar_fila_en_indices(const std::string& nt,
                                    long pid,
                                    const std::vector<std::pair<std::string, Value>>& cambios,
                                    const std::vector<Value>& row_antes,
                                    const std::vector<Value>& row_despues)
    {
        TablaInfo& ti = obtener_tabla(nt);

        auto apply_one = [&](const std::string& c, const Value& before, const Value& after){
            auto itI = ti.idx_int.find(c);
            if (itI != ti.idx_int.end()) {
                // quitar clave vieja e insertar nueva
                itI->second->remove_key(before.i);
                itI->second->insert(after.i, (int)pid);
                return;
            }
            auto itF = ti.idx_float.find(c);
            if (itF != ti.idx_float.end()) {
                itF->second->remove_key(before.f);
                itF->second->insert(after.f, (int)pid);
                return;
            }
            auto itS = ti.idx_char.find(c);
            if (itS != ti.idx_char.end()) {
                itS->second->remove_key(before.s);
                itS->second->insert(after.s, (int)pid);
                return;
            }
            // si no hay índice para esa columna, nada que hacer
        };

        // Para cada columna cambiada, localizar su índice en la fila y refrescar si tiene índice abierto
        for (auto& kv : cambios) {
            const std::string& col = kv.first;
            // localizar posición de columna en la fila
            // Reconstruimos por nombre con las getters que ya validan tipos:
            try {
                ColType t = tipo_columna(nt, col);
                // buscamos valor "before" y "after" exactos en los vectores (mismo orden de columnas del archivo)
                // row_antes / row_despues vienen en orden físico de columnas
                // Encontrar índice por nombre:
                int idx_physical = ti.tabla->col_index(col);
                if (idx_physical < 0) continue;
                apply_one(col, row_antes[idx_physical], row_despues[idx_physical]);
            } catch(...) {
                // si falla determinar tipo o leer, ignoramos esa columna para índices
            }
        }
    }

    // --- NUEVO: actualiza múltiples filas (por pageID) aplicando set-list ---
    int update_filas_by_pageIDs(const std::string& nt,
                                const std::vector<int>& pids,
                                const std::vector<std::pair<std::string, Value>>& setlist)
    {
        TablaInfo& ti = obtener_tabla(nt);
        auto& tbl = *ti.tabla;

        // Mapear nombre->indice físico para aplicar más rápido
        std::vector<int> idxs; idxs.reserve(setlist.size());
        std::vector<ColType> tipos; tipos.reserve(setlist.size());
        for (auto& kv : setlist) {
            const std::string& col = kv.first;
            int idx = tbl.col_index(col);
            if (idx < 0) throw std::runtime_error("Columna no existe: " + col);
            idxs.push_back(idx);
            tipos.push_back(tipo_columna(nt, col));
        }

        int count = 0;
        for (int pid : pids) {
            std::vector<Value> before, after;
            if (!tbl.ReadRowByPageID(pid, before)) continue;
            after = before;

            // Aplicar cambios con verificación de tipo
            for (size_t k=0;k<setlist.size();++k){
                const auto& col = setlist[k].first;
                const auto& v   = setlist[k].second;
                int idx = idxs[k];
                ColType t = tipos[k];

                if (t != v.t) {
                    // permitir casteos sencillos INT<->FLOAT si hace falta:
                    if (t==ColType::INT32 && v.t==ColType::FLOAT32) {
                        after[idx] = Value::Int((int32_t)std::lround(v.f));
                    } else if (t==ColType::FLOAT32 && v.t==ColType::INT32) {
                        after[idx] = Value::Flt((float)v.i);
                    } else if (t==ColType::CHAR && (v.t==ColType::INT32 || v.t==ColType::FLOAT32)) {
                        after[idx] = Value::Chr(v.t==ColType::INT32 ? std::to_string(v.i) : std::to_string(v.f));
                    } else {
                        throw std::runtime_error("Tipo incompatible en UPDATE para columna: " + col);
                    }
                } else {
                    after[idx] = v;
                }
            }

            // Persistir
            tbl.WriteRowInDisk(pid, after);
            // Refrescar índices abiertos solo de columnas afectadas
            actualizar_fila_en_indices(nt, pid, setlist, before, after);

            ++count;
        }
        return count;
    }

private:
    fs::path root;
    bool abierta = false;

    std::unordered_map<std::string, TablaInfo> tablas;

    void asegurar_abierta() const {
        if (!abierta) throw std::runtime_error("No hay base de datos abierta");
    }

    TablaInfo& obtener_tabla(const std::string& nombre_tabla) {
        asegurar_abierta();
        auto it = tablas.find(nombre_tabla);
        if (it == tablas.end()) {
            // Intentar abrir si no está en el mapa (sesión nueva)
            abrir_tabla(nombre_tabla);
            it = tablas.find(nombre_tabla);
            if (it == tablas.end()) throw std::runtime_error("Tabla no abierta: " + nombre_tabla);
        }
        return it->second;
    }

    ColType detectar_tipo_columna(GenericFixedTable& tbl, TablaInfo& ti, const std::string& col) {
        // Si ya lo conocemos:
        auto it = ti.col_tipos.find(col);
        if (it != ti.col_tipos.end()) return it->second;

        long n = tbl.Count();
        if (n > 0) {
            try { (void)tbl.ReadChar(0, col); ti.col_tipos[col]=ColType::CHAR; return ColType::CHAR; } catch (...) {}
            try { (void)tbl.ReadInt(0,  col); ti.col_tipos[col]=ColType::INT32; return ColType::INT32; } catch (...) {}
            try { (void)tbl.ReadFloat(0,col); ti.col_tipos[col]=ColType::FLOAT32; return ColType::FLOAT32; } catch (...) {}
        }
        throw std::runtime_error("No se puede inferir tipo de columna (tabla vacía o columna inexistente): " + col);
    }

    // Obtención de índices (lanza si no existen)
    diskbtree::BTreeInt* obtener_indice_int(const std::string& nt, const std::string& col) {
        TablaInfo& ti = obtener_tabla(nt);
        auto it = ti.idx_int.find(col);
        if (it == ti.idx_int.end()) throw std::runtime_error("No existe índice INT en " + nt + "." + col);
        return it->second.get();
    }
    diskbtree::BTreeFloat* obtener_indice_float(const std::string& nt, const std::string& col) {
        TablaInfo& ti = obtener_tabla(nt);
        auto it = ti.idx_float.find(col);
        if (it == ti.idx_float.end()) throw std::runtime_error("No existe índice FLOAT en " + nt + "." + col);
        return it->second.get();
    }
    diskbtree::BTreeChar32* obtener_indice_char(const std::string& nt, const std::string& col) {
        TablaInfo& ti = obtener_tabla(nt);
        auto it = ti.idx_char.find(col);
        if (it == ti.idx_char.end()) throw std::runtime_error("No existe índice CHAR en " + nt + "." + col);
        return it->second.get();
    }
};

} // namespace minidb
