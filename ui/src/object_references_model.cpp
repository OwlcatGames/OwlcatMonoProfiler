#include "object_references_model.h"

void object_references_tree_model::add_node(const std::vector<owlcat::object_references_t>& references, const owlcat::object_references_t& ref, tree_node_t* parent)
{
	auto& children = parent ? parent->children : m_roots;

	tree_node_t* new_node = nullptr;

	// Group by type
	auto type_iter = std::find_if(children.begin(), children.end(), [&](auto node) {return node->type == ref.type; });
	if (type_iter == children.end())
	{
		++m_nodes_count;
		if (m_nodes_count > 1000)
		{
			children.push_back(std::make_shared<tree_node_t>(0, "...Too much references...", parent));
			return;
		}

		children.push_back(std::make_shared<tree_node_t>(ref.address, ref.type, parent));
		new_node = children.back().get();
	}
	else
	{
		(*type_iter)->addresses.push_back(ref.address);
		new_node = (*type_iter).get();
	}

	// Prevent endless recursion on cyclical references
	if (ref.visited)
		return;

	ref.visited = true;
	for (auto& ref_parent : ref.parents)
	{
		auto ref_iter = std::find_if(references.begin(), references.end(), [&](auto& r) {return r.address == ref_parent.address; });
		if (ref_iter == references.end())
		{
			// This should not happen, but does. It means that some object in references was not reported correctly?
			continue;
		}
		add_node(references , *ref_iter, new_node);
	}
	ref.visited = false;
}

void object_references_tree_model::clear()
{
	beginRemoveRows(QModelIndex(), 0, (int)m_roots.size());
	m_roots.clear();
	endRemoveRows();
}

void object_references_tree_model::update(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> references)
{
	m_nodes_count = 0;

	beginResetModel();

	m_roots.clear();

	for (auto& root : addresses)
	{
		auto ref_iter = std::find_if(references.begin(), references.end(), [&](auto& r) {return r.address == root; });
		if (ref_iter != references.end())
		{
			add_node(references, *ref_iter, nullptr);
		}
	}

	endResetModel();
}

QModelIndex object_references_tree_model::index(int row, int column, const QModelIndex& parent) const
{
	auto parent_node = (tree_node_t*)parent.internalPointer();
	auto& children = parent_node ? parent_node->children : m_roots;
	if (row < 0 || row >= children.size())
		return QModelIndex();
	return createIndex(row, column, children[row].get());
}

QModelIndex object_references_tree_model::parent(const QModelIndex& index) const
{
	auto this_node = (tree_node_t*)index.internalPointer();
	auto parent = this_node->parent;

	if (parent == nullptr)
		return QModelIndex();

	auto& parent_parents = parent->parent ? parent->parent->children : m_roots;
	for (int i = 0; i < parent_parents.size(); ++i)
	{
		if (parent_parents[i]->type == parent->type)
			return createIndex(i , index.column(), parent_parents[i].get());
	}

	return QModelIndex();
}

int object_references_tree_model::rowCount(const QModelIndex& index) const
{	
	auto this_node = (tree_node_t*)index.internalPointer();
	if (this_node == nullptr)
		return (int)m_roots.size();

	return (int)this_node->children.size();
}

int object_references_tree_model::columnCount(const QModelIndex& index) const
{
	return 1;
}

QVariant object_references_tree_model::data(const QModelIndex& index, int role) const
{
	if (role != Qt::DisplayRole)
		return QVariant();

	auto this_node = (tree_node_t*)index.internalPointer();
	std::string node_text = this_node->type + std::string(" (") + std::to_string(this_node->addresses.size()) + std::string(")");
	
	return node_text.c_str();
}
