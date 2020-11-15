#pragma once
#include <qstring.h>
#include <qsortfilterproxymodel.h>

// Converts a value in bytes to a string representation (B, Kb, Mb, Gb)
QString size_to_string(double value);

// We want to use QSortFilterProxyModel's filtering ability, but our custom sorting
class filter_proxy_model : public QSortFilterProxyModel
{
public:
	filter_proxy_model()
	{
		setDynamicSortFilter(false);
	}

	void sort(int column, Qt::SortOrder order)
	{
		sourceModel()->sort(column, order);
		invalidate();
	}
};
