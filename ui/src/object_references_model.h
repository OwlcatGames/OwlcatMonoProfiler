#pragma once
#include <qabstractitemmodel.h>

#include "mono_profiler_client.h"

/*
    Model used by list of references for a list of objects
*/
class object_references_tree_model : public QAbstractItemModel
{
    struct tree_node_t
    {
        tree_node_t(uint64_t _addr, std::string _type, tree_node_t* _parent)
            : type(_type)
            , parent(_parent)
        {
            addresses.push_back(_addr);
        }

        // All addresses that share this node
        std::vector<uint64_t> addresses;
        // Type of object
        std::string type;
        // Parent of object
        tree_node_t* parent;

        // Children of object
        std::vector<std::shared_ptr<tree_node_t>> children;        
    };

    // Root objects (objects for which we searched)
    std::vector< std::shared_ptr<tree_node_t>> m_roots;
    // Total nodes count for sanity checks
    int m_nodes_count = 0;

    void add_node(const std::vector<owlcat::object_references_t>& references, const owlcat::object_references_t& ref, tree_node_t* parent);
    void clear();

    Q_OBJECT
public slots:
    void update(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> references);

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& index) const override;
    int columnCount(const QModelIndex& index) const override;
    QVariant data(const QModelIndex& index, int role) const override;
};
