#include "Resulttablemodel.h"

using namespace gft;

ResultTableModel::ResultTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void ResultTableModel::setData(const QStringList& headers,
                               const std::vector<std::vector<Value>>& rows,
                               const sqlmini::TableSchema& schema)
{
    beginResetModel();
    headers_ = headers;
    rows_ = rows;

    // Derivar tipos desde schema según headers
    types_.clear(); types_.reserve(headers_.size());
    for (const auto& h : headers_) {
        gft::ColType ct = gft::ColType::INT32;
        for (auto& c : schema.cols) {
            if (h.toStdString() == c.name) { ct = c.type; break; }
        }
        types_.push_back(ct);
    }

    endResetModel();
}

int ResultTableModel::rowCount(const QModelIndex&) const { return (int)rows_.size(); }
int ResultTableModel::columnCount(const QModelIndex&) const { return headers_.size(); }

QVariant ResultTableModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || role != Qt::DisplayRole) return {};
    const auto& v = rows_[idx.row()][idx.column()];
    switch (types_[idx.column()]) {
    case ColType::INT32:   return v.i;
    case ColType::FLOAT32: return v.f;
    case ColType::CHAR:    return QString::fromStdString(v.s);
    }
    return {};
}

QVariant ResultTableModel::headerData(int section, Qt::Orientation ori, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (ori == Qt::Horizontal) return headers_.value(section);
    return section + 1;
}
