#include "object_references_model.h"

void object_references_tree_model::add_node(const std::unordered_map<uint64_t, owlcat::object_references_t>& references, const owlcat::object_references_t& ref, tree_node_t* parent, int depth)
{
	auto& children = parent ? parent->children : m_roots;

	tree_node_t* new_node = nullptr;

	// Group by type
	auto type_iter = std::find_if(children.begin(), children.end(), [&](auto node) {return node->type == ref.type; });
	if (type_iter == children.end())
	{
		children.push_back(std::make_shared<tree_node_t>(ref.address, ref.type, parent));
		new_node = children.back().get();
	}
	else
	{
		(*type_iter)->addresses.push_back(ref.address);
		new_node = (*type_iter).get();
	}

	if (depth == 0)
	{
		for (auto& ref_parent : ref.parents)
		{
			auto ref_iter = references.find(ref_parent.address);
			if (ref_iter == references.end())
			{
				// This should not happen, but does. It means that some object in references was not reported correctly?
				continue;
			}
			add_node(references, ref_iter->second, new_node, depth + 1);
		}
	}
}

void object_references_tree_model::clear()
{
	beginRemoveRows(QModelIndex(), 0, (int)m_roots.size());
	m_roots.clear();
	endRemoveRows();
}

void object_references_tree_model::update(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> references)
{
	for(auto& ref : references)
	{
		m_references.insert(std::make_pair(ref.address, ref));
	}

	beginResetModel();

	m_roots.clear();

	for (auto& root : addresses)
	{
		auto ref_iter = m_references.find(root);//std::find_if(references.begin(), references.end(), [&](auto& r) {return r.address == root; });
		if (ref_iter != m_references.end())
		{
			add_node(m_references, ref_iter->second, nullptr, 0);
		}
	}

	endResetModel();
}

void object_references_tree_model::expand(QModelIndex index)
{
	auto this_node = (tree_node_t*)index.internalPointer();
	if (this_node == nullptr)
		return;

	if (this_node->was_expanded)
		return;
	
	for(int row = 0; row < this_node->children.size(); ++row)
	{
		auto& child = this_node->children[row];		

		if (child->was_expanded)
			continue;

		for (auto& child_addr : child->addresses)
		{
			auto ref_iter = m_references.find(child_addr);
			if (ref_iter == m_references.end())
				continue;

			for (auto& ref_parent : ref_iter->second.parents)
			{
				auto ref_parent_iter = m_references.find(ref_parent.address);
				if (ref_parent_iter != m_references.end())
				{
					add_node(m_references, ref_parent_iter->second, child.get(), 1);
				}
			}
		}		
		beginInsertRows(this->index(row, 0, index), 0, (int)child->children.size());
		endInsertRows();
	}

	this_node->was_expanded = true;
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
	auto this_node = (tree_node_t*)index.internalPointer();
	
	if (role != Qt::DisplayRole)
		return QVariant();

	std::string node_text = this_node->type + std::string(" (") + std::to_string(this_node->addresses.size()) + std::string(")");
	
	return node_text.c_str();
}

std::vector<uint64_t> object_references_tree_model::get_addresses(const QModelIndex& index) const
{
	auto this_node = (tree_node_t*)index.internalPointer();

	if (this_node->addresses.empty())
		return std::vector<uint64_t>();
	else
		return this_node->addresses;
}
