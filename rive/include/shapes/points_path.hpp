#ifndef _RIVE_POINTS_PATH_HPP_
#define _RIVE_POINTS_PATH_HPP_
#include "generated/shapes/points_path_base.hpp"
#include <stdio.h>
namespace rive
{
	class PointsPath : public PointsPathBase
	{
	public:
		PointsPath() { printf("Constructing PointsPath\n"); }
	};
} // namespace rive

#endif