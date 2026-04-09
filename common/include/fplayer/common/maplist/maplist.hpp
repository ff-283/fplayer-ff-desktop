/*************************************************
  * 描述: 提取出某个结构体QList中的某个单独字段，组成QList
  *
  * File：maplist.hpp
  * Date：2026/3/1
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_MAPLIST_HPP
#define FPLAYER_DESKETOP_MAPLIST_HPP
#include <QList>

namespace fplayer
{
	template<typename T, typename Func>
	auto mapList(const QList<T>& list, Func func)
	{
		using ResultType = decltype(func(list.first()));
		QList<ResultType> result;
		result.reserve(list.size());

		for (const auto& item: list) result.append(func(item));

		return result;
	}
}

#endif //FPLAYER_DESKETOP_MAPLIST_HPP