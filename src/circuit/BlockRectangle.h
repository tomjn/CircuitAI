/*
 * BlockRectangle.h
 *
 *  Created on: Dec 11, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_BLOCKRECTANGLE_H_
#define SRC_CIRCUIT_BLOCKRECTANGLE_H_

#include "BlockMask.h"

namespace circuit {

class CBlockRectangle: public virtual IBlockMask {
public:
	// bsize - block size, ssize - struct size
	CBlockRectangle(const int2& offset, const int2& bsize, const int2& ssize);
	virtual ~CBlockRectangle();
};

} // namespace circuit

#endif // SRC_CIRCUIT_BLOCKRECTANGLE_H_
