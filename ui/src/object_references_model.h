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

        // If node already was expanded once and doesn't need to add its children
        bool was_expanded = false;

        // All addresses that share this node
        std::vector<uint64_t> addresses;
        // Type of object
        std::string type;
        // Parent of object
        tree_node_t* parent;

        // Children of object
        std::vector<std::shared_ptr<tree_node_t>> children;        
    };    

    // Saved list of references
    std::unordered_map<uint64_t, owlcat::object_references_t> m_references;

    // Root objects (objects for which we searched)
    std::vector< std::shared_ptr<tree_node_t>> m_roots;

    void add_node(const std::unordered_map<uint64_t, owlcat::object_references_t>& references, const owlcat::object_references_t& ref, tree_node_t* parent, int depth);
    void clear();

    Q_OBJECT
public slots:
    void update(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> references);
    void expand(QModelIndex index);

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& index) const override;
    int columnCount(const QModelIndex& index) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    std::vector<uint64_t> get_addresses(const QModelIndex& index) const;
};
