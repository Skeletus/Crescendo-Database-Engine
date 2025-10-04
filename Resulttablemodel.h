#pragma once
#include <QAbstractTableModel>
#include <QStringList>
#include <vector>
#include "GenericFixedTable.h" // gft::Value, gft::ColType
#include "MiniDBSQL.h"         // sqlmini::TableSchema

class ResultTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ResultTableModel(QObject* parent=nullptr);

    void setData(const QStringList& headers,
                 const std::vector<std::vector<gft::Value>>& rows,
                 const sqlmini::TableSchema& schema_for_types);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QStringList headers_;
    std::vector<std::vector<gft::Value>> rows_;
    std::vector<gft::ColType> types_;
};
