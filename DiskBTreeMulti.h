                // DiskBTreeMulti.h
                #pragma once
                #include <cstdint>
                #include <cstdio>
                #include <cstring>
                #include <string>
                #include <vector>
                #include <stdexcept>
                #include <algorithm>
                #include <iostream>
                #include <type_traits>

                // =================== CONFIG ===================
                #define MAX_T 128  // grado mínimo máximo; claves por nodo = 2*MAX_T - 1
                // ==============================================

                namespace diskbtree {

                // --------- Rasgos de clave (traits) ---------

                // INT
                struct KeyInt {
                    using Key = int32_t;
                    static constexpr const char* MAGIC() { return "BTi\1\0\0\0"; } // 8 bytes
                    static constexpr int KEY_BYTES = 4;
                    static void put(void* dst, const Key& k) { std::memcpy(dst, &k, 4); }
                    static void get(const void* src, Key& k) { std::memcpy(&k, src, 4); }
                    static int  cmp_mem(const void* a, const void* b) {
                        int32_t ka, kb; std::memcpy(&ka,a,4); std::memcpy(&kb,b,4);
                        if (ka<kb) return -1; if (ka>kb) return 1; return 0;
                    }
                    static std::string to_string(const void* a){
                        int32_t k; std::memcpy(&k,a,4); return std::to_string(k);
                    }
                };

                // FLOAT
                struct KeyFloat {
                    using Key = float;
                    static constexpr const char* MAGIC() { return "BTf\1\0\0\0"; }
                    static constexpr int KEY_BYTES = 4;
                    static void put(void* dst, const Key& k) { std::memcpy(dst, &k, 4); }
                    static void get(const void* src, Key& k) { std::memcpy(&k, src, 4); }
                    static int  cmp_mem(const void* a, const void* b) {
                        float ka, kb; std::memcpy(&ka,a,4); std::memcpy(&kb,b,4);
                        if (ka<kb) return -1; if (ka>kb) return 1; return 0;
                    }
                    static std::string to_string(const void* a){
                        float k; std::memcpy(&k,a,4); char buf[64]; std::snprintf(buf,64,"%.4f",k); return buf;
                    }
                };

                // CHAR[32] (string fija, lexicográfica binaria)
                struct KeyChar32 {
                    using Key = std::string; // al insertar/consultar, usamos std::string (se trunca/pad)
                    static constexpr const char* MAGIC() { return "BTs\1\0\0\0"; }
                    static constexpr int KEY_BYTES = 32;
                    static void put(void* dst, const Key& s) {
                        char tmp[KEY_BYTES]; std::memset(tmp, 0, KEY_BYTES);
                        std::memcpy(tmp, s.c_str(), std::min((int)s.size(), KEY_BYTES-1));
                        std::memcpy(dst, tmp, KEY_BYTES);
                    }
                    static void get(const void* src, Key& out) {
                        const char* p = (const char*)src;
                        out.assign(p, p + strnlen(p, KEY_BYTES));
                    }
                    static int  cmp_mem(const void* a, const void* b) {
                        return std::memcmp(a, b, KEY_BYTES);
                    }
                    static std::string to_string(const void* a){
                        const char* p = (const char*)a;
                        return std::string(p, p + strnlen(p, KEY_BYTES));
                    }
                };

                // --------- Infraestructura común ---------
                static constexpr int MAX_KEYS  = 2 * MAX_T - 1;
                static constexpr int MAX_CHILD = 2 * MAX_T;

                #pragma pack(push,1)
                struct FileHeader {
                    char     magic[8];       // depende del tipo
                    int32_t  t;              // grado mínimo
                    uint64_t root_off;       // offset de la raíz (0 == none)
                    uint64_t node_size;      // tamaño fijo del nodo (bytes)
                    int32_t  key_bytes;      // tamaño del campo clave por entrada
                };
                #pragma pack(pop)

                class Pager {
                    std::FILE* f = nullptr;
                public:
                    Pager() = default;
                    ~Pager(){ if (f) std::fclose(f); }
                    void open(const std::string& path, bool create) {
                        if (create) { f = std::fopen(path.c_str(), "wb+"); }
                        else        { f = std::fopen(path.c_str(), "rb+"); }
                        if (!f) throw std::runtime_error("No se pudo abrir archivo: " + path);
                        std::setvbuf(f, nullptr, _IOFBF, 1<<20);
                    }
                    uint64_t size() {
                        std::fflush(f);
                        auto cur = std::ftell(f);
                        std::fseek(f, 0, SEEK_END);
                        long long sz = std::ftell(f);
                        std::fseek(f, cur, SEEK_SET);
                        return (uint64_t)sz;
                    }
                    void read_bytes(uint64_t off, void* buf, size_t len) {
                        if (std::fseek(f, (long)off, SEEK_SET) != 0) throw std::runtime_error("seek read");
                        if (std::fread(buf, 1, len, f) != len) throw std::runtime_error("read bytes");
                    }
                    void write_bytes(uint64_t off, const void* buf, size_t len) {
                        if (std::fseek(f, (long)off, SEEK_SET) != 0) throw std::runtime_error("seek write");
                        if (std::fwrite(buf, 1, len, f) != len) throw std::runtime_error("write bytes");
                    }
                    void flush(){ std::fflush(f); }
                };

                // Nodo genérico: las claves se guardan como bytes de longitud fija TRAITS::KEY_BYTES
                #pragma pack(push,1)
                template<int KEY_BYTES>
                struct NodeDiskGeneric {
                    uint8_t  isLeaf;
                    int16_t  n;
                    uint8_t  keys[MAX_KEYS][KEY_BYTES]; // claves en crudo
                    int32_t  pages[MAX_KEYS];           // values
                    uint64_t children[MAX_CHILD];
                    uint32_t _reserved;
                };
                #pragma pack(pop)

                // B-Tree genérico parametrizado por TRAITS
                template<class TRAITS>
                class DiskBTree {
                    using Key = typename TRAITS::Key;
                    static constexpr int KBYTES = TRAITS::KEY_BYTES;
                    using NodeDisk = NodeDiskGeneric<KBYTES>;

                public:
                    explicit DiskBTree(const std::string& path, int t, bool create_new = true) {
                        if (t < 2) throw std::invalid_argument("t debe ser >= 2");
                        if (t > MAX_T) throw std::invalid_argument("t excede MAX_T compilado");
                        node_size = sizeof(NodeDisk);
                        if (create_new) {
                            pager.open(path, /*create*/true);
                            FileHeader hdr{};
                            std::memset(hdr.magic, 0, 8);
                            std::memcpy(hdr.magic, TRAITS::MAGIC(), 8);
                            hdr.t = t;
                            hdr.root_off = 0;
                            hdr.node_size = node_size;
                            hdr.key_bytes = KBYTES;
                            pager.write_bytes(0, &hdr, sizeof(hdr));
                            pager.flush();
                            header = hdr;
                        } else {
                            pager.open(path, /*create*/false);
                            FileHeader hdr{};
                            pager.read_bytes(0, &hdr, sizeof(hdr));
                            if (std::memcmp(hdr.magic, TRAITS::MAGIC(), 8) != 0) {
                                throw std::runtime_error("Tipo/magic incompatible con este índice");
                            }
                            if ((uint64_t)hdr.node_size != sizeof(NodeDisk) || hdr.key_bytes != KBYTES) {
                                throw std::runtime_error("node_size/key_bytes incompatible");
                            }
                            if (hdr.t > MAX_T) throw std::runtime_error("t del archivo excede MAX_T");
                            header = hdr;
                        }
                    }
                    ~DiskBTree(){ sync_header(); pager.flush(); }

                    int  T() const { return header.t; }
                    bool empty() const { return header.root_off == 0; }

                    // Insertar (key,value)
                    void insert(const Key& key, int value) {
                        if (header.root_off == 0) {
                            uint64_t r = alloc_node();
                            NodeDisk R{}; R.isLeaf=1; R.n=1;
                            TRAITS::put(R.keys[0], key);
                            R.pages[0] = value;
                            std::fill(std::begin(R.children), std::end(R.children), 0ULL);
                            write_node(r, R);
                            header.root_off = r; sync_header();
                            return;
                        }
                        NodeDisk root = read_node(header.root_off);
                        if (root.n == 2*T()-1) {
                            uint64_t s_off = alloc_node();
                            NodeDisk S{}; S.isLeaf=0; S.n=0;
                            std::fill(std::begin(S.children), std::end(S.children), 0ULL);
                            S.children[0] = header.root_off; write_node(s_off, S);
                            split_child(s_off, 0, header.root_off);
                            S = read_node(s_off);
                            int i = 0;
                            if (TRAITS::cmp_mem(S.keys[0], key_as_bytes(key)) < 0) i = 1;
                            insert_non_full(S.children[i], key, value);
                            header.root_off = s_off; sync_header();
                        } else {
                            insert_non_full(header.root_off, key, value);
                        }
                    }

                    // Búsqueda exacta (retorna un value cualquiera si hay duplicados). -1 si no existe.
                    int search_get_value(const Key& k) const {
                        if (header.root_off == 0) return -1;
                        return search_rec(header.root_off, k);
                    }

                    // Rango: devuelve las CLAVES (útil para debug)
                    std::vector<Key> range_search_keys(const Key& a_in, const Key& b_in) const {
                        Key a=a_in, b=b_in;
                        if (cmp_key(a,b)>0) std::swap(a,b);
                        std::vector<Key> out;
                        if (header.root_off==0) return out;
                        range_rec_keys(header.root_off, a, b, out);
                        return out;
                    }

                    // Rango: devuelve los VALUES (pageID) de cada entrada en [a,b] (incluye duplicados)
                    std::vector<int> range_search_values(const Key& a_in, const Key& b_in) const {
                        Key a=a_in, b=b_in;
                        if (cmp_key(a,b)>0) std::swap(a,b);
                        std::vector<int> out;
                        if (header.root_off==0) return out;
                        range_rec_values(header.root_off, a, b, out);
                        return out;
                    }

                    // Borrado de una ocurrencia de la clave
                    void remove_key(const Key& k) {
                        if (header.root_off==0) return;
                        remove_rec(header.root_off, k);
                        NodeDisk root = read_node(header.root_off);
                        if (root.n==0) {
                            header.root_off = root.isLeaf ? 0 : root.children[0];
                            sync_header();
                        }
                    }

                    // Debug
                    void traverse_print() const {
                        if (header.root_off==0) return;
                        traverse_rec(header.root_off, 0, 0);
                    }

                    // --- (expuesto para demos avanzadas si lo necesitas) ---
                    uint64_t root_offset() const { return header.root_off; }

                private:
                    Pager pager;
                    FileHeader header{};
                    uint64_t node_size = 0;

                    // Helpers de clave
                    static int cmp_key(const Key& a, const Key& b) {
                        uint8_t A[KBYTES], B[KBYTES];
                        TRAITS::put(A,a); TRAITS::put(B,b);
                        return TRAITS::cmp_mem(A,B);
                    }
                    static const void* key_as_bytes(const Key& a) {
                        // solo para comparaciones puntuales (temporal)
                        static thread_local uint8_t buf[KBYTES];
                        TRAITS::put(buf, a); return buf;
                    }

                    // IO nodos
                    uint64_t header_size() const { return sizeof(FileHeader); }
                    void sync_header(){ pager.write_bytes(0, &header, sizeof(header)); }
                    uint64_t alloc_node() {
                        uint64_t off = pager.size();
                        if (off < header_size()) off = header_size();
                        NodeDisk blank{}; std::memset(&blank,0,sizeof(blank));
                        pager.write_bytes(off, &blank, sizeof(blank));
                        return off;
                    }
                    NodeDisk read_node(uint64_t off) const {
                        NodeDisk n{}; const_cast<Pager&>(pager).read_bytes(off, &n, sizeof(n)); return n;
                    }
                    void write_node(uint64_t off, const NodeDisk& n){ pager.write_bytes(off, &n, sizeof(n)); }

                    // ---------- SEARCH ----------
                    int search_rec(uint64_t x_off, const Key& k) const {
                        NodeDisk x = read_node(x_off);
                        int i=0;
                        while (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(k)) < 0) ++i;
                        if (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(k)) == 0) return x.pages[i];
                        if (x.isLeaf) return -1;
                        return search_rec(x.children[i], k);
                    }

                    // ---------- INSERT ----------
                    void insert_non_full(uint64_t x_off, const Key& k, int value) {
                        NodeDisk x = read_node(x_off);
                        int i = x.n - 1;
                        if (x.isLeaf) {
                            while (i>=0 && TRAITS::cmp_mem(x.keys[i], key_as_bytes(k)) > 0) {
                                std::memcpy(x.keys[i+1], x.keys[i], KBYTES);
                                x.pages[i+1] = x.pages[i];
                                --i;
                            }
                            TRAITS::put(x.keys[i+1], k);
                            x.pages[i+1] = value;
                            x.n++; write_node(x_off, x);
                        } else {
                            while (i>=0 && TRAITS::cmp_mem(x.keys[i], key_as_bytes(k)) > 0) --i;
                            ++i;
                            NodeDisk child = read_node(x.children[i]);
                            if (child.n == 2*T()-1) {
                                split_child(x_off, i, x.children[i]);
                                x = read_node(x_off);
                                if (TRAITS::cmp_mem(x.keys[i], key_as_bytes(k)) < 0) ++i;
                            }
                            insert_non_full(x.children[i], k, value);
                        }
                    }

                    void split_child(uint64_t x_off, int i, uint64_t y_off) {
                        NodeDisk x = read_node(x_off);
                        NodeDisk y = read_node(y_off);

                        uint64_t z_off = alloc_node();
                        NodeDisk z{}; z.isLeaf = y.isLeaf; z.n = T()-1;

                        for (int j=0;j<T()-1;++j) {
                            std::memcpy(z.keys[j], y.keys[j+T()], KBYTES);
                            z.pages[j] = y.pages[j+T()];
                        }
                        if (!y.isLeaf) {
                            for (int j=0;j<T();++j) z.children[j] = y.children[j+T()];
                        }
                        y.n = T()-1;

                        for (int j=x.n;j>=i+1;--j) x.children[j+1] = x.children[j];
                        x.children[i+1] = z_off;

                        for (int j=x.n-1;j>=i;--j) {
                            std::memcpy(x.keys[j+1], x.keys[j], KBYTES);
                            x.pages[j+1] = x.pages[j];
                        }
                        std::memcpy(x.keys[i], y.keys[T()-1], KBYTES);
                        x.pages[i] = y.pages[T()-1];
                        x.n++;

                        write_node(y_off, y);
                        write_node(z_off, z);
                        write_node(x_off, x);
                    }

                    // ---------- RANGE: KEYS ----------
                    void range_rec_keys(uint64_t x_off, const Key& a, const Key& b, std::vector<Key>& out) const {
                        NodeDisk x = read_node(x_off);
                        if (x.isLeaf) {
                            for (int i=0;i<x.n;++i) {
                                if (TRAITS::cmp_mem(x.keys[i], key_as_bytes(a)) < 0) continue;
                                if (TRAITS::cmp_mem(x.keys[i], key_as_bytes(b)) > 0) break;
                                Key k{}; TRAITS::get(x.keys[i], k); out.emplace_back(k);
                            }
                            return;
                        }
                        int i=0;
                        while (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(a)) < 0) {
                            range_rec_keys(x.children[i], a, b, out); ++i;
                        }
                        if (i<x.n) range_rec_keys(x.children[i], a, b, out);
                        while (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(b)) <= 0) {
                            Key k{}; TRAITS::get(x.keys[i], k); out.emplace_back(k);
                            range_rec_keys(x.children[i+1], a, b, out); ++i;
                        }
                    }

                    // ---------- RANGE: VALUES ----------
                    void range_rec_values(uint64_t x_off, const Key& a, const Key& b, std::vector<int>& out) const {
                        NodeDisk x = read_node(x_off);
                        if (x.isLeaf) {
                            for (int i=0;i<x.n;++i) {
                                if (TRAITS::cmp_mem(x.keys[i], key_as_bytes(a)) < 0) continue;
                                if (TRAITS::cmp_mem(x.keys[i], key_as_bytes(b)) > 0) break;
                                out.emplace_back(x.pages[i]);
                            }
                            return;
                        }
                        int i=0;
                        while (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(a)) < 0) {
                            range_rec_values(x.children[i], a, b, out); ++i;
                        }
                        if (i<x.n) range_rec_values(x.children[i], a, b, out);
                        while (i<x.n && TRAITS::cmp_mem(x.keys[i], key_as_bytes(b)) <= 0) {
                            out.emplace_back(x.pages[i]);
                            range_rec_values(x.children[i+1], a, b, out); ++i;
                        }
                    }

                    // ---------- DELETE ----------
                    void remove_rec(uint64_t x_off, const Key& k) {
                        NodeDisk x = read_node(x_off);
                        int idx=0;
                        while (idx<x.n && TRAITS::cmp_mem(x.keys[idx], key_as_bytes(k)) < 0) ++idx;

                        if (idx<x.n && TRAITS::cmp_mem(x.keys[idx], key_as_bytes(k)) == 0) {
                            if (x.isLeaf) { remove_from_leaf(x_off, x, idx); }
                            else          { remove_from_non_leaf(x_off, x, idx); }
                        } else {
                            if (x.isLeaf) return;
                            bool flag = (idx == x.n);
                            NodeDisk child = read_node(x.children[idx]);
                            if (child.n < T()) { fill(x_off, x, idx); x = read_node(x_off); }
                            if (flag && idx > x.n) { uint64_t ch = read_node(x_off).children[idx-1]; remove_rec(ch, k); }
                            else { uint64_t ch = read_node(x_off).children[idx]; remove_rec(ch, k); }
                        }
                    }

                    void remove_from_leaf(uint64_t x_off, NodeDisk x, int idx) {
                        for (int i=idx+1;i<x.n;++i) {
                            std::memcpy(x.keys[i-1], x.keys[i], KBYTES);
                            x.pages[i-1] = x.pages[i];
                        }
                        x.n--; write_node(x_off, x);
                    }

                    std::pair<Key,int> get_predecessor(uint64_t child_off) {
                        NodeDisk cur = read_node(child_off);
                        while (!cur.isLeaf) { child_off = cur.children[cur.n]; cur = read_node(child_off); }
                        Key k{}; TRAITS::get(cur.keys[cur.n-1], k); return {k, cur.pages[cur.n-1]};
                    }
                    std::pair<Key,int> get_successor(uint64_t child_off) {
                        NodeDisk cur = read_node(child_off);
                        while (!cur.isLeaf) { child_off = cur.children[0]; cur = read_node(child_off); }
                        Key k{}; TRAITS::get(cur.keys[0], k); return {k, cur.pages[0]};
                    }

                    void remove_from_non_leaf(uint64_t x_off, NodeDisk x, int idx) {
                        uint64_t y_off = x.children[idx];
                        uint64_t z_off = x.children[idx+1];
                        NodeDisk y = read_node(y_off);
                        NodeDisk z = read_node(z_off);

                        if (y.n >= T()) {
                            auto pred = get_predecessor(y_off);
                            TRAITS::put(x.keys[idx], pred.first);
                            x.pages[idx] = pred.second; write_node(x_off, x);
                            remove_rec(y_off, pred.first);
                        } else if (z.n >= T()) {
                            auto succ = get_successor(z_off);
                            TRAITS::put(x.keys[idx], succ.first);
                            x.pages[idx] = succ.second; write_node(x_off, x);
                            remove_rec(z_off, succ.first);
                        } else {
                            merge(x_off, x, idx);
                            uint64_t merged_off = read_node(x_off).children[idx];
                            Key k{}; TRAITS::get(x.keys[idx], k);
                            remove_rec(merged_off, k);
                        }
                    }

                    void fill(uint64_t x_off, NodeDisk x, int idx) {
                        if (idx != 0) {
                            NodeDisk left = read_node(x.children[idx-1]);
                            if (left.n >= T()) { borrow_from_prev(x_off, x, idx); return; }
                        }
                        if (idx != x.n) {
                            NodeDisk right = read_node(x.children[idx+1]);
                            if (right.n >= T()) { borrow_from_next(x_off, x, idx); return; }
                        }
                        if (idx != x.n) merge(x_off, x, idx);
                        else merge(x_off, x, idx-1);
                    }

                    void borrow_from_prev(uint64_t x_off, NodeDisk x, int idx) {
                        uint64_t child_off = x.children[idx];
                        uint64_t sib_off   = x.children[idx-1];
                        NodeDisk child = read_node(child_off);
                        NodeDisk sib   = read_node(sib_off);

                        for (int i=child.n-1;i>=0;--i) {
                            std::memcpy(child.keys[i+1], child.keys[i], KBYTES);
                            child.pages[i+1] = child.pages[i];
                        }
                        if (!child.isLeaf) {
                            for (int i=child.n;i>=0;--i) child.children[i+1] = child.children[i];
                        }
                        std::memcpy(child.keys[0], x.keys[idx-1], KBYTES);
                        child.pages[0] = x.pages[idx-1];
                        if (!child.isLeaf) child.children[0] = sib.children[sib.n];

                        std::memcpy(x.keys[idx-1], sib.keys[sib.n-1], KBYTES);
                        x.pages[idx-1] = sib.pages[sib.n-1];

                        child.n += 1; sib.n -= 1;
                        write_node(child_off, child); write_node(sib_off, sib); write_node(x_off, x);
                    }

                    void borrow_from_next(uint64_t x_off, NodeDisk x, int idx) {
                        uint64_t child_off = x.children[idx];
                        uint64_t sib_off   = x.children[idx+1];
                        NodeDisk child = read_node(child_off);
                        NodeDisk sib   = read_node(sib_off);

                        std::memcpy(child.keys[child.n], x.keys[idx], KBYTES);
                        child.pages[child.n] = x.pages[idx];
                        if (!child.isLeaf) child.children[child.n+1] = sib.children[0];

                        std::memcpy(x.keys[idx], sib.keys[0], KBYTES);
                        x.pages[idx] = sib.pages[0];

                        for (int i=1;i<sib.n;++i) {
                            std::memcpy(sib.keys[i-1], sib.keys[i], KBYTES);
                            sib.pages[i-1] = sib.pages[i];
                        }
                        if (!sib.isLeaf) {
                            for (int i=1;i<=sib.n;++i) sib.children[i-1] = sib.children[i];
                        }
                        child.n += 1; sib.n -= 1;

                        write_node(child_off, child); write_node(sib_off, sib); write_node(x_off, x);
                    }

                    void merge(uint64_t x_off, NodeDisk x, int idx) {
                        uint64_t c_off = x.children[idx];
                        uint64_t s_off = x.children[idx+1];
                        NodeDisk c = read_node(c_off);
                        NodeDisk s = read_node(s_off);

                        std::memcpy(c.keys[T()-1], x.keys[idx], KBYTES);
                        c.pages[T()-1] = x.pages[idx];

                        for (int i=0;i<s.n;++i) {
                            std::memcpy(c.keys[i+T()], s.keys[i], KBYTES);
                            c.pages[i+T()] = s.pages[i];
                        }
                        if (!c.isLeaf) {
                            for (int i=0;i<=s.n;++i) c.children[i+T()] = s.children[i];
                        }
                        c.n += s.n + 1;

                        for (int i=idx+1;i<x.n;++i) {
                            std::memcpy(x.keys[i-1], x.keys[i], KBYTES);
                            x.pages[i-1] = x.pages[i];
                        }
                        for (int i=idx+2;i<=x.n;++i) x.children[i-1] = x.children[i];
                        x.n--;

                        write_node(c_off, c);
                        write_node(x_off, x);
                    }

                    void traverse_rec(uint64_t x_off, int level, int index) const {
                        NodeDisk x = read_node(x_off);
                        std::cout << std::string(level*2,' ') << "Nivel " << level
                                  << " (n="<<x.n<<", leaf="<<int(x.isLeaf)<<") keys: ";
                        for (int i=0;i<x.n;++i) {
                            std::cout << TRAITS::to_string(x.keys[i]) << "(" << x.pages[i] << ") ";
                        }
                        std::cout << "\n";
                        if (!x.isLeaf) for (int i=0;i<=x.n;++i) traverse_rec(x.children[i], level+1, i);
                    }
                };

                // Aliases listos para usar
                using BTreeInt    = DiskBTree<KeyInt>;
                using BTreeFloat  = DiskBTree<KeyFloat>;
                using BTreeChar32 = DiskBTree<KeyChar32>;

                } // namespace diskbtree
