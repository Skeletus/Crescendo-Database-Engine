// GenericFixedTable.h
#pragma once
#include <cstdint>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace gft {

namespace fs = std::filesystem;

// Tipos de columna
enum class ColType : int32_t { INT32=1, FLOAT32=2, CHAR=3 };

// Definición de columna (estilo "create table", pero via API)
struct ColumnDef {
    std::string name;   // nombre de columna
    ColType     type;   // INT32, FLOAT32 o CHAR
    int32_t     char_len = 0; // requerido si type==CHAR (ancho fijo)
};

// Valor genérico para IO
struct Value {
    ColType     t;
    int32_t     i{};
    float       f{};
    std::string s;
    static Value Int(int32_t v){ Value x; x.t=ColType::INT32;  x.i=v; return x; }
    static Value Flt(float v){  Value x; x.t=ColType::FLOAT32; x.f=v; return x; }
    static Value Chr(const std::string& v){ Value x; x.t=ColType::CHAR; x.s=v; return x; }
};

#pragma pack(push,1)
struct FileHeader {
    char     magic[8];      // "GFTABv1"
    char     table_name[32];
    int32_t  ncols;
    int32_t  row_size;      // bytes por registro
    int32_t  reserved;
};
#pragma pack(pop)

#pragma pack(push,1)
struct ColMetaDisk {
    char     name[32];
    int32_t  type;     // ColType
    int32_t  width;    // 4 (int), 4 (float), N (char)
    int32_t  offset;   // offset dentro del registro
};
#pragma pack(pop)

class GenericFixedTable {
public:
    // Crear tabla NUEVA con esquema o abrir existente
    GenericFixedTable(const std::string& path,
                      const std::string& table_name,
                      const std::vector<ColumnDef>& cols,
                      bool create_new)
        : filename(path)
    {
        std::ios::openmode mode = std::ios::binary | std::ios::in | std::ios::out;
        if (create_new) mode |= std::ios::trunc;

        file.open(filename, mode);
        if (!file.is_open()) {
            if (!create_new) {
                file.clear();
                file.open(filename, std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc);
            }
        }
        if (!file.is_open()) throw std::runtime_error("No se pudo abrir/crear: " + filename);

        // abrir/crear archivo .del (tombstones)
        del_filename = filename + ".del";
        std::ios::openmode dmode = std::ios::binary | std::ios::in | std::ios::out;
        if (create_new) dmode |= std::ios::trunc;
        del.open(del_filename, dmode);
        if (!del.is_open()) {
            // si no existía, créalo
            del.clear();
            del.open(del_filename, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        }
        if (!del.is_open()) throw std::runtime_error("No se pudo abrir/crear: " + del_filename);

        if (create_new) init_schema(table_name, cols);
        else            load_schema();
    }

    ~GenericFixedTable(){
        if (file.is_open()) file.close();
        if (del.is_open())  del.close();
    }

    // ----------- API estilo UserTable (por pageID) -----------
    long AppendRow(const std::vector<Value>& row) {
        const long pid = Count();
        WriteRowInDisk(pid, row);        // escribe fila
        ensure_del_size(pid+1);          // asegura flag
        set_del_flag(pid, 0);            // nueva fila "viva"
        return pid;
    }

    void WriteRowInDisk(long pageID, const std::vector<Value>& row) {
        ensure_open();
        if ((int)row.size()!=hdr.ncols) throw std::invalid_argument("row.size != ncols");
        std::vector<char> buf(hdr.row_size, 0);
        pack_row(row, buf.data());
        const auto off = data_offset() + std::streampos(pageID) * std::streampos(hdr.row_size);
        file.clear();
        file.seekp(off, std::ios::beg);
        file.write(buf.data(), hdr.row_size);
        file.flush();
        if (!file.good()) std::cerr << "Error al escribir pageID="<<pageID<<"\n";

        // Mantener .del en coherencia
        ensure_del_size(pageID+1);
        set_del_flag(pageID, 0);
    }

    // Lee fila completa a vector<Value>; retorna false si no existe o está borrada
    bool ReadRowByPageID(const long& pageID, std::vector<Value>& out) {
        ensure_open();
        if (pageID < 0 || pageID >= Count()) return false;
        if (IsDeleted(pageID)) return false;

        std::vector<char> buf(hdr.row_size, 0);
        const auto off = data_offset() + std::streampos(pageID) * std::streampos(hdr.row_size);
        file.clear();
        file.seekg(off, std::ios::beg);
        file.read(buf.data(), hdr.row_size);
        if (!file.good()) { std::cerr << "Error al leer pageID="<<pageID<<"\n"; return false; }
        unpack_row(buf.data(), out);
        return true;
    }

    // Cantidad de registros físicos (incluye borrados)
    long Count() {
        ensure_open();
        file.clear();
        auto posg = file.tellg();
        file.seekg(0, std::ios::end);
        auto end = file.tellg();
        file.seekg(posg, std::ios::beg);
        if (end < data_offset()) return 0;
        auto used = end - data_offset();
        return static_cast<long>(used / std::streampos(hdr.row_size));
    }

    // ----------- Helpers por nombre de columna -----------
    int col_index(const std::string& name) const {
        for (int i=0;i<hdr.ncols;++i) {
            if (name == std::string(cols[i].name, strnlen(cols[i].name,32))) return i;
        }
        return -1;
    }

    int32_t ReadInt(long pid, const std::string& colname) {
        int idx = col_index(colname); check_idx(idx, ColType::INT32);
        std::vector<Value> row;
        if (!ReadRowByPageID(pid, row)) throw std::runtime_error("Fila no disponible (borrada o inexistente)");
        return row[idx].i;
    }
    float ReadFloat(long pid, const std::string& colname) {
        int idx = col_index(colname); check_idx(idx, ColType::FLOAT32);
        std::vector<Value> row;
        if (!ReadRowByPageID(pid, row)) throw std::runtime_error("Fila no disponible (borrada o inexistente)");
        return row[idx].f;
    }
    std::string ReadChar(long pid, const std::string& colname) {
        int idx = col_index(colname); check_idx(idx, ColType::CHAR);
        std::vector<Value> row;
        if (!ReadRowByPageID(pid, row)) throw std::runtime_error("Fila no disponible (borrada o inexistente)");
        return row[idx].s;
    }

    // ----------- Tombstones (borrado lógico) -----------
    bool IsDeleted(long pageID) {
        ensure_del_size(pageID+1);
        del.clear();
        del.seekg(std::streampos(pageID), std::ios::beg);
        char b=0; del.read(&b, 1);
        if (!del.good()) return false;
        return b!=0;
    }
    void MarkDeleted(long pageID) {
        ensure_del_size(pageID+1);
        set_del_flag(pageID, 1);
    }

    // Metadatos
    int ncols() const { return hdr.ncols; }
    int row_size() const { return hdr.row_size; }
    std::string table_name() const { return std::string(hdr.table_name, strnlen(hdr.table_name,32)); }

private:
    std::string  filename;
    std::fstream file;

    std::string  del_filename;
    std::fstream del;

    FileHeader   hdr{};
    std::vector<ColMetaDisk> cols; // tamaño = ncols

    // ---------- schema ----------
    void init_schema(const std::string& tname, const std::vector<ColumnDef>& def) {
        if (def.empty()) throw std::invalid_argument("Se requiere al menos 1 columna");
        if (def.size() > 64) throw std::invalid_argument("Máx 64 columnas");

        cols.clear(); cols.resize(def.size());
        int offset = 0;
        for (size_t i=0;i<def.size();++i) {
            ColMetaDisk c{}; std::memset(c.name,0,32);
            std::strncpy(c.name, def[i].name.c_str(), 31);
            c.type = (int32_t)def[i].type;

            if (def[i].type == ColType::INT32) c.width = 4;
            else if (def[i].type == ColType::FLOAT32) c.width = 4;
            else if (def[i].type == ColType::CHAR) {
                if (def[i].char_len <= 0) throw std::invalid_argument("CHAR requiere char_len > 0");
                c.width = def[i].char_len;
            } else throw std::invalid_argument("Tipo no soportado");

            c.offset = offset;
            offset += c.width;
            cols[i] = c;
        }

        // header
        std::memset(hdr.magic, 0, 8);
        std::memcpy(hdr.magic, "GFTABv1", 7);
        std::memset(hdr.table_name, 0, 32);
        std::strncpy(hdr.table_name, tname.c_str(), 31);
        hdr.ncols    = (int32_t)def.size();
        hdr.row_size = offset;
        hdr.reserved = 0;

        // persistir schema
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        for (auto& c : cols) file.write(reinterpret_cast<const char*>(&c), sizeof(c));
        file.flush();
        if (!file.good()) throw std::runtime_error("Error al escribir schema");

        // archivo .del arranca vacío
        del.flush();
    }

    void load_schema() {
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!file.good()) throw std::runtime_error("Error al leer header");
        if (std::memcmp(hdr.magic, "GFTABv1", 7) != 0) throw std::runtime_error("Archivo no es GFTABv1");

        cols.resize(hdr.ncols);
        for (int i=0;i<hdr.ncols;++i) {
            file.read(reinterpret_cast<char*>(&cols[i]), sizeof(ColMetaDisk));
        }
        if (!file.good()) throw std::runtime_error("Error al leer columnas");
    }

    std::streampos data_offset() const {
        return std::streampos(sizeof(FileHeader) + sizeof(ColMetaDisk)*hdr.ncols);
    }

    void ensure_open(){ if (!file.is_open()) throw std::runtime_error("Archivo no abierto"); }

    void check_idx(int idx, ColType expected) const {
        if (idx < 0 || idx >= hdr.ncols) throw std::invalid_argument("Columna no existe");
        if ((ColType)cols[idx].type != expected) throw std::invalid_argument("Tipo de columna no coincide");
    }

    // ---------- pack/unpack ----------
    void pack_row(const std::vector<Value>& row, char* dst) const {
        for (int i=0;i<hdr.ncols;++i) {
            const auto& c = cols[i];
            const auto& v = row[i];
            char* p = dst + c.offset;
            switch ((ColType)c.type) {
            case ColType::INT32:
                if (v.t!=ColType::INT32) throw std::invalid_argument("Valor no INT32");
                std::memcpy(p, &v.i, 4); break;
            case ColType::FLOAT32:
                if (v.t!=ColType::FLOAT32) throw std::invalid_argument("Valor no FLOAT32");
                std::memcpy(p, &v.f, 4); break;
            case ColType::CHAR: {
                if (v.t!=ColType::CHAR) throw std::invalid_argument("Valor no CHAR");
                std::vector<char> tmp(c.width, 0);
                std::memcpy(tmp.data(), v.s.c_str(), std::min((int)v.s.size(), c.width-1));
                std::memcpy(p, tmp.data(), c.width);
            } break;
            default: throw std::invalid_argument("Tipo no soportado");
            }
        }
    }

    void unpack_row(const char* src, std::vector<Value>& out) const {
        out.clear(); out.resize(hdr.ncols);
        for (int i=0;i<hdr.ncols;++i) {
            const auto& c = cols[i];
            const char* p = src + c.offset;
            switch ((ColType)c.type) {
            case ColType::INT32: {
                int32_t x; std::memcpy(&x, p, 4);
                out[i] = Value::Int(x);
            } break;
            case ColType::FLOAT32: {
                float x; std::memcpy(&x, p, 4);
                out[i] = Value::Flt(x);
            } break;
            case ColType::CHAR: {
                int len=0; while (len<c.width && p[len]!='\0') ++len;
                out[i] = Value::Chr(std::string(p, p+len));
            } break;
            default: throw std::runtime_error("Tipo no soportado");
            }
        }
    }

    // ---------- tombstones helpers ----------
    void ensure_del_size(long needed) {
        del.clear();
        del.seekg(0, std::ios::end);
        auto cur = del.tellg();
        long have = (cur < 0 ? 0 : (long)cur);
        if (have >= needed) return;
        // extender con ceros
        del.seekp(0, std::ios::end);
        std::vector<char> zeros(std::max<long>(needed - have, 4096), 0);
        long remain = needed - have;
        while (remain > 0) {
            long chunk = std::min<long>(remain, (long)zeros.size());
            del.write(zeros.data(), chunk);
            remain -= chunk;
        }
        del.flush();
    }

    void set_del_flag(long pid, uint8_t flag) {
        del.clear();
        del.seekp(std::streampos(pid), std::ios::beg);
        char b = (char)flag;
        del.write(&b, 1);
        del.flush();
    }
};

} // namespace gft
